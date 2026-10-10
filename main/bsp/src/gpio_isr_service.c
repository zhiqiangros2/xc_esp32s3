#include "gpio_isr_service.h"

#include <stdbool.h>

#include "driver/gpio.h"

/* 本接口只由 app_main() 在启动阶段调用，不需要额外的并发锁。 */
static bool s_service_installed;

esp_err_t board_gpio_isr_service_init(void)
{
    if (s_service_installed) {
        return ESP_OK;
    }

    /*
     * 参数 0 是中断分配标志 intr_alloc_flags，表示不附加
     * ESP_INTR_FLAG_IRAM、ESP_INTR_FLAG_LEVELx 等限制，使用 ESP-IDF 默认的
     * GPIO 中断分配方式；它不是 GPIO 编号，也不是中断优先级。
     */
    const esp_err_t result = gpio_install_isr_service(0);
    if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
        s_service_installed = true;
        return ESP_OK;
    }
    return result;
}
