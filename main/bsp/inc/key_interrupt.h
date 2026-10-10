#ifndef KEY_INTERRUPT_H
#define KEY_INTERRUPT_H

#include "esp_err.h"

/**
 * @brief 初始化 BOX3 板载 K0 按键的 GPIO0 下降沿中断。
 *
 * 调用前必须已通过 board_gpio_isr_service_init() 安装全局 GPIO ISR 服务。
 *
 * @return ESP_OK 初始化成功或已经初始化；其他值表示初始化失败。
 */
esp_err_t key_interrupt_init(void);

#endif
