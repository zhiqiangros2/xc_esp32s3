#include "lvgl_port.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd.h"
#include "lvgl.h"
#include "tp.h"

/*
 * LVGL 使用局部刷新模式，绘制缓冲区一次保存 20 行，而不是保存整个屏幕。
 * 屏幕每行有 LCD_X_RESOLUTION=320 个 RGB565 像素。
 */
#define LVGL_DRAW_BUFFER_LINES 20U

/*
 * 绘制缓冲区的像素容量：320 像素/行 * 20 行 = 6400 个像素。
 * 每个 RGB565 像素占 2 字节，因此实际分配 12800 字节片内内存。
 */
#define LVGL_DRAW_BUFFER_PIXELS (LCD_X_RESOLUTION * LVGL_DRAW_BUFFER_LINES)

/*
 * LVGL 主任务需要执行控件布局、绘制、动画和显示刷新，分配 8192 字节栈。
 * ESP-IDF 的 xTaskCreate() 栈大小参数以字节为单位。
 */
#define LVGL_TASK_STACK_SIZE 8192U

/* LVGL 主任务优先级。数值越大优先级越高，本项目使用优先级 4。 */
#define LVGL_TASK_PRIORITY 4U

/*
 * 触摸任务只负责阻塞队列、复制一帧状态并上报输入，分配 4096 字节栈。
 * lv_indev_read() 会同步执行控件事件回调，因此仍需保留足够栈空间。
 */
#define LVGL_TOUCH_TASK_STACK_SIZE 4096U

/*
 * 触摸任务与 LVGL 主任务使用相同优先级 4。触摸任务大部分时间阻塞在队列，
 * 收到消息后才运行；两个任务通过 lvgl_mutex 串行访问 LVGL。
 */
#define LVGL_TOUCH_TASK_PRIORITY 4U

/* lv_timer_handler() 建议延时过小时，主任务至少休眠 5 ms，避免空转。 */
#define LVGL_TASK_MIN_DELAY_MS 5U

/* lv_timer_handler() 建议延时过大时，主任务最多休眠 20 ms。 */
#define LVGL_TASK_MAX_DELAY_MS 20U

/* 本文件输出 ESP_LOGx 日志时使用的模块标签。 */
static const char *TAG = "LVGL";

/*
 * LVGL 全局互斥锁。LVGL 本身不是线程安全的，主任务、触摸任务和应用任务
 * 调用任何 LVGL API 时都必须通过该锁串行化。本锁不保护 I2C 或触摸队列。
 */
static SemaphoreHandle_t lvgl_mutex = NULL;

/* LVGL 主任务句柄，用于确认任务是否创建以及初始化失败时删除任务。 */
static TaskHandle_t lvgl_task_handle = NULL;

/* 独立触摸任务句柄；该任务永久阻塞等待触摸消息队列。 */
static TaskHandle_t lvgl_touch_task_handle = NULL;

/* 代表 320x240 LCD 的 LVGL 显示设备对象。 */
static lv_display_t *lvgl_display = NULL;

/* 代表 CHSC5432 的 LVGL 指针输入设备对象。 */
static lv_indev_t *lvgl_touch_input = NULL;

/*
 * LVGL 局部绘制缓冲区。初始化时从片内 RAM 分配 6400 个 uint16_t 像素，
 * LCD DMA 完成后 LVGL 才会重新使用其中的数据。
 */
static uint16_t *lvgl_draw_buffer = NULL;

/* 最近一次有效触点坐标；手指抬起后仍保留该坐标用于上报释放事件。 */
static lv_point_t last_touch_point = {0};

/* 最近一次已经上报给 LVGL 的指针状态，初始状态为松开。 */
static lv_indev_state_t last_touch_state = LV_INDEV_STATE_RELEASED;

/* 触摸任务刚从队列收到、等待 lvgl_touch_read() 消费的一帧完整状态。 */
static tp_state_t pending_touch_state = {0};

/* true 表示 pending_touch_state 包含一帧尚未消费的新消息。 */
static bool pending_touch_state_valid = false;

/* true 表示已经调用过 lv_init()，防止初始化失败后重试时重复初始化核心。 */
static bool lvgl_core_initialized = false;

