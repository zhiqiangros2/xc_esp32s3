#include "lvgl_port.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "lvgl.h"
#include "spi.h"
#include "tp.h"

/*
 * 每个 LVGL 绘制缓冲区保存完整的 320x240 RGB565 画面，共 76800 个像素、
 * 153600 字节。两个全屏缓冲区都放在 PSRAM 中，避免耗尽片内 RAM。
 */
#define LVGL_DRAW_BUFFER_PIXELS (LCD_X_RESOLUTION * LCD_Y_RESOLUTION)

/*
 * LVGL 主任务需要执行控件布局、绘制、动画和显示刷新，分配 8192 字节栈。
 * ESP-IDF 的 xTaskCreate() 栈大小参数以字节为单位。
 */
#define LVGL_TASK_STACK_SIZE 8192U

/* LVGL 主任务优先级。数值越大优先级越高，本项目使用优先级 4。 */
#define LVGL_TASK_PRIORITY 4U

/* lv_timer_handler() 建议延时过小时，主任务至少休眠 5 ms，避免空转。 */
#define LVGL_TASK_MIN_DELAY_MS 5U

/* lv_timer_handler() 建议延时过大时，主任务最多休眠 20 ms。 */
#define LVGL_TASK_MAX_DELAY_MS 20U

/* 本文件输出 ESP_LOGx 日志时使用的模块标签。 */
static const char *TAG = "LVGL";

/* LVGL 主任务句柄，用于确认任务是否创建以及初始化失败时删除任务。 */
static TaskHandle_t lvgl_task_handle = NULL;

/* 代表 320x240 LCD 的 LVGL 显示设备对象。 */
static lv_display_t *lvgl_display = NULL;

/* 代表 CHSC5432 的 LVGL 指针输入设备对象。 */
static lv_indev_t *lvgl_touch_input = NULL;

/* 两个 PSRAM 全屏缓冲区交替用于 LVGL 渲染和 SPI DMA 发送。 */
static uint16_t *lvgl_draw_buffer_1 = NULL;
static uint16_t *lvgl_draw_buffer_2 = NULL;

/* 最近一次有效触点坐标；手指抬起后仍保留该坐标用于上报释放事件。 */
static lv_point_t last_touch_point = {0};

/* 最近一次已经上报给 LVGL 的指针状态，初始状态为松开。 */
static lv_indev_state_t last_touch_state = LV_INDEV_STATE_RELEASED;

/* LVGL 任务刚从队列收到、等待 lvgl_touch_read() 消费的一帧完整状态。 */
static tp_state_t pending_touch_state = {0};

/* true 表示 pending_touch_state 包含一帧尚未消费的新消息。 */
static bool pending_touch_state_valid = false;

/* true 表示已经调用过 lv_init()，防止初始化失败后重试时重复初始化核心。 */
static bool lvgl_core_initialized = false;

/*
 * LVGL 端口初始化标志。为 true 表示显示、输入设备和绘制缓冲区已经就绪。
 */
static bool lvgl_initialized = false;

/**
 * @brief 向 LVGL 返回系统启动后经过的毫秒数。
 *
 * @details lvgl_port_init() 通过 lv_tick_set_cb() 把本函数注册为 LVGL 的
 * 时间基准。LVGL 在处理动画、定时器、按键消抖和界面刷新周期时会调用它，
 * 因此本函数只读取系统计时器，不延时，也不修改任何全局状态。
 *
 * esp_timer_get_time() 返回系统启动后经过的微秒数。除以 1000 后转换成 LVGL
 * 要求的毫秒数，再转换为 uint32_t。32 位毫秒计数约每 49.7 天自然回绕一次；
 * LVGL 使用无符号时间差处理回绕，因此不需要在本函数中额外清零或校正。
 *
 * @return 系统启动后经过的毫秒数，数值溢出后按 uint32_t 规则自然回绕。
 */
