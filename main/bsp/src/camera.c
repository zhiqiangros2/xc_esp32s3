#include "camera.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "hal/i2c_types.h"
#include "lcd.h"

/* GC0308 的 8 位 DVP 数据总线。D0~D7 必须与原理图的连线顺序一致。 */
#define CAMERA_D0_GPIO GPIO_NUM_7
#define CAMERA_D1_GPIO GPIO_NUM_8
#define CAMERA_D2_GPIO GPIO_NUM_9
#define CAMERA_D3_GPIO GPIO_NUM_10
#define CAMERA_D4_GPIO GPIO_NUM_11
#define CAMERA_D5_GPIO GPIO_NUM_12
#define CAMERA_D6_GPIO GPIO_NUM_4
#define CAMERA_D7_GPIO GPIO_NUM_5

/* DVP 帧同步、行有效和像素时钟输入。 */
#define CAMERA_VSYNC_GPIO GPIO_NUM_6
#define CAMERA_HREF_GPIO GPIO_NUM_46
#define CAMERA_PCLK_GPIO GPIO_NUM_45

/* 板上 24 MHz 有源晶振直接驱动 GC0308，因此 ESP32-S3 不输出 XCLK。 */
#define CAMERA_XCLK_GPIO GPIO_NUM_NC
#define CAMERA_XCLK_FREQUENCY_HZ 24000000U

static const char *TAG = "CAMERA";
static bool camera_initialized = false;

/*
 * GC0308 输出 320x240 RGB565。SCCB 的 SDA/SCL 设置为 -1，表示不创建新的
 * I2C 总线，而是通过 sccb_i2c_port 取得 main.c 已经初始化的 I2C0。
 */