/*
 * LVGL 端口运行标志。创建任务前先置为 true，使刚启动的任务可以立即工作；
 * 任一任务创建失败时会重新置为 false，并释放已经创建的资源。
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

/**
 * @brief 把 LVGL 已经绘制好的矩形像素区域发送到 ST7789 LCD。
 *
 * @details 本函数通过 lv_display_set_flush_cb() 注册为显示刷新回调。当 LVGL
 * 完成一个局部区域的渲染后，会把刷新区域 area 和对应的 RGB565 像素缓冲区
 * pixel_map 传入本函数。
 *
 * area 使用 LVGL 标准的 320x240 屏幕坐标：原点位于左上角，X 从左向右递增，
 * 范围为 0~319；Y 从上向下递增，范围为 0~239。area 的 x1、y1、x2、y2
 * 都包含边界像素，因此矩形宽、高必须分别加 1：
 *
 *     width  = x2 - x1 + 1
 *     height = y2 - y1 + 1
 *
 * pixel_map 仅包含本次 area 覆盖的像素，不是完整屏幕缓冲区。像素按照从左
 * 到右、从上到下的顺序连续保存；LVGL 和 lcd_draw_pixels() 都使用 RGB565
 * 及相同的坐标方向，所以可以直接发送，不需要交换 X/Y 或旋转像素。
 *
 * lcd_draw_pixels() 返回前会等待本次 SPI DMA 传输完成，因此返回后才能调用
 * lv_display_flush_ready()，通知 LVGL 该绘制缓冲区已经可以重新使用。即使 LCD
 * 写入失败，也必须发送完成通知，否则 LVGL 会永久等待本次刷新结束，后续界面
 * 将不再刷新。写入错误通过日志报告给应用。
 *
 * @param[in] display 发起本次刷新的 LVGL 显示设备，同时用于发送刷新完成通知。
 * @param[in] area 本次需要更新的矩形区域，四个坐标均包含在刷新范围内。
 * @param[in] pixel_map area 对应的连续 RGB565 像素数据，所有权仍属于 LVGL。
 */
static void lvgl_display_flush(lv_display_t *display,
                               const lv_area_t *area,
                               uint8_t *pixel_map)
{
    /* 矩形左上角就是 lcd_draw_pixels() 所需的起始坐标。 */
    const uint16_t x = (uint16_t)area->x1;
    const uint16_t y = (uint16_t)area->y1;

    /* LVGL 的结束坐标包含最后一个像素，所以计算宽、高时需要加 1。 */
    const uint16_t width = (uint16_t)(area->x2 - area->x1 + 1);
    const uint16_t height = (uint16_t)(area->y2 - area->y1 + 1);

    /*
     * LVGL RGB565 缓冲区和 lcd_draw_pixels() 都按从左到右、从上到下排列，
     * 因此可以直接传递，不需要旋转、转置或另建整屏缓冲区。
     */
    const esp_err_t result = lcd_draw_pixels(x,
                                              y,
                                              width,
                                              height,
                                              (const uint16_t *)pixel_map);
    if (result != ESP_OK) {
        /* 记录失败区域，便于判断是坐标越界还是底层 SPI/LCD 传输异常。 */
        ESP_LOGE(TAG,
                 "LCD flush failed at (%u,%u) %ux%u: %s",
                 (unsigned)x,
                 (unsigned)y,
                 (unsigned)width,
                 (unsigned)height,
                 esp_err_to_name(result));
    }

    /*
     * 无论 LCD 写入成功还是失败，都要结束本次 LVGL 刷新。这里调用后，LVGL
     * 才能重新使用 pixel_map 指向的局部绘制缓冲区并继续处理下一帧。
     */
    lv_display_flush_ready(display);
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
 *     lvgl_touch_task() 阻塞接收
 *             |
 *             v
 *     pending_touch_state
 *             |
 *             v
 *     本函数填写 lv_indev_data_t
 *
 * lvgl_touch_task() 收到一帧队列消息后，先获取 lvgl_mutex，再设置
 * pending_touch_state 和 pending_touch_state_valid，最后调用
 * lv_indev_read()。LVGL 随后同步调用本函数。因此状态的写入和读取发生在
 * 同一个任务、同一次 LVGL 加锁期间，不需要为 pending_touch_state 再增加
 * 互斥锁。
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
 * data->continue_reading 固定为 false；队列中的下一帧由 lvgl_touch_task()
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
     * pending_touch_state 由 lvgl_touch_task() 从 FreeRTOS 队列取出，并在
     * 调用本回调之前填好。填充和读取发生在同一个触摸任务、同一次 LVGL
     * 加锁期间，因此这里不需要额外互斥锁，也不会访问 I2C。
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

    /* 一次队列消息对应一次读取，下一帧由触摸任务再次调用 lv_indev_read()。 */
    data->continue_reading = false;
}