static uint32_t lvgl_tick_get_ms(void)
{
    /* ESP Timer 单位为微秒；1000 微秒等于 1 毫秒。 */
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/** SPI DMA 完成后通知 LVGL 当前绘制缓冲区可以重新使用。 */
static bool lvgl_display_flush_done(void *user_context)
{
    lv_display_flush_ready((lv_display_t *)user_context);
    return false;
}

/**
 * @brief 把 LVGL 已经绘制好的矩形像素区域发送到 ST7789 LCD。
 *
 * @details 本函数通过 lv_display_set_flush_cb() 注册为显示刷新回调。当 LVGL
 * 完成一帧画面的渲染后，会把刷新区域 area 和对应的 RGB565 像素缓冲区
 * pixel_map 传入本函数。
 *
 * area 使用 LVGL 标准的 320x240 屏幕坐标：原点位于左上角，X 从左向右递增，
 * 范围为 0~319；Y 从上向下递增，范围为 0~239。area 的 x1、y1、x2、y2
 * 都包含边界像素，因此矩形宽、高必须分别加 1：
 *
 *     width  = x2 - x1 + 1
 *     height = y2 - y1 + 1
 *
 * 当前使用全屏渲染模式，因此 area 为完整的 320x240 屏幕，pixel_map 包含
 * 一整帧连续像素。像素按照从左到右、从上到下的顺序保存；提交 DMA 前只需
 * 把每个 RGB565 像素的高低字节交换为 ST7789 所需的线路顺序。
 *
 * lcd_draw_rgb565_bytes_async() 只提交传输，不等待 SPI DMA。DMA 完成中断通过
 * lvgl_display_flush_done() 调用 lv_display_flush_ready()，此后 LVGL 才能复用
 * 当前绘制缓冲区。提交失败时不会产生完成中断，由刷新回调直接结束本次刷新。
 *
 * @param[in] display 发起本次刷新的 LVGL 显示设备，同时用于发送刷新完成通知。
 * @param[in] area 本次需要更新的矩形区域，四个坐标均包含在刷新范围内。
 * @param[in] pixel_map area 对应的连续 RGB565 像素数据，所有权仍属于 LVGL。
 */
static void lvgl_display_flush(lv_display_t *display,
                               const lv_area_t *area,
                               uint8_t *pixel_map)
{
    /* 矩形左上角就是 LCD 异步绘制接口所需的起始坐标。 */
    const uint16_t x = (uint16_t)area->x1;
    const uint16_t y = (uint16_t)area->y1;

    /* LVGL 的结束坐标包含最后一个像素，所以计算宽、高时需要加 1。 */
    const uint16_t width = (uint16_t)(area->x2 - area->x1 + 1);
    const uint16_t height = (uint16_t)(area->y2 - area->y1 + 1);

    /* ST7789 在线路上先接收 RGB565 高字节，直接在当前 DMA 缓冲区内交换。 */
    lv_draw_rgb565_swap(pixel_map, (uint32_t)width * height);

    /*
     * DMA 直接读取 LVGL 绘制缓冲区。成功提交后不能立即通知 LVGL，必须等
     * lcd_color_transfer_done() 在 DMA 完成中断中调用完成回调。
     */
    const esp_err_t result = lcd_draw_rgb565_bytes_async(
        x,
        y,
        width,
        height,
        pixel_map,
        lvgl_display_flush_done,
        display);
    if (result != ESP_OK) {
        /* 记录失败区域，便于判断是坐标越界还是底层 SPI/LCD 传输异常。 */
        ESP_LOGE(TAG,
                 "LCD flush failed at (%u,%u) %ux%u: %s",
                 (unsigned)x,
                 (unsigned)y,
                 (unsigned)width,
                 (unsigned)height,
                 esp_err_to_name(result));
        /* 提交失败时不会产生 DMA 完成回调，需要在当前上下文结束刷新。 */
        lv_display_flush_ready(display);
    }
}

/**
 * @brief 把一帧已经解析完成的触摸状态填入 LVGL 指针输入数据。
 *
 * @details 本函数是通过 lv_indev_set_read_cb() 注册的 LVGL 输入设备读取
 * 回调。它不访问 CHSC5432、不发起 I2C 通信，也不直接读取 FreeRTOS 队列。
 * 完整的数据流如下：
 *
 *     GPIO42 中断任务
 *             |
 *             v
 *     读取并解析 CHSC5432
 *             |
 *             v
 *     FreeRTOS 触摸消息队列
 *             |
 *             v
 *     lvgl_task() 限时阻塞接收
 *             |
 *             v
 *     pending_touch_state
 *             |
 *             v
 *     本函数填写 lv_indev_data_t
 *
 * lvgl_task() 收到一帧队列消息后，设置 pending_touch_state 和有效标志，
 * 再调用 lv_indev_read()。LVGL 随后同步调用本函数；状态的写入和读取始终
 * 发生在同一个任务中，不需要额外互斥锁。
 *
 * 本项目把 CHSC5432 作为单指 LVGL 指针使用。即使控制器同时报告多个触点，
 * 本函数也只把 points[0] 上报给 LVGL：
 *
 *  - point_count == 0：当前没有触点，上报 LV_INDEV_STATE_RELEASED；
 *  - event == TP_TOUCH_EVENT_PUT_UP：手指抬起，上报 RELEASED；
 *  - 按下、持续接触或其他非抬起事件：上报 LV_INDEV_STATE_PRESSED。
 *
 * 松开时保留 last_touch_point，不把坐标清零。这符合 LVGL 指针输入习惯，
 * 使释放事件仍对应最后一次有效触摸位置。每次调用只消费一帧状态，所以
 * data->continue_reading 固定为 false；队列中的下一帧由 lvgl_task()
 * 再次接收并触发下一次 lv_indev_read()。
 *
 * pending_touch_state_valid 正常情况下必定为 true。保留无待处理状态分支是
 * 防御性处理：如果其他代码主动调用 lv_indev_read()，函数会重复上报最近
 * 一次状态和坐标，而不会使用未准备好的数据。
 *
 * @param[in] input_device 调用本回调的 LVGL 输入设备。本项目只有一个触摸
 * 指针设备，函数不需要读取该参数。
 * @param[out] data 返回给 LVGL 的按下/松开状态、指针坐标以及是否继续读取。
 */
static void lvgl_touch_read(lv_indev_t *input_device,
                            lv_indev_data_t *data)
{
    /* 本项目只有一个触摸输入设备，不需要根据 input_device 区分硬件。 */
    (void)input_device;

    /*
     * pending_touch_state 由 lvgl_task() 从 FreeRTOS 队列取出，并在调用
     * 本回调之前填好。填充和读取发生在同一个任务中，因此这里不需要锁，
     * 也不会访问 I2C。
     */
    if (!pending_touch_state_valid) {
        /*
         * 当前没有新消息可消费时，重复返回最近一次状态。释放状态下保留最后
         * 坐标；按下状态下继续报告原坐标，避免 LVGL 收到未初始化的数据。
         */
        data->state = last_touch_state;
        data->point = last_touch_point;

        /* 当前没有更多已经准备好的状态，本次读取到此结束。 */
        data->continue_reading = false;
        return;
    }

    /* 当前 pending_touch_state 只允许消费一次，防止重复处理同一队列消息。 */
    pending_touch_state_valid = false;

    /*
     * 第一触点已经由 tp.c 转换成与 LVGL 相同的左上原点 320x240 坐标。
     * 无触点表示松开；有触点时更新坐标，并根据事件码区分按下和抬起。
     */
    if (pending_touch_state.point_count == 0) {
        /* 控制器报告 0 个有效触点，表示所有手指都已经离开屏幕。 */
        last_touch_state = LV_INDEV_STATE_RELEASED;
    } else {
        /* LVGL 指针设备只使用第一触点，其余触点保留在状态中但不上报。 */
        const tp_point_t *point = &pending_touch_state.points[0];

        /* 保存最新有效坐标，抬起后仍使用这个位置上报释放事件。 */
        last_touch_point.x = point->x;
        last_touch_point.y = point->y;

        /* Put up 表示松开；Put down 和 Contact 都表示指针仍处于按下状态。 */
        last_touch_state = point->event == TP_TOUCH_EVENT_PUT_UP
                               ? LV_INDEV_STATE_RELEASED
                               : LV_INDEV_STATE_PRESSED;
    }

    /* 把转换完成的状态和坐标写入 LVGL 本次要求返回的数据结构。 */
    data->state = last_touch_state;
    data->point = last_touch_point;

    /* 一次队列消息对应一次读取，下一帧由 LVGL 任务继续处理。 */
    data->continue_reading = false;
}

/**
 * @brief 统一处理 LVGL 定时器、显示刷新和触摸输入。
 *
 * @details GPIO42 中断管理任务负责读取并解析 CHSC5432，再把触摸状态写入
 * 队列。本任务调用 lv_timer_handler() 后，使用 LVGL 建议的等待时间阻塞接收
 * 队列；触摸消息可以提前唤醒任务，等待超时则开始下一轮定时器处理。
 *
 * 每次循环依次执行以下操作：
 *
 *  1. 调用 lv_timer_handler() 处理刷新、动画和内部定时器；
 *  2. 把建议等待时间限制在 5~20 ms；
 *  3. 在触摸队列上阻塞到收到消息或等待时间结束；
 *  4. 收到消息时调用 lv_indev_read()，并非阻塞地清空已有队列消息；
 *  5. 返回循环开始下一轮 LVGL 定时器处理。
 *
 * 所有 LVGL API 和控件事件回调都在本任务中执行，因此不需要 LVGL 全局锁。
 * 应用界面在 lvgl_port_start() 前创建；任务启动后的界面修改由事件回调执行。
 *
 * 本函数不会自行退出；端口释放资源时由 lvgl_release_resources() 删除。
 *
 * @param[in] argument FreeRTOS 任务参数。当前创建任务时传入 NULL，本函数
 * 不使用该参数。
 */
static void lvgl_task(void *argument)
{
    /* 创建任务时没有传递上下文，显式忽略参数以避免编译器警告。 */
    (void)argument;

    /* 任务在整个 LVGL 运行期间持续处理定时器和触摸消息。 */
    while (true) {
        /*
         * 处理已到期的 LVGL 工作，并取得距离下一次处理建议等待的毫秒数。
         * 显示刷新回调会把当前缓冲区异步提交给 SPI DMA。
         */
        uint32_t delay_ms = lv_timer_handler();

        if (delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            /* 返回值过小时至少等待 5 ms，防止任务持续空转占满 CPU。 */
            delay_ms = LVGL_TASK_MIN_DELAY_MS;
        } else if (delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            /* 返回值过大时最多等待 20 ms，保证 LVGL 保持稳定处理频率。 */
            delay_ms = LVGL_TASK_MAX_DELAY_MS;
        }

        /* 等待触摸消息；没有消息时，该等待同时替代原来的 vTaskDelay()。 */
        tp_state_t touch_state;
        esp_err_t result = tp_receive_state(&touch_state, delay_ms);
        if (result == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Failed to receive touch state: %s",
                     esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(LVGL_TASK_MAX_DELAY_MS));
            continue;
        }

        /* 处理刚收到的消息，并连续取出队列中已经到达的其余消息。 */
        do {
            pending_touch_state = touch_state;
            pending_touch_state_valid = true;
            lv_indev_read(lvgl_touch_input);
            result = tp_receive_state(&touch_state, 0);
        } while (result == ESP_OK);

        if (result != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG,
                     "Failed to drain touch state: %s",
                     esp_err_to_name(result));
        }
    }
}

