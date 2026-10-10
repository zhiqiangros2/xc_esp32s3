#ifndef BOARD_CAMERA_H
#define BOARD_CAMERA_H

#include <stddef.h>

#include "esp_err.h"

/** GC0308 当前固定输出的 QVGA 图像尺寸和一帧 RGB565 字节数。 */
#define CAMERA_FRAME_WIDTH 320U
#define CAMERA_FRAME_HEIGHT 240U
#define CAMERA_RGB565_BYTES_PER_PIXEL 2U
#define CAMERA_FRAME_BUFFER_SIZE \
    (CAMERA_FRAME_WIDTH * CAMERA_FRAME_HEIGHT * \
     CAMERA_RGB565_BYTES_PER_PIXEL)

/**
 * @brief 初始化 BOX3 板载 GC0308 摄像头。
 *
 * @details 调用本函数前必须先完成 board_i2c_init()、aw9523b_init()、
 * aw9523b_enable_box3_power() 和 aw9523b_enable_box3_camera_power()。
 * 摄像头的 SCCB 配置接口与 AW9523B 共用 I2C0；图像数据则通过独立的
 * 8 位 DVP 总线送入 ESP32-S3，不占用 LCD/SD 使用的 SPI2。
 *
 * aw9523b_enable_box3_camera_power() 已在共享复位保持为低电平时打开摄像头电源，tp_init()
 * 随后通过 AW9523B P1_7 完成 CHSC5432 和 GC0308 的共用硬件复位。本函数
 * 必须在这两个步骤之后调用，并依次执行以下操作：
 *
 *  1. 保持摄像头电源和共用复位脚 P1_7 不变；
 *  2. 复用已经初始化的 I2C0，通过 SCCB 写入 GC0308 配置寄存器；
 *  3. 配置 QVGA 320x240、RGB565 和两个 PSRAM 帧缓冲区。
 *
 * AW9523B P1_7 同时连接 CHSC5432 触摸控制器和摄像头复位端，因此本函数
 * 不再操作 P1_7，防止破坏 tp_init() 已完成的触摸配置。
 *
 * @return ESP_OK 初始化成功或已经初始化；电源控制、SCCB 通信、传感器
 * 识别或摄像头资源分配失败时返回对应错误码。
 */
esp_err_t camera_init(void);

/**
 * @brief 阻塞等待一帧中断采集完成的 RGB565 图像，并复制到调用者缓冲区。
 *
 * @details `esp32-camera` 在 `camera_init()` 中已经建立 VSYNC/PCLK、GDMA
 * 中断和内部帧队列。摄像头驱动的高优先级任务永久阻塞在中断事件队列上；
 * VSYNC 和 DMA 完成中断到达后，驱动才组装一帧并放入帧缓冲队列。因此本
 * 接口不是周期查询摄像头寄存器，也不需要应用层再安装 GPIO 中断。
 *
 * 本函数调用 `esp_camera_fb_get()` 阻塞等待驱动帧队列，然后校验图像必须是
 * 320x240 RGB565，把 153600 字节复制到 destination，最后立即归还驱动帧
 * 缓冲。调用者因此可以在函数返回后继续持有自己的图像副本，而不会被下一
 * 次摄像头 DMA 覆盖。
 *
 * `esp_camera_fb_get()` 使用组件内部约 4 秒超时。等待期间当前任务休眠，
 * 不占用 CPU；摄像头无帧或硬件异常时返回 ESP_FAIL。
 *
 * @param[out] destination 接收完整 RGB565 图像的缓冲区。
 * @param[in] destination_size 缓冲区容量，至少为 CAMERA_FRAME_BUFFER_SIZE。
 * @return ESP_OK 成功；参数、初始化状态、图像格式或取帧失败时返回错误码。
 */
esp_err_t camera_read_rgb565_frame(void *destination,
                                    size_t destination_size);

/**
 * @brief 采集一帧 GC0308 图像，并显示到整个 320x240 LCD。
 *
 * @details 本函数是摄像头显示测试，调用前必须先完成 board_spi_init()、
 * lcd_init() 和 camera_init()。函数从摄像头取得一帧 320x240 RGB565
 * 图像，调用 LCD 驱动覆盖刷新整个屏幕，然后立即把帧缓冲归还给摄像头
 * 驱动。帧缓冲位于 PSRAM，不能在归还后继续访问。
 *
 * 摄像头输出与 LCD 使用相同的逐行坐标顺序：
 *
 *                         X 向右，0~319
 *       (0,0) +----------------------------------+
 *             | frame buffer 第 0 行             |
 *             | frame buffer 第 1 行             |
 *             | ...                              |
 *             +----------------------------------+
 *             |
 *             v Y 向下，0~239
 *
 * GC0308 已经按高字节在前输出 RGB565。测试函数使用 LCD 的摄像头字节流
 * 接口直接分批复制和发送，不再交换每个像素的两个字节。
 *
 * @return ESP_OK 表示一帧图像已经完整显示；发生错误时返回
 * ESP_ERR_INVALID_STATE、ESP_ERR_INVALID_RESPONSE、ESP_FAIL 或 LCD 驱动
 * 返回的错误码。
 */
esp_err_t camera_test(void);

#endif
