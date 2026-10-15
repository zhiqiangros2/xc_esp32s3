#include "usb_uart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp32s3_cmd_handle.h"
#include "esp32s3_protocol.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

/*
 * 本文件只负责 TinyUSB CDC-ACM 通道的数据收发和接收缓存管理：
 *
 * 1. CDC 回调把收到的字节快速写入 PSRAM 流缓冲，不在 USB 任务中处理命令；
 * 2. 接收任务取出的数据可能是半帧、完整帧或多帧数据；
 * 3. 接收数据先存入 protocol_frame_buffer，再调用
 *    esp32s3_protocol_unpack() 检查帧头、长度和校验和；
 * 4. 只有完整且校验正确的帧才交给 esp32s3_cmd_handle_frame() 处理；
 * 5. 命令处理完成后，通过 usb_uart_send() 组包并发送应答；
 * 6. 协议格式、解包和组包细节统一放在 esp32s3_protocol.c 中。
 */

/* CDC 回调每次从 TinyUSB RX FIFO 取出的数据大小。 */
#define USB_UART_CDC_READ_BUFFER_SIZE 64U
/* CDC 回调和协议任务之间的流缓冲大小，可保存多个连续协议帧。 */
#define USB_UART_RX_STREAM_SIZE 1024U
/* USB 命令接收任务的栈大小和任务优先级。 */
#define USB_UART_TASK_STACK_SIZE 3072U
#define USB_UART_TASK_PRIORITY 5U
/* 每次等待 USB 接收数据的最长时间，用于周期性检查残包是否超时。 */
#define USB_UART_RX_WAIT_MS 100U
/*
 * 一帧数据开始接收后，超过该时间仍未接收完整，则清空残包。
 * 这样可以避免异常长度或中途断开的数据一直阻塞后续合法命令。
 */
#define USB_UART_PARTIAL_FRAME_TIMEOUT_MS 500U
/* 单次向 USB 驱动写数据时允许等待发送缓冲区的最长时间。 */
#define USB_UART_TX_TIMEOUT_MS 100U

static const char *TAG = "USB_UART";
/* 非空表示 USB 命令接收任务已经创建，防止重复初始化驱动和任务。 */
static TaskHandle_t s_usb_uart_task;
/* CDC 回调是唯一写入方，协议任务是唯一读取方。缓冲区内存位于 PSRAM。 */
static StreamBufferHandle_t s_usb_uart_rx_stream;
/* 回调不能阻塞；流缓冲满时置位，由协议任务清除残帧并输出告警。 */
static volatile bool s_usb_uart_rx_overflow;
/* CDC 回调使用固定静态缓冲，避免在 USB 任务上下文动态分配内存。 */
static uint8_t s_usb_uart_cdc_read_buffer[USB_UART_CDC_READ_BUFFER_SIZE];

/*
 * USB 流式接收缓存。
 *
 * 最大协议帧为：帧头 2 字节 + 长度 1 字节 + CMD/数据 255 字节
 * + 校验和 1 字节，总计 259 字节。
 */
typedef struct {
    uint8_t data[ESP32S3_PROTOCOL_MAX_FRAME_SIZE];
    size_t length;
} usb_uart_frame_buffer_t;

/**
 * @brief 组装并完整发送一帧协议数据。
 *
 * 本函数只接收命令字和有效数据，帧头、长度和校验和由
 * esp32s3_protocol_build_frame() 统一生成。
 *
 * TinyUSB TX FIFO 可能无法一次容纳完整帧，因此这里循环排队并刷新，直到
 * 整帧发送完成或主机未读取数据导致超时。
 *
 * @param command        协议命令字 CMD。
 * @param payload        有效数据地址；payload_length 为 0 时允许为 NULL。
 * @param payload_length 有效数据长度，最大为 254 字节。
 *
 * @return ESP_OK              整帧发送完成。
 * @return ESP_ERR_INVALID_ARG 参数不符合协议要求或组包失败。
 * @return ESP_ERR_TIMEOUT     USB 驱动在规定时间内未继续发送数据。
 */
