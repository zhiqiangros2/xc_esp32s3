#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "aw9523b.h"
#include "bsp_info.h"
#include "camera.h"
#include "display.h"
#include "fatfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"
#include "interrupt_manager.h"
#include "key_interrupt.h"
#include "lcd.h"
#include "littlefs.h"
#include "lvgl_port.h"
#include "lvgl_ui.h"
#include "nvs_flash.h"
#include "sd_fatfs.h"
#include "spi.h"
#include "tp.h"

static const char *TAG = "BOX3";

void app_main(void)
{
    //vTaskDelay(pdMS_TO_TICKS(10000));

    /* NVS 用于蓝牙控制器的 PHY 校准和配对信息，必须先于蓝牙功能初始化。 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    bsp_info_print();

    /* K0 独立连接 GPIO0，不经过 I2C 或 GPIO42 共享中断管理器。 */
    ESP_ERROR_CHECK(key_interrupt_init());

    /* 初始化板级 I2C0 总线，后续 AW9523B 和触摸屏共用该总线。 */
    ESP_ERROR_CHECK(board_i2c_init());

    /* 初始化 AW9523B 后显式打开上游 VBAT 和模拟 3.3 V 电源。 */
    ESP_ERROR_CHECK(aw9523b_init());
    ESP_ERROR_CHECK(aw9523b_enable_box3_power());

    /* 分开控制共享复位和 GC0308 2.8 V 电源，保持摄像头处于复位状态上电。 */
    ESP_ERROR_CHECK(aw9523b_set_box3_touch_camera_reset(true));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK(aw9523b_enable_box3_camera_power());

    /* 确保板载红、蓝 LED 均处于熄灭状态。 */
    ESP_ERROR_CHECK(aw9523b_set_box3_led(AW9523B_BOX3_LED_RED, false));
    ESP_ERROR_CHECK(aw9523b_set_box3_led(AW9523B_BOX3_LED_BLUE, false));

    /* 配置 AW9523B 的 K1/K2 输入变化中断。 */
    ESP_ERROR_CHECK(aw9523b_interrupt_init());

    /*
     * GC0308 与 CHSC5432 共用 AW9523B P1_7 复位信号。摄像头上电期间保持复位，
     * 这里释放复位并等待 200 ms，随后再初始化触摸控制器。
     */
    ESP_ERROR_CHECK(aw9523b_set_box3_touch_camera_reset(false));
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_ERROR_CHECK(tp_init());

    esp_err_t camera_result = camera_init();
    if (camera_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "Camera unavailable; continuing without camera: %s",
                 esp_err_to_name(camera_result));
    }

    /*
     * 触摸和摄像头初始化完成后才启动 GPIO42 中断任务，避免初始化期间出现
     * 触摸 I2C 访问与摄像头 SCCB 配置同时占用 I2C0 的情况。
     */
    ESP_ERROR_CHECK(interrupt_manager_init(INTERRUPT_SOURCE_AW9523B |
                                           INTERRUPT_SOURCE_TOUCH));

    /*
     * LCD 和 SD 共用 SPI2 的 SCLK=GPIO15、MOSI=GPIO16、MISO=GPIO17，
     * 这里只初始化一次总线；两个设备之后分别使用 LCD_CS=GPIO47 和
     * SD_CS=GPIO18。LCD 和 SD 都会注册成 SPI2 上的独立设备，ESP-IDF
     * 内部的 SPI bus lock 会把同时到来的读写请求自动排队，同一时刻只
     * 允许一个设备执行事务，因此不需要手动切换 CS，也不要再增加一把
     * 跨 LCD/SD 的全局 SPI mutex。
     */
    ESP_ERROR_CHECK(board_spi_init(BOARD_SPI_MAX_TRANSFER_SIZE));

    /* 基础外设就绪后初始化 320x240 标准横屏坐标的 LCD。 */
    ESP_ERROR_CHECK(lcd_init());

    /* SD 与 LCD 共用 SPI2；SD 挂载失败时只记录警告，不影响其他功能。 */
    esp_err_t sd_result = sd_init();
    if (sd_result == ESP_OK) {
        /* 挂载成功后执行 SD FATFS 的写入、读回校验和重命名测试。 */
        const esp_err_t sd_test_result = sd_fatfs_test();
        if (sd_test_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "SD FATFS test failed: %s",
                     esp_err_to_name(sd_test_result));
        }
    } else {
        ESP_LOGW(TAG,
                 "SD unavailable; continuing without storage: %s",
                 esp_err_to_name(sd_result));
    }

    /* 挂载内部 Flash 的 vfs FATFS 分区；失败时只记录警告，不触发重启。 */
    esp_err_t fatfs_result = fatfs_init();
    if (fatfs_result == ESP_OK) {
        /* 挂载成功后执行内部 Flash FATFS 的文件读写测试。 */
        const esp_err_t fatfs_test_result = fatfs_test();
        if (fatfs_test_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "Flash FATFS test failed: %s",
                     esp_err_to_name(fatfs_test_result));
        }
    } else {
        ESP_LOGW(TAG,
                 "Flash FATFS unavailable; continuing without storage: %s",
                 esp_err_to_name(fatfs_result));
    }

    /* 挂载内部 Flash 的 LittleFS 分区；失败时只记录警告，不触发重启。 */
    esp_err_t littlefs_result = littlefs_init();
    if (littlefs_result == ESP_OK) {
        /* 挂载成功后执行 LittleFS 的文件读写测试。 */
        const esp_err_t littlefs_test_result = littlefs_test();
        if (littlefs_test_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "LittleFS test failed: %s",
                     esp_err_to_name(littlefs_test_result));
        }
    } else {
        ESP_LOGW(TAG,
                 "LittleFS unavailable; continuing without storage: %s",
                 esp_err_to_name(littlefs_result));
    }