static const camera_config_t camera_config = {
    .pin_pwdn = GPIO_NUM_NC,
    .pin_reset = GPIO_NUM_NC,
    .pin_xclk = CAMERA_XCLK_GPIO,
    .pin_sccb_sda = GPIO_NUM_NC,
    .pin_sccb_scl = GPIO_NUM_NC,
    .sccb_i2c_port = I2C_NUM_0,
    .pin_d7 = CAMERA_D7_GPIO,
    .pin_d6 = CAMERA_D6_GPIO,
    .pin_d5 = CAMERA_D5_GPIO,
    .pin_d4 = CAMERA_D4_GPIO,
    .pin_d3 = CAMERA_D3_GPIO,
    .pin_d2 = CAMERA_D2_GPIO,
    .pin_d1 = CAMERA_D1_GPIO,
    .pin_d0 = CAMERA_D0_GPIO,
    .pin_vsync = CAMERA_VSYNC_GPIO,
    .pin_href = CAMERA_HREF_GPIO,
    .pin_pclk = CAMERA_PCLK_GPIO,
    .xclk_freq_hz = CAMERA_XCLK_FREQUENCY_HZ,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    /*
     * 设置每个像素的编码格式。PIXFORMAT_RGB565 表示一个像素占 16 bit：
     * 红色 5 bit、绿色 6 bit、蓝色 5 bit。
     */
    .pixel_format = PIXFORMAT_RGB565,
    /*
     * 设置一帧图像的分辨率。FRAMESIZE_QVGA 对应宽 320 像素、高 240 像素，
     * 即 X=0~319、Y=0~239。该参数只决定尺寸，不决定像素颜色格式。
     */
    .frame_size = FRAMESIZE_QVGA,
    /* RGB565 模式不使用 JPEG，此字段保留驱动示例的默认值。 */
    .jpeg_quality = 12,
    /* 两个帧缓冲区允许摄像头采集与 LCD 显示交替进行。 */
    .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

esp_err_t camera_init(void)
{
    if (camera_initialized) {
        return ESP_OK;
    }

    esp_err_t result = esp_camera_init(&camera_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to initialize GC0308: %s",
                 esp_err_to_name(result));
        return result;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    /* esp_camera_init() 成功后应该能够取得传感器控制对象。 */
    if (sensor == NULL) {
        ESP_LOGE(TAG, "Camera sensor handle is NULL");
        esp_camera_deinit();
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * BOX3 板载传感器应为 GC0308。控制对象存在但 PID 不匹配时，说明实际
     * 连接的传感器型号与当前驱动配置不一致。
     */
    const uint16_t detected_pid = sensor->id.PID;
    if (detected_pid != GC0308_PID) {
        ESP_LOGE(TAG,
                 "Unexpected camera sensor: expected GC0308 PID=0x%02X, detected=0x%02X",
                 GC0308_PID,
                 detected_pid);
        esp_camera_deinit();
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * 当前 GC0308 输出 QVGA 图像，宽度为 320 像素，高度为 240 像素。
     * 摄像头帧缓冲和 LCD 使用相同的左上角原点及坐标方向：
     *
     *      GC0308 帧缓冲坐标                    LCD 显示坐标
     *
     *      (0,0) -------> CAMERA_X=319         (0,0) -------> LCD_X=319
     *        |                                     |
     *        |                                     |
     *        v                                     v
     *      CAMERA_Y=239                          LCD_Y=239
     *
     * 两边都是从左到右保存一行，再从上到下保存下一行，因此直接对应：
     *
     *      LCD_X = CAMERA_X
     *      LCD_Y = CAMERA_Y
     *
     * lcd_draw_rgb565_bytes() 只处理 RGB565 字节流传输，不交换 X/Y、不旋转
     * 图像，也不改变坐标方向。下面关闭垂直翻转和水平镜像后，LCD 显示的就是
     * GC0308 模块当前安装方向下的自然图像。
     */

    /* 参数 0 表示不垂直翻转，上下方向保持 GC0308 的自然输出方向。 */
    int sensor_setting_result = sensor->set_vflip(sensor, 0);
    if (sensor_setting_result != 0) {
        ESP_LOGE(TAG, "Failed to disable GC0308 vertical flip");
        esp_camera_deinit();
        return ESP_FAIL;
    }

    /* 参数 0 表示不水平镜像，图像左右方向保持不变。 */
    sensor_setting_result = sensor->set_hmirror(sensor, 0);
    if (sensor_setting_result != 0) {
        ESP_LOGE(TAG, "Failed to disable GC0308 horizontal mirror");
        esp_camera_deinit();
        return ESP_FAIL;
    }

    /* 参数 0 使用 GC0308 的默认对比度，不额外增强或减弱明暗差异。 */
    sensor_setting_result = sensor->set_contrast(sensor, 0);
    if (sensor_setting_result != 0) {
        ESP_LOGE(TAG, "Failed to set GC0308 default contrast");
        esp_camera_deinit();
        return ESP_FAIL;
    }

    camera_initialized = true;
    ESP_LOGI(TAG,
             "GC0308 ready: 320x240 RGB565, DVP 8-bit, 2 PSRAM frame buffers");
    return ESP_OK;
}

esp_err_t camera_test(void)
{
    if (!camera_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * esp_camera_fb_get() 取得摄像头驱动拥有的帧缓冲区。当前帧显示完成前
     * 不能调用 esp_camera_fb_return()，否则摄像头可能提前覆盖图像数据。
     */
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == NULL) {
        ESP_LOGE(TAG, "Failed to capture a camera frame");
        return ESP_FAIL;
    }

    const size_t expected_frame_size =
        (size_t)LCD_X_RESOLUTION * LCD_Y_RESOLUTION * sizeof(uint16_t);
    if (frame->format != PIXFORMAT_RGB565 ||
        frame->width != LCD_X_RESOLUTION ||
        frame->height != LCD_Y_RESOLUTION ||
        frame->len < expected_frame_size) {
        ESP_LOGE(TAG,
                 "Invalid frame: format=%d, size=%ux%u, bytes=%u",
                 frame->format,
                 frame->width,
                 frame->height,
                 (unsigned)frame->len);
        esp_camera_fb_return(frame);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * GC0308 帧中每个 RGB565 像素已经是高字节在前。使用专用 LCD 接口
     * 原样发送，避免把已经正确的高低字节顺序再次交换。
     */
    const esp_err_t result = lcd_draw_rgb565_bytes(0,
                                                    0,
                                                    frame->width,
                                                    frame->height,
                                                    frame->buf);
    esp_camera_fb_return(frame);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to display camera frame: %s",
                 esp_err_to_name(result));
        return result;
    }

    return ESP_OK;
}