esp_err_t usb_uart_send(uint8_t command,
                        const uint8_t *payload,
                        size_t payload_length)
{
    if (!tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 先在栈上准备一块能够容纳最大协议帧的发送缓冲区。 */
    uint8_t frame[ESP32S3_PROTOCOL_MAX_FRAME_SIZE];

    /* 根据 CMD 和有效数据生成帧头、长度、数据以及 XOR 校验和。 */
    const size_t frame_length = esp32s3_protocol_build_frame(
        command,
        payload,
        payload_length,
        frame,
        sizeof(frame));

    /* 返回 0 表示参数非法、数据过长或目标缓冲区不足。 */
    if (frame_length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 驱动一次写调用不保证写完全部数据，sent_length 用于记录已经成功
     * 交给驱动的字节数，后续调用从未发送的位置继续。
     */
    size_t sent_length = 0U;
    while (sent_length < frame_length) {
        const size_t written = tinyusb_cdcacm_write_queue(
            TINYUSB_CDC_ACM_0,
            &frame[sent_length],
            frame_length - sent_length);

        /* FIFO 无空间通常表示主机没有打开或没有继续读取该 CDC 端口。 */
        if (written == 0U) {
            return ESP_ERR_TIMEOUT;
        }

        sent_length += written;

        /* 在协议任务中等待刷新完成，不会阻塞 TinyUSB 自身的事件任务。 */
        const esp_err_t err = tinyusb_cdcacm_write_flush(
            TINYUSB_CDC_ACM_0,
            pdMS_TO_TICKS(USB_UART_TX_TIMEOUT_MS));
        if (err != ESP_OK) {
            return err == ESP_ERR_NOT_FINISHED ? ESP_ERR_TIMEOUT : err;
        }
    }

    return ESP_OK;
}

/**
 * @brief TinyUSB CDC 收到数据时的回调。
 *
 * 回调运行在 TinyUSB 事件任务中，只负责把 RX FIFO 数据搬到流缓冲。这里不
 * 解析协议，也不等待缓冲空间，避免命令处理阻塞 USB 枚举和端点事务。
 */
static void usb_uart_cdc_rx_callback(int interface, cdcacm_event_t *event)
{
    (void)event;

    if (interface != TINYUSB_CDC_ACM_0 || s_usb_uart_rx_stream == NULL) {
        return;
    }

    while (true) {
        size_t received_length = 0U;
        const esp_err_t err = tinyusb_cdcacm_read(
            TINYUSB_CDC_ACM_0,
            s_usb_uart_cdc_read_buffer,
            sizeof(s_usb_uart_cdc_read_buffer),
            &received_length);
        if (err != ESP_OK || received_length == 0U) {
            return;
        }

        const size_t stored_length = xStreamBufferSend(
            s_usb_uart_rx_stream,
            s_usb_uart_cdc_read_buffer,
            received_length,
            0U);
        if (stored_length != received_length) {
            s_usb_uart_rx_overflow = true;
            return;
        }
    }
}

/**
 * @brief 从接收缓存头部删除已经处理或确认无效的数据。
 *
 * 删除后将剩余数据前移，保证下一次解包仍然从 data[0] 开始检查。
 * count 大于或等于当前数据长度时，直接将缓存置空。
 */
static void usb_uart_discard_rx_data(usb_uart_frame_buffer_t *frame_buffer,
                                     size_t count)
{
    /* 已处理范围覆盖整个缓存时，无需搬移数据，直接标记为空。 */
    if (count >= frame_buffer->length) {
        frame_buffer->length = 0U;
        return;
    }

    /* 保留未处理的尾部数据，并将其移动到下一次解包的起始位置。 */
    frame_buffer->length -= count;
    memmove(frame_buffer->data,
            &frame_buffer->data[count],
            frame_buffer->length);
}

/**
 * @brief 检查缓存中是否存在完整协议帧，并处理所有可用帧。
 *
 * esp32s3_protocol_unpack() 返回三种结果：
 *
 * - COMPLETE：帧头、长度和校验和均正确，将完整帧交给命令处理模块；
 * - INCOMPLETE：数据不足以组成完整帧，保留残包等待下一批数据；
 * - INVALID：发现噪声、非法长度或校验失败，丢弃 consumed_length 指定
 *   的字节后重新搜索下一帧。
 *
 * consumed_length 为 0 表示当前数据必须继续保留，此时立即退出，避免
 * 对半帧重复解析。大于 0 时删除对应数据并继续循环，因此同一接收缓存
 * 中的多个粘连帧可以依次处理。
 */
static void usb_uart_check_received_data(usb_uart_frame_buffer_t *frame_buffer)
{
    /* 一次缓存中可能包含多个粘连帧，因此持续解析到没有可消费数据为止。 */
    while (frame_buffer->length != 0U) {
        esp32s3_protocol_frame_t frame;
        size_t consumed_length = 0U;

        /*
         * 协议层负责定位帧头、读取长度并检查 XOR 校验和，同时返回本次
         * 可以安全移出接收缓存的字节数。
         */
        const esp32s3_protocol_unpack_status_t status =
            esp32s3_protocol_unpack(frame_buffer->data,
                                    frame_buffer->length,
                                    &frame,
                                    &consumed_length);

        /* 只有完整且校验正确的数据才允许进入命令处理模块。 */
        if (status == ESP32S3_PROTOCOL_UNPACK_COMPLETE) {
            esp32s3_cmd_handle_frame(&frame);
        }

        /*
         * 没有可消费字节说明当前缓存保存的是半帧，需要等待下一次接收；
         * 此处不能清空，否则跨 USB 读取返回的分包数据会丢失。
         */
        if (consumed_length == 0U) {
            return;
        }

        /*
         * 完整帧：删除整帧；错误帧或噪声：删除协议层确认无效的部分，
         * 随后继续从剩余数据中搜索下一帧。
         */
        usb_uart_discard_rx_data(frame_buffer, consumed_length);
    }
}

/**
 * @brief 将 USB 驱动本次读到的数据加入接收缓存。
 *
 * 数据按字节加入后立即检查，这样既能跨多次读取拼接半帧，也能在一次读取
 * 中处理多个连续帧。正常情况下，缓存达到最大帧长度时一定能够判定为完整
 * 帧或错误帧；额外的满缓存保护用于异常状态下丢弃最早字节并继续同步。
 */
static void usb_uart_receive_data(usb_uart_frame_buffer_t *frame_buffer,
                                  const uint8_t *data,
                                  size_t data_length)
{
    for (size_t i = 0U; i < data_length; ++i) {
        /*
         * 正常协议帧在缓存填满时已经可以完成判断。若异常状态导致缓存仍满，
         * 丢弃最早的一个字节，为新数据腾出空间并尝试重新同步帧头。
         */
        if (frame_buffer->length == sizeof(frame_buffer->data)) {
            usb_uart_discard_rx_data(frame_buffer, 1U);
        }

        /* 逐字节追加，确保帧头或协议帧跨两次驱动读取时仍能正确拼接。 */
        frame_buffer->data[frame_buffer->length++] = data[i];

        /* 每加入一个新字节就尝试解析，完整帧可以尽快交给命令模块。 */
        usb_uart_check_received_data(frame_buffer);
    }
}

/**
 * @brief TinyUSB CDC 命令通道接收任务。
 *
 * CDC 回调写入的字节由流缓冲唤醒本任务。protocol_frame_buffer 会一直保留
 * 未完成帧；只要收到新数据就刷新 last_receive_tick。
 * 连续 500 ms 没有新数据且缓存仍有残包时，清空残包并等待下一帧。
 */
static void usb_uart_command_task(void *arg)
{
    (void)arg;

    /* 保存 USB 驱动单次读取的数据，处理完成后即可重复使用。 */
    uint8_t usb_read_buffer[64];

    /* 跨多次驱动读取保存协议数据，直到拼成完整帧或残包超时。 */
    usb_uart_frame_buffer_t protocol_frame_buffer = {0};

    /* 记录最近一次收到数据的时间，用于判断半帧是否已经失效。 */
    TickType_t last_receive_tick = xTaskGetTickCount();

    while (true) {
        /*
         * 使用有限等待时间而不是 portMAX_DELAY，使任务即使没有新数据也能
         * 定期运行残包超时检查。
         */
        const size_t length = xStreamBufferReceive(
            s_usb_uart_rx_stream,
            usb_read_buffer,
            sizeof(usb_read_buffer),
            pdMS_TO_TICKS(USB_UART_RX_WAIT_MS));

        if (s_usb_uart_rx_overflow) {
            /* 丢字节后原残帧已不可信；清空它并等待协议层重新同步 AA 55。 */
            s_usb_uart_rx_overflow = false;
            protocol_frame_buffer.length = 0U;
            ESP_LOGW(TAG, "CDC RX overflow; discarded partial protocol frame");
        }

        if (length > 0U) {
            /* 收到新数据后刷新时间，并交给接收缓存执行拼帧和完整性检查。 */
            last_receive_tick = xTaskGetTickCount();
            usb_uart_receive_data(&protocol_frame_buffer,
                                  usb_read_buffer,
                                  length);
        } else if (protocol_frame_buffer.length != 0U &&
                   xTaskGetTickCount() - last_receive_tick >=
                       pdMS_TO_TICKS(USB_UART_PARTIAL_FRAME_TIMEOUT_MS)) {
            /*
             * 长时间没有补齐当前帧，认为发送端中断或长度字段异常。
             * 清空后下一批数据将重新从帧头 AA 55 开始同步。
             */
            protocol_frame_buffer.length = 0U;
        }
    }
}

/**
 * @brief 初始化 ESP32-S3 TinyUSB CDC-ACM 命令通道。
 *
 * 通道使用芯片原生 USB D-/GPIO19 和 D+/GPIO20，不占用 UART0。
 * UART0 继续用于系统启动信息和运行日志输出。
 *
 * 初始化过程包括创建 PSRAM 流缓冲、安装 TinyUSB、初始化 CDC 接口和创建
 * PSRAM 栈接收任务；任一步失败都会回收之前已创建的资源。
 */
esp_err_t usb_uart_init(void)
{
    /* 已创建接收任务说明通道已经初始化，重复调用不再安装第二套驱动。 */
    if (s_usb_uart_task != NULL) {
        return ESP_OK;
    }

    /* 流数据和控制结构都放入 PSRAM，给 SD/SPI 保留片内 DMA 内存。 */
    s_usb_uart_rx_stream = xStreamBufferCreateWithCaps(
        USB_UART_RX_STREAM_SIZE,
        1U,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_usb_uart_rx_stream == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 默认配置在 ESP32-S3 上选择 USB-OTG Full-Speed 内置 PHY。 */
    const tinyusb_config_t tinyusb_config = TINYUSB_DEFAULT_CONFIG();
    esp_err_t err = tinyusb_driver_install(&tinyusb_config);
    if (err != ESP_OK) {
        vStreamBufferDeleteWithCaps(s_usb_uart_rx_stream);
        s_usb_uart_rx_stream = NULL;
        return err;
    }

    /* 单路 CDC-ACM 接口，接收回调只负责无阻塞投递原始字节。 */
    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = usb_uart_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    err = tinyusb_cdcacm_init(&cdc_config);
    if (err != ESP_OK) {
        tinyusb_driver_uninstall();
        vStreamBufferDeleteWithCaps(s_usb_uart_rx_stream);
        s_usb_uart_rx_stream = NULL;
        return err;
    }

    /* 协议任务栈放在 PSRAM，避免额外占用约 3 KiB 片内 DMA 内存。 */
    if (xTaskCreateWithCaps(usb_uart_command_task,
                            "usb_uart_cmd",
                            USB_UART_TASK_STACK_SIZE,
                            NULL,
                            USB_UART_TASK_PRIORITY,
                            &s_usb_uart_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        tinyusb_driver_uninstall();
        vStreamBufferDeleteWithCaps(s_usb_uart_rx_stream);
        s_usb_uart_rx_stream = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 该日志仍通过 UART0 输出，不会混入 GPIO19/20 的 USB 指令通道。 */
    ESP_LOGI(TAG, "TinyUSB CDC-ACM command channel ready");
    return ESP_OK;
}