static void lvgl_release_resources(void)
{
    if (lvgl_task_handle != NULL) {
        vTaskDelete(lvgl_task_handle);
        lvgl_task_handle = NULL;
    }
    if (lvgl_touch_input != NULL) {
        lv_indev_delete(lvgl_touch_input);
        lvgl_touch_input = NULL;
    }
    if (lvgl_display != NULL) {
        lv_display_delete(lvgl_display);
        lvgl_display = NULL;
    }
    if (lvgl_draw_buffer_2 != NULL) {
        board_spi_dma_free(lvgl_draw_buffer_2);
        lvgl_draw_buffer_2 = NULL;
    }
    if (lvgl_draw_buffer_1 != NULL) {
        board_spi_dma_free(lvgl_draw_buffer_1);
        lvgl_draw_buffer_1 = NULL;
    }
    last_touch_point.x = 0;
    last_touch_point.y = 0;
    last_touch_state = LV_INDEV_STATE_RELEASED;
    pending_touch_state = (tp_state_t){0};
    pending_touch_state_valid = false;
}

/**
 * @brief 初始化 LVGL 核心、LCD 显示接口和触摸输入接口。
 *
 * @details 调用本函数前，应用必须已经完成 lcd_init()、tp_init() 和 GPIO42
 * 共享中断管理器初始化。本函数只建立 LVGL 与现有 LCD/触摸驱动之间的连接，
 * 不会在这里重新初始化 ST7789 或 CHSC5432。
 *
 * 初始化按照以下顺序执行：
 *
 *  1. 首次调用时执行 lv_init()，并把毫秒 Tick 来源设置为 esp_timer；
 *  2. 从 PSRAM 分配两个 320x240 RGB565 全屏绘制缓冲区；
 *  3. 创建 320x240 LVGL 显示设备，设置 RGB565、全屏刷新和 LCD 刷新回调；
 *  4. 创建指针输入设备，绑定 CHSC5432 输入回调并启用事件读取模式。
 *
 * lvgl_initialized 已经为 true 时直接返回 ESP_OK，因此重复调用不会重复分配
 * 内存或创建 LVGL 对象。首次执行过 lv_init() 后，
 * lvgl_core_initialized 会一直保持 true；如果后续资源创建失败，重试时不会
 * 再次调用 lv_init()，但会重新创建端口自己的缓冲区和对象。
 *
 * 任一步骤因资源分配失败时，函数调用 lvgl_release_resources()
 * 按照安全顺序删除已经创建的 LVGL 对象和绘制缓冲区，并返回
 * ESP_ERR_NO_MEM，不留下半初始化的端口资源。
 *
 * @return ESP_OK 初始化成功或此前已经初始化；资源分配失败时返回 ESP_ERR_NO_MEM。
 */