/**
 * @brief 周期运行 LVGL 定时器，完成界面刷新、动画和控件内部处理。
 *
 * @details 本任务由 lvgl_port_init() 创建，是 LVGL 的主处理任务。它不读取
 * CHSC5432，也不消费触摸消息队列；触摸队列由独立的 lvgl_touch_task()
 * 永久阻塞等待。这样即使当前没有触摸，主任务仍会按时处理显示刷新和 LVGL
 * 软件定时器。
 *
 * 每次循环依次执行以下操作：
 *
 *  1. 永久等待 lvgl_mutex，保证当前没有其他任务正在调用 LVGL；
 *  2. 调用 lv_timer_handler()，处理已经到期的刷新、动画和内部定时器；
 *  3. 保存 lv_timer_handler() 返回的“距离下一次处理还需等待多少毫秒”；
 *  4. 释放 lvgl_mutex，让触摸任务或应用任务能够操作 LVGL；
 *  5. 把等待时间限制在 5~20 ms，再调用 vTaskDelay() 让出 CPU；
 *  6. 延时结束后重新进入循环。
 *
 * 最小等待时间 LVGL_TASK_MIN_DELAY_MS 用于避免返回值为 0 时形成高速空转；
 * 最大等待时间 LVGL_TASK_MAX_DELAY_MS 用于保证即使 LVGL 暂时没有近期定时器，
 * 主任务仍会定期运行。任务延时期间不持有 lvgl_mutex，所以独立触摸任务收到
 * 队列消息后可以立即获取互斥锁并调用 lv_indev_read()。
 *
 * 所有可能从其他任务调用的 LVGL 操作都必须使用同一个 lvgl_mutex。应用
 * 任务应通过 lvgl_port_lock()/lvgl_port_unlock() 获取和释放该锁；触摸任务
 * 在上报输入前会直接获取该锁。
 *
 * 本函数不会自行退出。lvgl_port_init() 后续步骤失败或端口释放资源时，
 * lvgl_release_resources() 使用 vTaskDelete() 删除本任务。
 *
 * @param[in] argument FreeRTOS 任务参数。当前创建任务时传入 NULL，本函数
 * 不使用该参数。
 */
