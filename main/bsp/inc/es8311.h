#ifndef BOARD_ES8311_H
#define BOARD_ES8311_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 初始化 BOX3 板载 ES8311，配置为 16 位双声道 I2S 从机。 */
esp_err_t es8311_init(void);

/** 设置 DAC 输出音量，percent 的有效范围为 0～100。 */
esp_err_t es8311_set_volume(uint8_t percent);

/** 设置 DAC 数字静音。 */
esp_err_t es8311_set_mute(bool muted);

#endif