#if 0
    /*
     * LVGL 接入前使用的直接 LCD 绘图示例保留在本代码块中。当前 #if 1
     * 表示启用直接 LCD 测试；由于下面包含无限循环，程序不会继续执行后面的
     * LVGL 初始化。字符显示验证完成后改回 #if 0，即可恢复 LVGL 界面。
     */

    /*
     * 旧版 LCD 八色测试图代码保留在这里。LVGL 接管屏幕时必须关闭，避免
     * LVGL 刷新任务和直接 LCD 绘图同时修改画面。
     */
    ESP_ERROR_CHECK(lcd_show_test_pattern());
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint32_t seconds = 0;
    char uptime_text[40];
    while (true) {

        ESP_ERROR_CHECK(lcd_clear(LCD_COLOR_GREEN));
        vTaskDelay(pdMS_TO_TICKS(1000));
        /*
         * 每次刷新前清除屏幕顶部一整行。标准坐标中 width 沿 X 轴水平
         * 向右，所以宽度使用 320；height 沿 Y 轴竖直向下，所以高度为 16。
         */
        esp_err_t display_result = lcd_fill_rect(0,
                                                  0,
                                                  LCD_X_RESOLUTION,
                                                  16,
                                                  LCD_COLOR_WHITE);

        if (display_result == ESP_OK) {

            snprintf(uptime_text,sizeof(uptime_text),
                "Hello World - uptime: %u s",
                (unsigned)seconds);
            /* 在 Y=0 的第一行中，从左向右显示运行时间。 */
            display_result = lcd_show_string(0,
                                             0,
                                             LCD_X_RESOLUTION,
                                             16,
                                             16,
                                             uptime_text,
                                             LCD_COLOR_BLACK);
        }
        if (display_result == ESP_OK) {
            /* 在 Y=20 的第二行中，从屏幕左侧显示 xc_lcd。 */
            display_result = lcd_show_string(0,
                                             20,
                                             LCD_X_RESOLUTION,
                                             16,
                                             16,
                                             "xc_lcd",
                                             LCD_COLOR_BLACK);
        }
        if (display_result == ESP_OK) {
            /* 在 Y=40 的第三行左侧显示单个字符 A。 */
            display_result = lcd_show_char(0,
                                           40,
                                           'A',
                                           16,
                                           LCD_COLOR_BLACK);
        }
        if (display_result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Failed to update LCD content: %s",
                     esp_err_to_name(display_result));
        }

        seconds += 1;
        vTaskDelay(pdMS_TO_TICKS(1000));

        #if 1
        /* 摄像头初始化或上一帧显示成功时，才继续读取下一帧。 */
        if (camera_result == ESP_OK) {
            /* 显示一帧 320x240 的 GC0308 图像。 */
            camera_result = camera_test();

            /* 相机图像在 LCD 上保留 1 秒，再进入下一轮文字显示。 */
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        #endif
    }