esp_err_t lvgl_port_init(void)
{
    /* 已经完整初始化时直接返回，保证本函数可以安全重复调用。 */
    if (lvgl_initialized) {
        return ESP_OK;
    }

    /*
     * LVGL 核心在整个程序生命周期中只能初始化一次。Tick 回调使用 esp_timer
     * 的微秒计数除以 1000，向 LVGL 提供持续递增的毫秒时间。
     */
    if (!lvgl_core_initialized) {
        lv_init();
        lv_tick_set_cb(lvgl_tick_get_ms);
        lvgl_core_initialized = true;
    }

    /* 计算 320x240 个 RGB565 像素需要的缓冲区字节数，即 153600 字节。 */
    const size_t draw_buffer_size =
        LVGL_DRAW_BUFFER_PIXELS * sizeof(uint16_t);

    /* 两个全屏缓冲区都位于 PSRAM，并满足 ESP32-S3 外部内存 DMA 对齐要求。 */
    lvgl_draw_buffer_1 = board_spi_psram_dma_alloc(draw_buffer_size);
    lvgl_draw_buffer_2 = board_spi_psram_dma_alloc(draw_buffer_size);
    if (lvgl_draw_buffer_1 == NULL || lvgl_draw_buffer_2 == NULL) {
        /* 统一释放前面已经创建的端口资源。 */
        lvgl_release_resources();
        return ESP_ERR_NO_MEM;
    }

    /* 创建与物理 LCD 分辨率一致的 320x240 LVGL 显示设备。 */
    lvgl_display = lv_display_create(LCD_X_RESOLUTION, LCD_Y_RESOLUTION);
    if (lvgl_display == NULL) {
        /* 删除已经分配的绘制缓冲区。 */
        lvgl_release_resources();
        return ESP_ERR_NO_MEM;
    }

    /* LVGL 绘制缓冲区使用 RGB565 像素格式。 */
    lv_display_set_color_format(lvgl_display, LV_COLOR_FORMAT_RGB565);

    /*
     * 注册两个 153600 字节全屏缓冲区。界面发生变化时 LVGL 绘制完整画面；
     * SPI DMA 发送一个缓冲区时，LVGL 可以使用另一个缓冲区准备下一帧。
     */
    lv_display_set_buffers(lvgl_display,
                           lvgl_draw_buffer_1,
                           lvgl_draw_buffer_2,
                           (uint32_t)draw_buffer_size,
                           LV_DISPLAY_RENDER_MODE_FULL);

    /* LVGL 完成全屏渲染后，通过该回调把像素写入 ST7789。 */
    lv_display_set_flush_cb(lvgl_display, lvgl_display_flush);

    /* 创建一个 LVGL 指针输入设备，用于接收 CHSC5432 第一触点。 */
    lvgl_touch_input = lv_indev_create();
    if (lvgl_touch_input == NULL) {
        /* 删除显示对象和绘制缓冲区。 */
        lvgl_release_resources();
        return ESP_ERR_NO_MEM;
    }

    /* 声明该输入设备是触摸屏、鼠标一类的坐标指针设备。 */
    lv_indev_set_type(lvgl_touch_input, LV_INDEV_TYPE_POINTER);

    /* 把触摸输入明确关联到上面创建的 320x240 显示设备。 */
    lv_indev_set_display(lvgl_touch_input, lvgl_display);

    /* LVGL 读取输入时，由 lvgl_touch_read() 填写坐标和按下/松开状态。 */
    lv_indev_set_read_cb(lvgl_touch_input, lvgl_touch_read);
    /*
     * 关闭 LVGL 内部的输入定时读取。统一的 lvgl_task 收到队列消息后主动调用
     * lv_indev_read()，触摸事件和显示刷新始终在同一个任务中执行。
     */
    lv_indev_set_mode(lvgl_touch_input, LV_INDEV_MODE_EVENT);

    /* 发布端口就绪状态，但暂不运行 lv_timer_handler()。 */
    lvgl_initialized = true;

    return ESP_OK;
}

esp_err_t lvgl_port_start(void)
{
    if (!lvgl_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (lvgl_task_handle != NULL) {
        return ESP_OK;
    }

    /* 创建统一负责触摸、LVGL 定时器、布局、绘制和显示刷新的任务。 */
    const BaseType_t lvgl_task_create_result = xTaskCreate(lvgl_task,
                                                            "lvgl",
                                                            LVGL_TASK_STACK_SIZE,
                                                            NULL,
                                                            LVGL_TASK_PRIORITY,
                                                            &lvgl_task_handle);
    if (lvgl_task_create_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* 首屏已经创建完成，现在才允许 LVGL 执行第一次布局和刷新。 */
    ESP_LOGI(TAG,
              "LVGL 9 ready: %ux%u, RGB565, draw buffers=2x full screen in PSRAM, task=single",
              LCD_X_RESOLUTION,
              LCD_Y_RESOLUTION);
    return ESP_OK;
}
