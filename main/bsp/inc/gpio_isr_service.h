#ifndef BSP_GPIO_ISR_SERVICE_H
#define BSP_GPIO_ISR_SERVICE_H

#include "esp_err.h"

/**
 * @brief 安装全局 GPIO ISR 服务。
 *
 * ESP-IDF 的 GPIO ISR 服务在整个芯片上只能安装一次。app_main() 在初始化
 * 具体 GPIO 中断驱动之前调用本函数，板载 K0 和 GPIO42 随后共享该服务。
 */
esp_err_t board_gpio_isr_service_init(void);

#endif
