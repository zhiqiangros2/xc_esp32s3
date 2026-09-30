#ifndef AW9523B_H
#define AW9523B_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** AW9523B Port 0 的 GPIO 编号。 */
typedef enum {
    AW9523B_PIN_P0_0 = 0,
    AW9523B_PIN_P0_1 = 1,
    AW9523B_PIN_P0_2 = 2,
    AW9523B_PIN_P0_3 = 3,
    AW9523B_PIN_P0_4 = 4,
    AW9523B_PIN_P0_5 = 5,
    AW9523B_PIN_P0_6 = 6,
    AW9523B_PIN_P0_7 = 7,
} aw9523b_p0_pin_t;

/** AW9523B Port 1 的 GPIO 编号。 */
typedef enum {
    AW9523B_PIN_P1_0 = 0,
    AW9523B_PIN_P1_1 = 1,
    AW9523B_PIN_P1_2 = 2,
    AW9523B_PIN_P1_3 = 3,
    AW9523B_PIN_P1_4 = 4,
    AW9523B_PIN_P1_5 = 5,
    AW9523B_PIN_P1_6 = 6,
    AW9523B_PIN_P1_7 = 7,
} aw9523b_p1_pin_t;

/** AW9523B GPIO 端口。 */
typedef enum {
    AW9523B_PORT_0 = 0,
    AW9523B_PORT_1 = 1,
} aw9523b_port_t;

/** 单个端口内的 GPIO 编号，范围为 0～7。 */
typedef uint8_t aw9523b_pin_t;

/** GPIO 方向；AW9523B 方向寄存器中 0 表示输出，1 表示输入。 */
typedef enum {
    AW9523B_GPIO_OUTPUT = 0,
    AW9523B_GPIO_INPUT = 1,
} aw9523b_gpio_direction_t;

#define AW9523B_BOX3_KEY_K1 AW9523B_PIN_P0_0
#define AW9523B_BOX3_KEY_K2 AW9523B_PIN_P0_1
#define AW9523B_BOX3_BAT_CHARGE_ENABLE AW9523B_PIN_P0_2
#define AW9523B_BOX3_BAT_CHARGE AW9523B_PIN_P0_3
#define AW9523B_BOX3_ADC_SELECT AW9523B_PIN_P0_4
#define AW9523B_BOX3_PA_CONTROL AW9523B_PIN_P0_5
#define AW9523B_BOX3_EXT_GPIO0 AW9523B_PIN_P0_6
#define AW9523B_BOX3_EXT_GPIO1 AW9523B_PIN_P0_7
#define AW9523B_BOX3_LCD_BACKLIGHT AW9523B_PIN_P1_0
#define AW9523B_BOX3_LED_RED AW9523B_PIN_P1_1
#define AW9523B_BOX3_LED_BLUE AW9523B_PIN_P1_2
#define AW9523B_BOX3_VDD_3V3_ENABLE AW9523B_PIN_P1_3
#define AW9523B_BOX3_VBAT_ENABLE AW9523B_PIN_P1_4
#define AW9523B_BOX3_VDDA_3V3_ENABLE AW9523B_PIN_P1_5
#define AW9523B_BOX3_VDD_2V8_ENABLE AW9523B_PIN_P1_6
#define AW9523B_BOX3_TOUCH_CAMERA_RESET AW9523B_PIN_P1_7

/**
 * @brief 初始化 AW9523B，并读取芯片 ID 进行验证。
 *
 * 调用前必须先执行 board_i2c_init()。AW9523B 使用 7 位地址 0x59 和 400 kHz
 * 总线速率。初始化不会执行软件复位。驱动会把 P0_0/P0_1 配置为输入，其余
 * 14 路配置为输出；不会改写输出锁存寄存器中的电平。
 */
esp_err_t aw9523b_init(void);

/**
 * @brief 打开 BOX3 的 VBAT 和模拟 3.3 V 电源。
 *
 * 调用前必须先执行 aw9523b_init()。函数将 P1_4/VBAT_EN 和
 * P1_5/VDDA_3V3_EN 显式拉高；重复调用安全。
 *
 * @return ESP_OK 表示两个电源使能均已拉高；否则返回 I2C 操作错误码。
 */
esp_err_t aw9523b_enable_box3_power(void);

/** 从指定寄存器读取一个字节。 */
esp_err_t aw9523b_read_register(uint8_t register_address, uint8_t *value);

/** 向指定寄存器写入一个字节。 */
esp_err_t aw9523b_write_register(uint8_t register_address, uint8_t value);

/** 将指定端口的引脚切换到 GPIO 模式，并配置为输入或输出。 */
esp_err_t aw9523b_configure_gpio(aw9523b_port_t port,
                                 aw9523b_pin_t pin,
                                 aw9523b_gpio_direction_t direction);

/** 写入指定端口 GPIO 的输出锁存电平；调用前应先将引脚配置为输出。 */
esp_err_t aw9523b_write_gpio(aw9523b_port_t port,
                             aw9523b_pin_t pin,
                             bool high);

/** 读取指定端口 GPIO 的当前实际电平。 */
esp_err_t aw9523b_read_gpio(aw9523b_port_t port,
                            aw9523b_pin_t pin,
                            bool *high);

/** 一次读取 P0/P1 的全部输入状态；bit0=P0_0，bit15=P1_7。 */
esp_err_t aw9523b_read_all_inputs(uint16_t *input_levels);

/**
 * 控制 BOX3 板载红灯或蓝灯。on=true 点亮，驱动会处理低电平有效逻辑。
 */
esp_err_t aw9523b_set_box3_led(aw9523b_p1_pin_t led, bool on);

/**
 * @brief 配置 AW9523B INTN。
 *
 * 固定监听 BOX3 的 K1、K2 输入，当前实例只允许初始化一次。
 */
esp_err_t aw9523b_interrupt_init(void);

/**
 * @brief 读取并处理 AW9523B 中断状态。
 *
 * 由 GPIO42 中断管理任务调用；读取输入寄存器会释放 AW9523B 的 INTN。
 */
esp_err_t aw9523b_interrupt_process(void);

/**
 * @brief 写 0x00 到寄存器 0x7F，对 AW9523B 执行软件复位。
 *
 * @warning 软件复位会恢复全部扩展端口配置，可能改变板上电源控制信号。
 */
esp_err_t aw9523b_soft_reset(void);

#endif
