#ifndef INTERRUPT_MANAGER_H
#define INTERRUPT_MANAGER_H

#include <stdint.h>

#include "esp_err.h"

/** GPIO42 上启用 AW9523B 中断处理。 */
#define INTERRUPT_SOURCE_AW9523B 0x0001U

/**
 * @brief 初始化 GPIO42 共享低电平中断和处理任务。
 *
 * source_flags 指定需要查询的设备。ISR 只发送任务通知；I2C 查询和后续处理
 * 均在普通任务上下文中执行。
 */
esp_err_t interrupt_manager_init(uint32_t source_flags);

#endif
