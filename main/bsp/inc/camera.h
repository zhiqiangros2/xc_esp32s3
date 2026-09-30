#ifndef BOARD_CAMERA_H
#define BOARD_CAMERA_H

#include "esp_err.h"

/**
 * @brief 打开 BOX3 板载 GC0308 的供电。
 *
 * @details 调用前必须先完成 board_i2c_init()、aw9523b_init() 和
 * aw9523b_enable_box3_power()。本函数先把与 CHSC5432 共用的 P1_7 复位脚
 * 拉低，再通过 P1_6 使能 VDD_2V8。必须在 tp_init() 之前调用，使 GC0308
 * 在复位保持期间稳定上电；tp_init() 随后负责释放共享复位。重复调用安全。
 *
 * @return ESP_OK 表示供电已经使能；I2C/AW9523B 操作失败时返回对应错误码。
 */
esp_err_t camera_power_on(void);

/**
 * @brief 初始化 BOX3 板载 GC0308 摄像头。
 *
 * @details 调用本函数前必须先完成 board_i2c_init() 和 aw9523b_init()。
 * 摄像头的 SCCB 配置接口与 AW9523B 共用 I2C0；图像数据则通过独立的
 * 8 位 DVP 总线送入 ESP32-S3，不占用 LCD/SD 使用的 SPI2。
 *
 * camera_power_on() 已在共享复位保持为低电平时打开摄像头电源，tp_init()
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