#else

    /*
     * 将 LCD 显示和 CHSC5432 触摸接入 LVGL，然后创建按钮界面。LCD、
     * CHSC5432 和 LVGL 三者的坐标方向及对应关系如下。
     *
     * 1. LCD 逻辑坐标：320x240。
     *
     *        (0,0) ----------------------> LCD_X，范围 0~319
     *          |
     *          |
     *          v
     *        LCD_Y，范围 0~239
     *
     *    LCD 原点位于屏幕左上角，LCD_X 从左向右递增，LCD_Y 从上向下递增。
     *
     * 2. CHSC5432 原始触摸坐标：240x320。
     *
     *                       TP_RAW_X，范围 0~239
     *                                ^
     *                                |
     *                                |
     *        (0,0) ------------------+------> TP_RAW_Y，范围 0~319
     *
     *    触摸原点位于屏幕左下角。TP_RAW_X 对应屏幕竖直方向，从下向上
     *    递增；TP_RAW_Y 对应屏幕水平方向，从左向右递增。
     *
     * 3. LVGL 坐标：320x240。
     *
     *        (0,0) ----------------------> LVGL_X，范围 0~319
     *          |
     *          |
     *          v
     *        LVGL_Y，范围 0~239
     *
     *    LVGL 原点位于屏幕左上角，LVGL_X 从左向右递增，LVGL_Y 从上向下
     *    递增，与 LCD 逻辑坐标完全相同。
     *
     * 显示关系：LVGL 绘制区域直接传给 LCD，不需要交换或反转坐标。
     *
     *        LCD_X = LVGL_X
     *        LCD_Y = LVGL_Y
     *
     * 触摸关系：tp.c 将 CHSC5432 原始坐标转换为 LVGL/LCD 坐标。
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
     *    四个角点的转换结果：
     *
     *        原始左上角 (239,   0) -> 转换后左上角 (  0,   0)
     *        原始右上角 (239, 319) -> 转换后右上角 (319,   0)
     *        原始左下角 (  0,   0) -> 转换后左下角 (  0, 239)
     *        原始右下角 (  0, 319) -> 转换后右下角 (319, 239)
     *
     * 交换 TP_RAW_X/TP_RAW_Y 是因为触摸原始 X 是竖直轴，而 LCD/LVGL 的
     * X 是水平轴；用 239 减 TP_RAW_X 是为了把竖直原点从左下角转换到
     * 左上角。转换后，触摸位置与屏幕上对应的 LVGL 控件坐标一致。
     *
     * CHSC5432 的 I2C 读取只在 GPIO42 中断管理任务中执行。触摸驱动完成
     * 坐标转换后，把完整状态发送到 FreeRTOS 消息队列；LVGL 任务在两次
     * 定时器处理之间阻塞等待该队列，同时负责输入事件和显示刷新。
     */
    ESP_ERROR_CHECK(lvgl_port_init());
    ESP_ERROR_CHECK(lvgl_ui_init());
    ESP_ERROR_CHECK(lvgl_port_start());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

#endif

}