static void lvgl_task(void *argument)
{
    /* 创建任务时没有传递上下文，显式忽略参数以避免编译器警告。 */
    (void)argument;

    /* 任务在整个 LVGL 运行期间持续处理定时器，不会自行退出。 */
    while (true) {
        /* 等待其他 LVGL 调用结束，确保同一时刻只有一个任务进入 LVGL。 */
        xSemaphoreTake(lvgl_mutex, portMAX_DELAY);

        /*
         * 处理已到期的 LVGL 工作，并取得距离下一次处理建议等待的毫秒数。
         * 显示刷新回调 lcd_draw_pixels() 也可能在本次调用中同步执行。
         */
        uint32_t delay_ms = lv_timer_handler();

        /* 本轮 LVGL 处理结束，允许触摸任务和应用任务获取全局锁。 */
        xSemaphoreGive(lvgl_mutex);

        if (delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            /* 返回值过小时至少休眠 5 ms，防止任务持续空转占满 CPU。 */
            delay_ms = LVGL_TASK_MIN_DELAY_MS;
        } else if (delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            /* 返回值过大时最多休眠 20 ms，保证 LVGL 保持稳定处理频率。 */
            delay_ms = LVGL_TASK_MAX_DELAY_MS;
        }

        /* 将毫秒转换为 RTOS Tick，在不持有 LVGL 锁的情况下让出 CPU。 */
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief 永久阻塞等待触摸消息，并把收到的状态直接上报给 LVGL。
 *
 * @details 本任务由 lvgl_port_init() 创建，专门负责连接触摸消息队列和
 * LVGL 指针输入设备。它不读取 CHSC5432，也不调用 I2C；触摸芯片的读取和
 * 坐标解析已经由 GPIO42 中断管理任务完成。
 *
 * 每次循环依次执行以下操作：
 *
 *  1. 调用 tp_receive_state(..., TP_WAIT_FOREVER) 永久等待触摸队列；
 *  2. 队列没有消息时，任务处于阻塞态，不占用 CPU 时间；
 *  3. GPIO42 中断任务发送 tp_state_t 后，本任务立即被 FreeRTOS 唤醒；
 *  4. 获取 lvgl_mutex，保证不会与主 lvgl_task() 或应用任务同时操作 LVGL；
 *  5. 把收到的状态写入 pending_touch_state，并将有效标志置为 true；
 *  6. 调用 lv_indev_read()，由 LVGL 同步调用 lvgl_touch_read() 填充坐标和
 *     PRESSED/RELEASED 状态；
 *  7. 上报结束后释放 lvgl_mutex，再回到队列继续永久等待。
 *
 * tp_receive_state() 正常情况下只会在收到消息时返回 ESP_OK。如果驱动状态
 * 异常导致接收失败，函数记录错误并短暂延时，避免错误日志形成无间隔循环。
 *
 * lv_indev_read() 会同步执行 LVGL 输入处理以及由触摸触发的控件事件回调，
 * 所以这些事件回调运行在本任务中，并且执行期间已经持有 lvgl_mutex。事件
 * 回调可以直接调用普通 LVGL API，但不能再次调用 lvgl_port_lock()，否则会
 * 重复获取同一个非递归互斥锁而死锁。
 *
 * 本函数不会自行退出。lvgl_port_init() 后续步骤失败或端口释放资源时，
 * lvgl_release_resources() 通过 vTaskDelete() 删除本任务，即使它当时正阻塞
 * 在触摸消息队列上也可以被删除。
 *
 * @param[in] argument FreeRTOS 任务参数。当前创建任务时传入 NULL，本函数
 * 不使用该参数。
 */
static void lvgl_touch_task(void *argument)
{
    /* 创建任务时没有传递上下文，显式忽略参数以避免编译器警告。 */
    (void)argument;

    /* 任务在整个 LVGL 运行期间持续等待并处理触摸消息。 */
    while (true) {
        /* 每次循环使用局部变量接收队列中的一帧完整触摸状态。 */
        tp_state_t touch_state;

        /*
         * 本任务只等待 FreeRTOS 消息队列，不读取 I2C。没有触摸消息时永久
         * 休眠，不占用 CPU；GPIO42 中断任务发送状态后，本任务立即被唤醒。
         */
        const esp_err_t result = tp_receive_state(&touch_state,
                                                   TP_WAIT_FOREVER);
        if (result != ESP_OK) {
            /*
             * 永久等待正常只会返回 ESP_OK。进入这里表示参数、初始化状态或
             * 队列状态异常；记录原因并延时，避免故障时持续占用 CPU 打日志。
             */
            ESP_LOGE(TAG,
                     "Failed to receive touch state: %s",
                     esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(LVGL_TASK_MAX_DELAY_MS));
            continue;
        }

        /*
         * LVGL 不是线程安全的。主 LVGL 任务调用 lv_timer_handler()、本任务
         * 填充输入状态并调用 lv_indev_read() 时，都必须持有同一个全局互斥锁。
         * 因此两个任务不会同时进入 LVGL。
         */
        xSemaphoreTake(lvgl_mutex, portMAX_DELAY);

        /*
         * 再次确认 LVGL 端口和输入设备仍然有效。该判断主要用于资源释放或
         * 初始化异常路径，防止使用已经删除的 lv_indev_t。
         */
        if (lvgl_initialized && lvgl_touch_input != NULL) {
            /* 把局部队列消息复制到输入回调能够访问的待处理状态。 */
            pending_touch_state = touch_state;

            /* 先发布有效标志，再调用 LVGL，保证回调能够消费这帧状态。 */
            pending_touch_state_valid = true;

            /*
             * LVGL 会同步调用 lvgl_touch_read()，并继续处理由本次触摸引起的
             * 按钮按下、释放或点击事件。
             */
            lv_indev_read(lvgl_touch_input);
        }

        /* 本帧输入处理结束，允许主 LVGL 任务继续执行定时器和显示刷新。 */
        xSemaphoreGive(lvgl_mutex);
    }
}

static void lvgl_release_resources(void)
{
    if (lvgl_touch_task_handle != NULL) {
        vTaskDelete(lvgl_touch_task_handle);
        lvgl_touch_task_handle = NULL;
    }
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
    if (lvgl_draw_buffer != NULL) {
        heap_caps_free(lvgl_draw_buffer);
        lvgl_draw_buffer = NULL;
    }
    if (lvgl_mutex != NULL) {
        vSemaphoreDelete(lvgl_mutex);
        lvgl_mutex = NULL;
    }

    last_touch_point.x = 0;
    last_touch_point.y = 0;
    last_touch_state = LV_INDEV_STATE_RELEASED;
    pending_touch_state = (tp_state_t){0};
    pending_touch_state_valid = false;
}

/**
 * @brief 初始化 LVGL 核心、LCD 显示接口、触摸输入接口和两个处理任务。
 *
 * @details 调用本函数前，应用必须已经完成 lcd_init()、tp_init() 和 GPIO42
 * 共享中断管理器初始化。本函数只建立 LVGL 与现有 LCD/触摸驱动之间的连接，
 * 不会在这里重新初始化 ST7789 或 CHSC5432。
 *
 * 初始化按照以下顺序执行：
 *
 *  1. 创建 lvgl_mutex，串行化所有跨任务 LVGL 调用；
 *  2. 首次调用时执行 lv_init()，并把毫秒 Tick 来源设置为 esp_timer；
 *  3. 从片内 RAM 分配 320x20 个 RGB565 像素的局部绘制缓冲区；
 *  4. 创建 320x240 LVGL 显示设备，设置 RGB565、局部刷新和 LCD 刷新回调；
 *  5. 创建指针输入设备，绑定 CHSC5432 输入回调并启用事件读取模式；
 *  6. 创建 lvgl_task，周期执行 lv_timer_handler()；
 *  7. 创建 lvgl_touch_task，永久阻塞等待触摸消息队列并上报输入。
 *
 * lvgl_initialized 已经为 true 时直接返回 ESP_OK，因此重复调用不会重复分配
 * 内存、创建 LVGL 对象或启动任务。首次执行过 lv_init() 后，
 * lvgl_core_initialized 会一直保持 true；如果后续资源创建失败，重试时不会
 * 再次调用 lv_init()，但会重新创建端口自己的互斥锁、缓冲区、对象和任务。
 *
 * 任一步骤因内存不足或任务创建失败时，函数调用 lvgl_release_resources()
 * 按照安全顺序删除已经创建的任务、LVGL 对象、绘制缓冲区和互斥锁，并返回
 * ESP_ERR_NO_MEM，不留下半初始化的端口资源。
 *
 * @return ESP_OK 初始化成功或此前已经初始化；资源分配或任务创建失败时返回
 * ESP_ERR_NO_MEM。
 */
esp_err_t lvgl_port_init(void)
{
    /* 已经完整初始化时直接返回，保证本函数可以安全重复调用。 */
    if (lvgl_initialized) {
        return ESP_OK;
    }

    /*
     * 创建所有 LVGL 调用共用的普通互斥锁。创建失败表示 FreeRTOS 堆空间不足，
     * 此时还没有分配其他端口资源，可以直接返回。
     */
    lvgl_mutex = xSemaphoreCreateMutex();
    if (lvgl_mutex == NULL) {
        return ESP_ERR_NO_MEM;
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

    /* 计算 320x20 个 RGB565 像素需要的缓冲区字节数，即 12800 字节。 */
    const size_t draw_buffer_size =
        LVGL_DRAW_BUFFER_PIXELS * sizeof(uint16_t);

    /*
     * 绘制缓冲区放在片内、可按字节访问的 RAM 中，避免 LVGL 绘图和 LCD
     * 传输期间依赖速度较慢的外部 PSRAM。
     */
    lvgl_draw_buffer = heap_caps_malloc(draw_buffer_size,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (lvgl_draw_buffer == NULL) {
        /* 统一释放前面已经创建的互斥锁等端口资源。 */
        lvgl_release_resources();
        return ESP_ERR_NO_MEM;
    }

    /* 创建与物理 LCD 分辨率一致的 320x240 LVGL 显示设备。 */
    lvgl_display = lv_display_create(LCD_X_RESOLUTION, LCD_Y_RESOLUTION);
    if (lvgl_display == NULL) {
        /* 删除已经分配的绘制缓冲区和互斥锁。 */
        lvgl_release_resources();
        return ESP_ERR_NO_MEM;
    }

    /* LVGL 输出和 lcd_draw_pixels() 都使用 RGB565 像素格式。 */
    lv_display_set_color_format(lvgl_display, LV_COLOR_FORMAT_RGB565);

    /*
     * 注册单个 12800 字节绘制缓冲区，并选择局部刷新模式。LVGL 每次只渲染
     * 发生变化的矩形区域，不需要占用完整的 320x240 帧缓冲区。
     */
    lv_display_set_buffers(lvgl_display,
                           lvgl_draw_buffer,
                           NULL,
                           (uint32_t)draw_buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* LVGL 完成局部渲染后，通过该回调把像素写入 ST7789。 */
    lv_display_set_flush_cb(lvgl_display, lvgl_display_flush);

    /* 创建一个 LVGL 指针输入设备，用于接收 CHSC5432 第一触点。 */
    lvgl_touch_input = lv_indev_create();
    if (lvgl_touch_input == NULL) {
        /* 删除显示对象、绘制缓冲区和互斥锁。 */
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
     * 关闭 LVGL 内部的输入定时读取。独立的 lvgl_touch_task 收到队列消息后，
     * 会持有全局 LVGL 互斥锁并主动调用 lv_indev_read()。
    */
    lv_indev_set_mode(lvgl_touch_input, LV_INDEV_MODE_EVENT);

    /*
     * 创建任务前先发布运行标志。新任务可能在 xTaskCreate() 返回前开始执行，
     * 因而触摸任务检查该标志时必须已经看到 true。任何创建失败分支都会将其
     * 恢复为 false。
     */
    lvgl_initialized = true;

    /* 创建负责 LVGL 定时器、布局、绘制和显示刷新的主任务。 */
    const BaseType_t lvgl_task_create_result = xTaskCreate(lvgl_task,
                                                            "lvgl",
                                                            LVGL_TASK_STACK_SIZE,
                                                            NULL,
                                                            LVGL_TASK_PRIORITY,
                                                            &lvgl_task_handle);
    if (lvgl_task_create_result != pdPASS) {
        /* 主任务未创建成功，撤销运行标志并释放全部端口资源。 */
        lvgl_initialized = false;
        lvgl_release_resources();
        return ESP_ERR_NO_MEM;
    }

    /* 创建永久阻塞等待触摸消息队列的独立输入任务。 */
    const BaseType_t lvgl_touch_task_create_result =
        xTaskCreate(lvgl_touch_task,
                    "lvgl_touch",
                    LVGL_TOUCH_TASK_STACK_SIZE,
                    NULL,
                    LVGL_TOUCH_TASK_PRIORITY,
                    &lvgl_touch_task_handle);
    if (lvgl_touch_task_create_result != pdPASS) {
        /*
         * 触摸任务创建失败时，主任务已经存在；统一清理函数会先删除任务，
         * 再删除 LVGL 对象、缓冲区和互斥锁。
         */
        lvgl_initialized = false;
        lvgl_release_resources();
        return ESP_ERR_NO_MEM;
    }

    /* 所有资源和两个任务均已就绪，输出最终显示和输入配置。 */
    ESP_LOGI(TAG,
              "LVGL 9 ready: %ux%u, RGB565, draw buffer=%u lines, touch task=queue",
              LCD_X_RESOLUTION,
              LCD_Y_RESOLUTION,
              LVGL_DRAW_BUFFER_LINES);
    return ESP_OK;
}

esp_err_t lvgl_port_lock(void)
{
    if (!lvgl_initialized || lvgl_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return xSemaphoreTake(lvgl_mutex, portMAX_DELAY) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

void lvgl_port_unlock(void)
{
    if (lvgl_mutex != NULL) {
        xSemaphoreGive(lvgl_mutex);
    }
}
