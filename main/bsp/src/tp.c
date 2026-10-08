/**
 * @file tp.c
 * @brief 正点原子 DNESP32S3 BOX3 外接 CHSC5432 电容触摸屏驱动。
 */

#include "tp.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c.h"

#define TP_I2C_ADDRESS 0x2E
#define TP_I2C_FREQUENCY_HZ 400000
#define TP_I2C_TIMEOUT_MS 1000

#define TP_CONFIG_INFO_ADDRESS_0x20000080 0x20000080UL
#define TP_EVENT_ADDRESS_0x2000002C 0x2000002CUL
#define TP_CONFIG_INFO_DATA_SIZE 15U
#define TP_POINT_DATA_SIZE 5U

#define TP_CONFIG_IC_TYPE_OFFSET 0U
#define TP_CONFIG_VERSION_OFFSET 1U
#define TP_CONFIG_PROJECT_ID_OFFSET 2U
#define TP_CONFIG_VENDOR_ID_OFFSET 4U
#define TP_CONFIG_X_RESOLUTION_OFFSET 6U
#define TP_CONFIG_Y_RESOLUTION_OFFSET 8U
#define TP_CONFIG_FINGER_OFFSET 14U

#define TP_EXPECTED_IC_TYPE_CHSC5432 0x05U
#define TP_EXPECTED_X_RESOLUTION 240U
#define TP_EXPECTED_Y_RESOLUTION 320U

#define TP_STATE_QUEUE_LENGTH 8U

static const char *TAG = "TP";
static board_i2c_device_handle_t tp_device_handle = NULL;
static SemaphoreHandle_t tp_mutex = NULL;
static QueueHandle_t tp_state_queue = NULL;
static bool tp_initialized = false;
static uint8_t previous_point_count = 0;
static uint16_t tp_x_resolution = 0;
static uint16_t tp_y_resolution = 0;

/** CHSC5xxx 配置区中用于识别硬件和解析坐标的信息。 */
typedef struct {
    uint8_t ic_type;
    uint8_t config_version;
    uint16_t project_id;
    uint8_t vendor_id;
    uint16_t tp_x_resolution;
    uint16_t tp_y_resolution;
    uint8_t maximum_touch_points;
} tp_controller_info_t;

/**
 * CHSC5xxx uses a 32-bit, big-endian register address followed by a repeated
 * START for the read phase, matching the vendor's chsc5xxx_rd_reg().
 */
static esp_err_t read_direct_address_unlocked(uint32_t address,
                                              uint8_t *data,
                                              size_t data_size)
{
    const uint8_t address_bytes[] = {
        (uint8_t)(address >> 24),
        (uint8_t)(address >> 16),
        (uint8_t)(address >> 8),
        (uint8_t)address,
    };

    return board_i2c_transmit_receive(tp_device_handle,
                                      address_bytes,
                                      sizeof(address_bytes),
                                      data,
                                      data_size,
                                      TP_I2C_TIMEOUT_MS);
}

/** CHSC5xxx 的 16 位直接地址数据采用低字节在前的排列。 */
static uint16_t read_little_endian_uint16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

/**
 * @brief 读取并解析 CHSC5432 配置区。
 *
 * 0x20000080~0x2000008E 位于同一段配置区，因此一次连续读取 15 字节，
 * 再按相对 0x20000080 的偏移解析 IC 型号、配置版本、Project ID、Vendor ID、
 * X/Y 分辨率和最大触点数。16 位字段为小端序；保留字节只读取、不使用。
 */
static esp_err_t read_controller_info(tp_controller_info_t *info)
{
    uint8_t config_data[TP_CONFIG_INFO_DATA_SIZE] = {0};
    esp_err_t result = read_direct_address_unlocked(TP_CONFIG_INFO_ADDRESS_0x20000080,
                                                     config_data,
                                                     sizeof(config_data));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read touch configuration: %s",
                 esp_err_to_name(result));
        return result;
    }

    info->ic_type = config_data[TP_CONFIG_IC_TYPE_OFFSET];
    info->config_version = config_data[TP_CONFIG_VERSION_OFFSET];

    info->project_id = read_little_endian_uint16(
        &config_data[TP_CONFIG_PROJECT_ID_OFFSET]);
    info->vendor_id = config_data[TP_CONFIG_VENDOR_ID_OFFSET];

    info->tp_x_resolution = read_little_endian_uint16(
        &config_data[TP_CONFIG_X_RESOLUTION_OFFSET]);
    info->tp_y_resolution = read_little_endian_uint16(
        &config_data[TP_CONFIG_Y_RESOLUTION_OFFSET]);

    info->maximum_touch_points = config_data[TP_CONFIG_FINGER_OFFSET];
    return ESP_OK;
}

