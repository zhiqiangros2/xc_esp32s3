/**
 * @file key_interrupt.c
 * @brief 正点原子 DNESP32S3 BOX3 板载 K0 按键中断驱动。
 *
 * K0 一端连接 GPIO0，另一端接地。按键松开时，GPIO0 由内部上拉保持高电平；
 * 按下时变为低电平，因此使用下降沿触发中断。
 *
 * 处理流程：
 * 1. K0 按下，GPIO0 产生下降沿并进入 ISR。
 * 2. ISR 只发送任务通知，不执行延时或日志打印。
 * 3. 按键任务收到通知后延时 20 ms，再次读取 GPIO0 进行软件消抖。
 * 4. 确认按下后打印一次日志，并等待按键松开，避免长按重复触发。
 *
 * GPIO0 同时是 ESP32-S3 的启动配置引脚。程序运行时可作为普通按键使用，
 * 但复位或上电时一直按住 K0 会使芯片进入下载模式。
 */

#include "key_interrupt.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define KEY_GPIO GPIO_NUM_0

static const char *TAG = "KEY";

/* 保存按键处理任务句柄，供 ISR 通过任务通知唤醒该任务。 */
static TaskHandle_t key_task_handle = NULL;
static bool key_interrupt_initialized = false;

/**
 * @brief GPIO0 下降沿中断服务函数。
 *
 * ISR 中不能调用会阻塞的函数，也不应直接使用普通日志输出。这里仅发送任务
 * 通知，把消抖、GPIO 电平确认和日志打印交给普通 FreeRTOS 任务处理。
 *
 * @param arg 注册中断时传入的用户参数，本驱动不需要使用。
 */
static void IRAM_ATTR key_isr_handler(void *arg)
{
    (void)arg;

    /*
     * vTaskNotifyGiveFromISR() 通过 task_woken 返回是否唤醒了更高优先级任务。
     * 调用前必须将其初始化为 pdFALSE。
     */
    BaseType_t task_woken = pdFALSE;

    /* ISR 只发送通知，不在中断上下文中进行消抖和日志输出。 */
    vTaskNotifyGiveFromISR(key_task_handle, &task_woken);

    /* 若按键任务已就绪且优先级更高，退出 ISR 前立即请求任务调度。 */
    if (task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief K0 按键事件处理任务。
 *
 * 任务平时阻塞等待 ISR 通知，不占用 CPU。收到通知后等待机械触点稳定，再
 * 确认按键是否仍为低电平。确认有效后只打印一次，并等待按键完全松开。
 */
static void key_task(void *arg)
{
    (void)arg;

    while (true) {
        /* 无限期等待 ISR 通知；pdTRUE 表示取出通知时清零通知计数。 */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* K0 是机械按键，延时 20 ms 过滤按下瞬间的触点抖动。 */
        vTaskDelay(pdMS_TO_TICKS(20));

        /* 按下时 GPIO0 为低电平，延时后仍为低才判定为有效按键。 */
        if (gpio_get_level(KEY_GPIO) == 0) {
            ESP_LOGI(TAG, "K0 pressed");

            /* 等待松开，确保长按只产生一次按键事件。 */
            while (gpio_get_level(KEY_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        /* 清除触点抖动期间累积的通知，准备检测下一次按键动作。 */
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

/**
 * @brief 初始化 K0 GPIO、按键任务和 GPIO ISR 服务。
 */
esp_err_t key_interrupt_init(void)
{
    if (key_interrupt_initialized) {
        ESP_LOGW(TAG, "K0 interrupt is already initialized");
        return ESP_OK;
    }

    const gpio_config_t key_config = {
        .pin_bit_mask = 1ULL << KEY_GPIO,        /* 只配置 GPIO0。 */
        .mode = GPIO_MODE_INPUT,                 /* 按键引脚作为数字输入。 */
        .pull_up_en = GPIO_PULLUP_ENABLE,        /* 松开时保持稳定高电平。 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,   /* 不启用内部下拉。 */
        .intr_type = GPIO_INTR_NEGEDGE,          /* 按下时下降沿触发。 */
    };

    esp_err_t result = gpio_config(&key_config);
    if (result != ESP_OK) {
        return result;
    }

    /*
     * 创建 K0 处理任务。ESP-IDF 的 xTaskCreate() 栈大小单位是字节。
     */
    BaseType_t task_result = xTaskCreate(
        key_task,               /* 任务入口函数。 */
        "key_task",             /* 任务名称。 */
        2048,                   /* 任务栈大小：2048 字节。 */
        NULL,                   /* 不传递任务参数。 */
        5,                      /* FreeRTOS 任务优先级 5，不是 GPIO 中断优先级。
                                 * 数值越大优先级越高，使按键任务被唤醒后尽快运行。 */
        &key_task_handle);      /* 返回创建成功后的任务句柄。 */

    if (task_result != pdPASS) {
        key_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 全局 ISR 服务已由 app_main() 安装，这里只注册 GPIO0 处理函数。 */
    result = gpio_isr_handler_add(KEY_GPIO, key_isr_handler, NULL);
    if (result != ESP_OK) {
        vTaskDelete(key_task_handle);
        key_task_handle = NULL;
        return result;
    }

    key_interrupt_initialized = true;
    ESP_LOGI(TAG, "K0 interrupt ready on GPIO0");
    return ESP_OK;
}
