#include "interrupt_manager.h"

#include <stdbool.h>

#include "aw9523b.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tp.h"

#define SHARED_INTERRUPT_GPIO GPIO_NUM_42
#define INTERRUPT_RETRY_DELAY_MS 10
#define INTERRUPT_SUPPORTED_SOURCES \
    (INTERRUPT_SOURCE_AW9523B | INTERRUPT_SOURCE_TOUCH)

static const char *TAG = "INT_MGR";
static TaskHandle_t manager_task_handle = NULL;
static uint32_t enabled_source_flags = 0;
static bool manager_initialized = false;

static void IRAM_ATTR shared_gpio_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t task_woken = pdFALSE;

    vTaskNotifyGiveFromISR(manager_task_handle, &task_woken);
    if (task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void interrupt_manager_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        //ESP_LOGI(TAG, "GPIO42 interrupt triggered");

        /*
         * GPIO42 是多个低电平有效、开漏中断源的线与结果。无法通过 GPIO 电平
         * 判断具体来源，因此每轮都查询标志位启用的所有设备。只要线路仍为低电平，
         * 就继续查询，避免 I2C 瞬时失败导致中断无法释放且不再产生下降沿。
         */
        while (true) {
            
            if ((enabled_source_flags & INTERRUPT_SOURCE_TOUCH) != 0) {
                esp_err_t result = tp_interrupt_process();
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "CHSC5432 interrupt query failed: %s",
                             esp_err_to_name(result));
                }
            }

            if ((enabled_source_flags & INTERRUPT_SOURCE_AW9523B) != 0) {
                esp_err_t result = aw9523b_interrupt_process();
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "AW9523B interrupt query failed: %s",
                             esp_err_to_name(result));
                }
            }

            if (gpio_get_level(SHARED_INTERRUPT_GPIO) != 0) {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(INTERRUPT_RETRY_DELAY_MS));
        }
    }
}

esp_err_t interrupt_manager_init(uint32_t source_flags)
{
    if (source_flags == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((source_flags & ~INTERRUPT_SUPPORTED_SOURCES) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (manager_initialized) {
        if (source_flags != enabled_source_flags) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    enabled_source_flags = source_flags;

    const gpio_config_t gpio_config_data = {
        .pin_bit_mask = 1ULL << SHARED_INTERRUPT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t result = gpio_config(&gpio_config_data);
    if (result != ESP_OK) {
        enabled_source_flags = 0;
        return result;
    }

    BaseType_t task_result = xTaskCreate(interrupt_manager_task,
                                         "interrupt_manager",
                                         4096,
                                         NULL,
                                         5,
                                         &manager_task_handle);
    if (task_result != pdPASS) {
        manager_task_handle = NULL;
        enabled_source_flags = 0;
        return ESP_ERR_NO_MEM;
    }

    result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        vTaskDelete(manager_task_handle);
        manager_task_handle = NULL;
        enabled_source_flags = 0;
        return result;
    }

    result = gpio_isr_handler_add(SHARED_INTERRUPT_GPIO,
                                  shared_gpio_isr_handler,
                                  NULL);
    if (result != ESP_OK) {
        vTaskDelete(manager_task_handle);
        manager_task_handle = NULL;
        enabled_source_flags = 0;
        return result;
    }

    manager_initialized = true;
    if (gpio_get_level(SHARED_INTERRUPT_GPIO) == 0) {
        xTaskNotifyGive(manager_task_handle);
    }

    ESP_LOGI(TAG, "Shared interrupt ready on GPIO42, flags=0x%04X",
             (unsigned)enabled_source_flags);
    return ESP_OK;
}