/**
 * @brief 核对控制器信息是否与本板 CHSC5432 和当前驱动配置一致。
 *
 * IC 型号、原始分辨率和最大触点数必须符合本板配置；配置版本、Project ID
 * 和 Vendor ID 仅记录，不作固定值限制。
 */
static esp_err_t validate_controller_info(const tp_controller_info_t *info)
{
    if (info->ic_type != TP_EXPECTED_IC_TYPE_CHSC5432) {
        ESP_LOGE(TAG, "Unexpected IC type: 0x%02X, expected CHSC5432 (0x%02X)",
                 (unsigned)info->ic_type,
                 (unsigned)TP_EXPECTED_IC_TYPE_CHSC5432);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (info->tp_x_resolution != TP_EXPECTED_X_RESOLUTION ||
        info->tp_y_resolution != TP_EXPECTED_Y_RESOLUTION) {
        ESP_LOGE(TAG,
                 "Unexpected touch resolution: "
                 "TP_X=%u, TP_Y=%u, expected_X=%u, expected_Y=%u",
                 (unsigned)info->tp_x_resolution,
                 (unsigned)info->tp_y_resolution,
                 (unsigned)TP_EXPECTED_X_RESOLUTION,
                 (unsigned)TP_EXPECTED_Y_RESOLUTION);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 事件缓冲区和输出数组均按板载 CHSC5432 的 5 点协议配置。 */
    if (info->maximum_touch_points != TP_MAX_TOUCH_POINTS) {
        ESP_LOGE(TAG, "Unexpected maximum touch points: %u, expected %u",
                 (unsigned)info->maximum_touch_points,
                 (unsigned)TP_MAX_TOUCH_POINTS);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static void release_init_resources(void)
{
    if (tp_device_handle != NULL) {
        board_i2c_remove_device(tp_device_handle);
        tp_device_handle = NULL;
    }

    if (tp_mutex != NULL) {
        vSemaphoreDelete(tp_mutex);
        tp_mutex = NULL;
    }
    if (tp_state_queue != NULL) {
        vQueueDelete(tp_state_queue);
        tp_state_queue = NULL;
    }

    tp_x_resolution = 0;
    tp_y_resolution = 0;
    previous_point_count = 0;
}

/** 把中断任务解析出的完整触摸状态发送到 LVGL 使用的消息队列。 */
static void send_touch_state(const tp_state_t *state)
{
    if (state == NULL || tp_state_queue == NULL) {
        return;
    }

    /*
     * 中断管理代码运行在普通 FreeRTOS 任务中，不在 GPIO ISR 中，因此使用
     * xQueueSend()。等待时间为 0，避免 LVGL 暂时没有消费消息时阻塞中断任务。
     */
    if (xQueueSend(tp_state_queue, state, 0) == pdTRUE) {
        return;
    }

    /*
     * 队列已满说明 LVGL 处理速度暂时落后。丢弃最旧的一帧，再加入最新状态，
     * 防止 LVGL 最终停留在过时的“按下”状态。队列长度为 8，正常情况下不会
     * 进入这里。
    */
    tp_state_t oldest_state;
    (void)xQueueReceive(tp_state_queue, &oldest_state, 0);
    if (xQueueSend(tp_state_queue, state, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to enqueue latest touch state");
    }
}

/**
 * @brief 解析 CHSC5432 的 28 字节事件帧并输出 LCD/LVGL 触摸坐标。
 *
 * @details tp_read_event_frame() 先以大端序写入事件地址 0x2000002C，再通过
 * repeated START 切换到读方向，连续读取 28 字节，最后以 NACK 和 STOP
 * 结束事务。
 *
 * 事件帧布局：
 *
 *  - event_data[0]：事件类型，当前未使用。
 *  - event_data[1] bit3:0：有效触点数，范围为 0~5。
 *  - event_data[1] bit7:4：保留，当前未使用。
 *  - event_data[2..26]：5 组触点数据，每组 5 字节。
 *  - event_data[27]：当前未使用。
 *
 * 第 i 个触点从 base = 2 + i * 5 开始：
 *
 *  - base + 0：原始 X 的低 8 位。
 *  - base + 1：原始 Y 的低 8 位。
 *  - base + 2：压力，当前未使用。
 *  - base + 3 bit3:0：原始 X 的高 4 位。
 *  - base + 3 bit7:4：原始 Y 的高 4 位。
 *  - base + 4 bit3:0：保留，当前未使用。
 *  - base + 4 bit7:4：Touch event；0=按下、4=抬起、8=持续接触。
 *
 * 12 位原始坐标解析公式：
 *
 *  raw_x = ((event_data[base + 3] & 0x0F) << 8) | event_data[base + 0]
 *  raw_y = ((event_data[base + 3] >> 4) << 8) | event_data[base + 1]
 *
 * @param[in] event_data 从 0x2000002C 读取的 28 字节事件帧。
 * @param[in] event_data_size event_data 缓冲区容量，不能小于 28 字节。
 * @param[out] state 有效触点数量及转换后的 LCD/LVGL X/Y 坐标。
 * @return ESP_OK 解析成功；参数或缓冲区容量无效时返回 ESP_ERR_INVALID_ARG；
 * 触点数超过 5 时返回 ESP_ERR_INVALID_RESPONSE。
 * @note CHSC5432 原始坐标会转换为左上原点、X 向右、Y 向下的 320x240 坐标。
 */
static esp_err_t parse_touch_event(const uint8_t *event_data,
                                   size_t event_data_size,
                                   tp_state_t *state)
{
    /* 至少需要完整的 28 字节事件帧，才能安全访问 event_data[0..27]。 */
    if (event_data == NULL || event_data_size < TP_EVENT_DATA_SIZE ||
        state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先清空输出，确保无触摸或所有触点越界时 point_count 为 0。 */
    memset(state, 0, sizeof(*state));

    /* byte[1] 的低 4 位是控制器报告的有效触点数量。 */
    const uint8_t reported_count = event_data[1] & 0x0FU;
    if (reported_count > TP_MAX_TOUCH_POINTS) {
        ESP_LOGW(TAG, "Invalid touch count: %u", (unsigned)reported_count);
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (uint8_t index = 0; index < reported_count; ++index) {
        /* 每个触点占 5 字节；第一个触点从 event_data[2] 开始。 */
        const size_t offset = (size_t)index * TP_POINT_DATA_SIZE;

        /*
         * 对当前触点，event_data[5 + offset] 的低 4 位是原始 X 高位，
         * 高 4 位是原始 Y 高位；event_data[2 + offset] 和 [3 + offset]
         * 分别保存原始 X、Y 坐标的低 8 位。
         */
        const uint16_t raw_x =
            ((uint16_t)(event_data[5U + offset] & 0x0FU) << 8) |
            event_data[2U + offset];
        const uint16_t raw_y =
            ((uint16_t)(event_data[5U + offset] >> 4) << 8) |
            event_data[3U + offset];

        /*
         * 使用初始化时从芯片读取的 TP 原始 X/Y 分辨率判断坐标是否越界，
         * 不在事件解析阶段假定触摸面板一定是某个固定分辨率。
         */
        if (raw_x >= tp_x_resolution || raw_y >= tp_y_resolution) {
            ESP_LOGW(TAG, "Ignore out-of-range point: raw=(%u, %u)",
                     (unsigned)raw_x,
                     (unsigned)raw_y);
            continue;
        }

        /*
         * index 是 CHSC5432 事件帧中的原始触点序号；state->point_count
         * 是目前已经通过坐标范围检查、成功保存到输出数组的触点数量。
         *
         * 例如控制器报告 3 个触点：
         *
         *  - 原始触点 0 有效，保存到 points[0]，point_count 变为 1；
         *  - 原始触点 1 越界，被上面的 continue 跳过，point_count 仍为 1；
         *  - 原始触点 2 有效，此时应保存到 points[1]，而不是 points[2]。
         *
         * 因此不能直接使用原始序号 index 作为输出数组下标，而是使用当前的
         * point_count。这样过滤掉无效触点后，所有有效触点仍会从 points[0]
         * 开始连续存放，中间不会留下空位。
         */
        const uint8_t valid_index = state->point_count;
        tp_point_t *point = &state->points[valid_index];

        /*
         * 触摸关系：将 CHSC5432 原始坐标转换为 LVGL/LCD 坐标。
         *
         *    CHSC5432 原始坐标                      LCD/LVGL 转换后坐标
         *
         *           TP_RAW_X=239                   (0,0) ---------> X=319
         *                ^                            |
         *                |                            |
         *                |                            v
         *       (0,0) ---+---> TP_RAW_Y=319          Y=239
         *
         *                 坐标交换，并反转竖直方向
         *       (TP_RAW_X, TP_RAW_Y) --------------> (X, Y)
         *
         *        LVGL_X = LCD_X = TP_RAW_Y
         *        LVGL_Y = LCD_Y = 239 - TP_RAW_X
         *
         * 本函数中的 raw_x、raw_y 分别对应图中的 TP_RAW_X、TP_RAW_Y。
         * 实现使用初始化时从芯片读取的 tp_x_resolution - 1，而不是写死
         * 239；本板 tp_x_resolution=240，因此计算结果与上式完全相同。
         */
        point->x = raw_y;
        point->y = (uint16_t)(tp_x_resolution - 1U - raw_x);

        /* 每个触点状态字节的高 4 位为 Touch event。 */
        point->event = event_data[6U + offset] >> 4;

        /* 只有完成坐标检查并写入数组的触点才计入最终有效数量。 */
        ++state->point_count;
    }

    return ESP_OK;
}

esp_err_t tp_init(void)
{
    if (tp_initialized) {
        return ESP_OK;
    }

    esp_err_t result = board_i2c_add_device(TP_I2C_ADDRESS,
                                             TP_I2C_FREQUENCY_HZ,
                                             &tp_device_handle);
    if (result != ESP_OK) {
        return result;
    }

    /*
     * 先把手册列出的识别信息全部读取并输出，再判断它们是否符合本板。
     * 这样即使初始化失败，日志中仍然保留实际读到的版本和配置，便于排查。
     */
    tp_controller_info_t controller_info = {0};
    result = read_controller_info(&controller_info);
    if (result != ESP_OK) {
        release_init_resources();
        return result;
    }
    //TP info: IC=0x05, CHSC5432  config=0x03, project=0x0001, vendor=0x45, 
    //TP_X=240, TP_Y=320, finger=5
    ESP_LOGI(TAG,
             "TP info: IC=0x%02X, config=0x%02X, project=0x%04X, "
             "vendor=0x%02X, TP_X=%u, TP_Y=%u, finger=%u",
             (unsigned)controller_info.ic_type,
             (unsigned)controller_info.config_version,
             (unsigned)controller_info.project_id,
             (unsigned)controller_info.vendor_id,
             (unsigned)controller_info.tp_x_resolution,
             (unsigned)controller_info.tp_y_resolution,
             (unsigned)controller_info.maximum_touch_points);

    result = validate_controller_info(&controller_info);
    if (result != ESP_OK) {
        release_init_resources();
        return result;
    }

    /*
     * 保存 CHSC5432 报告的原始分辨率，事件解析时先用它检查 raw_x/raw_y，
     * 再转换成 LCD/LVGL 使用的 X=320、Y=240 标准横屏坐标。
     */
    tp_x_resolution = controller_info.tp_x_resolution;
    tp_y_resolution = controller_info.tp_y_resolution;

    /* tp_mutex 串行化 I2C 事务；消息队列把解析结果传递给 LVGL 任务。 */
    tp_mutex = xSemaphoreCreateMutex();
    tp_state_queue = xQueueCreate(TP_STATE_QUEUE_LENGTH, sizeof(tp_state_t));
    if (tp_mutex == NULL || tp_state_queue == NULL) {
        release_init_resources();
        return ESP_ERR_NO_MEM;
    }

    tp_initialized = true;
    ESP_LOGI(TAG,
             "CHSC5432 ready: address=0x%02X, TP_X=%u, TP_Y=%u, points=%u",
             TP_I2C_ADDRESS,
             (unsigned)tp_x_resolution,
             (unsigned)tp_y_resolution,
             (unsigned)controller_info.maximum_touch_points);
    return ESP_OK;
}

esp_err_t tp_read_event_frame(uint8_t *event_data, size_t event_data_size)
{
    /* 调用者提供的缓冲区必须能够保存完整的 28 字节事件帧。 */
    if (event_data == NULL || event_data_size < TP_EVENT_DATA_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tp_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 本函数只读取原始事件帧；触点解析由调用者在读取成功后单独执行。 */
    xSemaphoreTake(tp_mutex, portMAX_DELAY);
    esp_err_t result = read_direct_address_unlocked(TP_EVENT_ADDRESS_0x2000002C,
                                                     event_data,
                                                     TP_EVENT_DATA_SIZE);
    xSemaphoreGive(tp_mutex);
    return result;
}

esp_err_t tp_receive_state(tp_state_t *state, uint32_t timeout_ms)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tp_initialized || tp_state_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t wait_ticks;
    if (timeout_ms == TP_WAIT_FOREVER) {
        /* portMAX_DELAY 使接收任务休眠，直到队列中真正出现一帧触摸状态。 */
        wait_ticks = portMAX_DELAY;
    } else {
        wait_ticks = pdMS_TO_TICKS(timeout_ms);
        if (timeout_ms != 0 && wait_ticks == 0) {
            /* 非零的短等待时间至少换算成 1 个 RTOS Tick。 */
            wait_ticks = 1;
        }
    }

    return xQueueReceive(tp_state_queue, state, wait_ticks) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t tp_interrupt_process(void)
{
    uint8_t event_data[TP_EVENT_DATA_SIZE] = {0};
    tp_state_t state = {0};

    /* 第一步只通过 I2C 读取 CHSC5432 的 28 字节原始事件帧。 */
    esp_err_t result = tp_read_event_frame(event_data, sizeof(event_data));
    if (result != ESP_OK) {
        /*
         * 通信失败时发布“无触点”状态，防止 LVGL 一直保留上一次按下状态。
         * 中断管理任务会在共享中断线仍为低电平时继续重试，不会触发重启。
         */
        send_touch_state(&state);
        return result;
    }

    /* 第二步解析刚读取的事件帧，得到触点数量、事件码和 LCD/LVGL 坐标。 */
    result = parse_touch_event(event_data, sizeof(event_data), &state);
    if (result != ESP_OK) {
        /* 帧内容无效时同样发布“无触点”状态，但不会触发系统重启。 */
        send_touch_state(&state);
        return result;
    }

    /* I2C 读取和事件解析都成功后，把完整状态发送到队列供 LVGL 任务接收。 */
    send_touch_state(&state);

    /*
     * 当前没有触点、但上一次存在触点，说明手指刚刚离开屏幕。
     * 只有状态从“按下”变为“松开”时才打印一次，避免空闲期间重复输出日志。
     */
    if (state.point_count == 0) {
        if (previous_point_count != 0) {
            ESP_LOGI(TAG, "Touch released");
        }
    } else {
        /* 当前仍有触摸时，依次输出转换后的 LCD/LVGL 坐标。 */
        for (uint8_t index = 0; index < state.point_count; ++index) {
            ESP_LOGI(TAG, "Point %u/%u: x=%u, y=%u, event=0x%02X",
                     (unsigned)(index + 1U),
                     (unsigned)state.point_count,
                     (unsigned)state.points[index].x,
                     (unsigned)state.points[index].y,
                     (unsigned)state.points[index].event);
        }
    }

    /* 保存本次触点数量，供下一次中断判断是否刚发生触摸释放。 */
    previous_point_count = state.point_count;
    return ESP_OK;
}
