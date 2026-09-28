/**
 * @file aw9523b.c
 * @brief 正点原子 DNESP32S3 BOX3 板载 AW9523B GPIO 扩展器驱动。
 *
 * 驱动使用 ESP-IDF 5.5 的新版 I2C Master API。初始化阶段会校验芯片 ID，
 * 并按 BOX3 原理图设置 GPIO 模式和方向，但不会自动复位芯片或改写输出寄存器。
 */

#include "aw9523b.h"

#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c.h"

#define AW9523B_I2C_ADDRESS 0x59
#define AW9523B_I2C_FREQUENCY_HZ 400000
#define AW9523B_I2C_TIMEOUT_MS 100
#define AW9523B_INTERRUPT_DEBOUNCE_MS 20

/* AW9523B 数据手册寄存器地址。 */
#define AW9523B_REG_INPUT_PORT0_0x00 0x00
#define AW9523B_REG_INPUT_PORT1_0x01 0x01
#define AW9523B_REG_OUTPUT_PORT0_0x02 0x02
#define AW9523B_REG_OUTPUT_PORT1_0x03 0x03
#define AW9523B_REG_CONFIG_PORT0_0x04 0x04
#define AW9523B_REG_CONFIG_PORT1_0x05 0x05
#define AW9523B_REG_INTERRUPT_PORT0_0x06 0x06
#define AW9523B_REG_INTERRUPT_PORT1_0x07 0x07
#define AW9523B_REG_ID_0x10 0x10
#define AW9523B_REG_CONTROL_0x11 0x11
#define AW9523B_REG_LED_MODE_PORT0_0x12 0x12
#define AW9523B_REG_LED_MODE_PORT1_0x13 0x13
#define AW9523B_REG_SOFT_RESET_0x7F 0x7F

#define AW9523B_EXPECTED_ID 0x23
#define AW9523B_PINS_PER_PORT 8

/* BOX3 固定方向：P0_0/P0_1 是按键输入，其余 14 路都是输出。 */
#define AW9523B_BOX3_PORT0_DIRECTION 0x03
#define AW9523B_BOX3_PORT1_DIRECTION 0x00
#define AW9523B_ALL_PINS_GPIO_MODE 0xFF
#define AW9523B_PORT0_PUSH_PULL_MODE 0x10
#define AW9523B_INTERRUPT_PORT0_PIN_MASK \
    ((uint8_t)(1U << AW9523B_BOX3_KEY_K1) | \
     (uint8_t)(1U << AW9523B_BOX3_KEY_K2))
#define AW9523B_INTERRUPT_PORT1_PIN_MASK ((uint8_t)0x00)

static const char *TAG = "AW9523B";
static board_i2c_device_handle_t aw9523b_device_handle = NULL;
static SemaphoreHandle_t aw9523b_mutex = NULL;
static bool aw9523b_initialized = false;
static uint8_t previous_port0_input_levels = 0;
static TickType_t last_key_change_ticks[AW9523B_PINS_PER_PORT] = {0};
static bool aw9523b_interrupt_initialized = false;
static bool red_led_on = false;
static bool blue_led_on = false;

/** 不加互斥锁的底层寄存器读操作，仅供驱动内部调用。 */
static esp_err_t read_register_unlocked(uint8_t register_address, uint8_t *value)
{
    return board_i2c_transmit_receive(aw9523b_device_handle,
                                      &register_address,
                                      sizeof(register_address),
                                      value,
                                      sizeof(*value),
                                      AW9523B_I2C_TIMEOUT_MS);
}

/** 不加互斥锁的底层寄存器写操作，仅供驱动内部调用。 */
static esp_err_t write_register_unlocked(uint8_t register_address, uint8_t value)
{
    const uint8_t write_data[] = {register_address, value};

    return board_i2c_transmit(aw9523b_device_handle,
                              write_data,
                              sizeof(write_data),
                              AW9523B_I2C_TIMEOUT_MS);
}

/** 不加互斥锁，一次读取 P0 和 P1 的 16 位输入状态。 */
static esp_err_t read_all_inputs_unlocked(uint16_t *input_levels)
{
    const uint8_t register_address = AW9523B_REG_INPUT_PORT0_0x00;
    uint8_t input_data[2] = {0};

    esp_err_t result = board_i2c_transmit_receive(aw9523b_device_handle,
                                                   &register_address,
                                                   sizeof(register_address),
                                                   input_data,
                                                   sizeof(input_data),
                                                   AW9523B_I2C_TIMEOUT_MS);
    if (result == ESP_OK) {
        *input_levels = (uint16_t)input_data[0] |
                        ((uint16_t)input_data[1] << 8);
    }
    return result;
}

static bool key_change_is_valid(aw9523b_p0_pin_t pin,
                                uint8_t current_levels,
                                uint8_t previous_levels)
{
    const uint8_t pin_mask = (uint8_t)(1U << pin);

    /* 当前电平与上次相同，说明这个按键没有变化。 */
    if (((current_levels ^ previous_levels) & pin_mask) == 0) {
        return false;
    }

    const TickType_t now = xTaskGetTickCount();
    if (last_key_change_ticks[pin] != 0 &&
        (now - last_key_change_ticks[pin]) <
            pdMS_TO_TICKS(AW9523B_INTERRUPT_DEBOUNCE_MS)) {
        return false;
    }

    last_key_change_ticks[pin] = now;
    return true;
}

static esp_err_t handle_key_change(uint8_t current_levels,
                                   uint8_t previous_levels)
{
    const uint8_t k1_mask = (uint8_t)(1U << AW9523B_BOX3_KEY_K1);
    const uint8_t k2_mask = (uint8_t)(1U << AW9523B_BOX3_KEY_K2);

    /* K1 默认高电平，变为低电平表示按下。 */
    if (key_change_is_valid(AW9523B_BOX3_KEY_K1,
                            current_levels,
                            previous_levels)) {
        ESP_LOGI(TAG, "K1 level changed: %s",
                 (current_levels & k1_mask) != 0 ? "high" : "low");

        if ((current_levels & k1_mask) == 0) {
            const bool new_state = !red_led_on;
            esp_err_t result = aw9523b_set_box3_led(AW9523B_BOX3_LED_RED,
                                                     new_state);
            if (result != ESP_OK) {
                return result;
            }

            red_led_on = new_state;
            ESP_LOGI(TAG, "K1 pressed, red LED %s",
                     red_led_on ? "on" : "off");
        }
    }
    
    /* K2 默认低电平，变为高电平表示按下。 */
    if (key_change_is_valid(AW9523B_BOX3_KEY_K2,
                            current_levels,
                            previous_levels)) {
        ESP_LOGI(TAG, "K2 level changed: %s",
                 (current_levels & k2_mask) != 0 ? "high" : "low");

        if ((current_levels & k2_mask) != 0) {
            const bool new_state = !blue_led_on;
            esp_err_t result = aw9523b_set_box3_led(AW9523B_BOX3_LED_BLUE,
                                                     new_state);
            if (result != ESP_OK) {
                return result;
            }

            blue_led_on = new_state;
            ESP_LOGI(TAG, "K2 pressed, blue LED %s",
                     blue_led_on ? "on" : "off");
        }
    }

    return ESP_OK;
}

/** 初始化失败时释放本驱动已经创建的资源。 */
static void release_init_resources(void)
{
    if (aw9523b_device_handle != NULL) {
        board_i2c_remove_device(aw9523b_device_handle);
        aw9523b_device_handle = NULL;
    }

    if (aw9523b_mutex != NULL) {
        vSemaphoreDelete(aw9523b_mutex);
        aw9523b_mutex = NULL;
    }
}

/**
 * GPIO42 中断管理任务调用此函数。读取 P0 输入寄存器 0x00 会获得 K1/K2
 * 状态并释放 AW9523B 的 INTN；函数本身不拥有 GPIO ISR 或任务。
 */
esp_err_t aw9523b_interrupt_process(void)
{
    if (!aw9523b_interrupt_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t current_port0_input_levels = 0;
    esp_err_t result = aw9523b_read_register(AW9523B_REG_INPUT_PORT0_0x00,
                                              &current_port0_input_levels);
    if (result != ESP_OK) {
        return result;
    }

    const uint8_t previous_levels = previous_port0_input_levels;
    previous_port0_input_levels = current_port0_input_levels;

    return handle_key_change(current_port0_input_levels, previous_levels);
}

esp_err_t aw9523b_init(void)
{
    if (aw9523b_initialized) {
        return ESP_OK;
    }

    esp_err_t result = board_i2c_add_device(AW9523B_I2C_ADDRESS,
                                             AW9523B_I2C_FREQUENCY_HZ,
                                             &aw9523b_device_handle);
    if (result != ESP_OK) {
        release_init_resources();
        return result;
    }

    uint8_t chip_id = 0;
    result = read_register_unlocked(AW9523B_REG_ID_0x10, &chip_id);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read chip ID: %s", esp_err_to_name(result));
        release_init_resources();
        return result;
    }
    if (chip_id != AW9523B_EXPECTED_ID) {
        ESP_LOGE(TAG, "Unexpected chip ID: 0x%02X, expected 0x%02X",
                 chip_id, AW9523B_EXPECTED_ID);
        release_init_resources();
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * LED mode 寄存器中 1=GPIO 模式，将 P0/P1 的 16 路端口全部作为 GPIO。
     * 这里只修改功能和方向寄存器，不改写输出锁存寄存器 0x02/0x03。
     */
    result = write_register_unlocked(AW9523B_REG_LED_MODE_PORT0_0x12,
                                     AW9523B_ALL_PINS_GPIO_MODE);
    if (result == ESP_OK) {
        result = write_register_unlocked(AW9523B_REG_LED_MODE_PORT1_0x13,
                                         AW9523B_ALL_PINS_GPIO_MODE);
    }

    /* CONTROL bit4=1，使 P0 输出使用推挽模式；其余位采用原例程的默认值。 */
    if (result == ESP_OK) {
        result = write_register_unlocked(AW9523B_REG_CONTROL_0x11,
                                         AW9523B_PORT0_PUSH_PULL_MODE);
    }

    /* 方向位 1=输入、0=输出，所以 0x0003 仅把 K1、K2 设置为输入。 */
    if (result == ESP_OK) {
        result = write_register_unlocked(AW9523B_REG_CONFIG_PORT0_0x04,
                                         AW9523B_BOX3_PORT0_DIRECTION);
    }
    if (result == ESP_OK) {
        result = write_register_unlocked(AW9523B_REG_CONFIG_PORT1_0x05,
                                         AW9523B_BOX3_PORT1_DIRECTION);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure BOX3 GPIO directions: %s",
                 esp_err_to_name(result));
        release_init_resources();
        return result;
    }

    aw9523b_mutex = xSemaphoreCreateMutex();
    if (aw9523b_mutex == NULL) {
        release_init_resources();
        return ESP_ERR_NO_MEM;
    }

    aw9523b_initialized = true;
    ESP_LOGI(TAG, "Ready: address=0x%02X, ID=0x%02X",
             AW9523B_I2C_ADDRESS, chip_id);
    return ESP_OK;
}

esp_err_t aw9523b_read_register(uint8_t register_address, uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!aw9523b_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result;
    xSemaphoreTake(aw9523b_mutex, portMAX_DELAY);
    result = read_register_unlocked(register_address, value);
    xSemaphoreGive(aw9523b_mutex);
    return result;
}

esp_err_t aw9523b_write_register(uint8_t register_address, uint8_t value)
{
    if (!aw9523b_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result;
    xSemaphoreTake(aw9523b_mutex, portMAX_DELAY);
    result = write_register_unlocked(register_address, value);
    xSemaphoreGive(aw9523b_mutex);
    return result;
}

esp_err_t aw9523b_configure_gpio(aw9523b_port_t port,
                                 aw9523b_pin_t pin,
                                 aw9523b_gpio_direction_t direction)
{
    if ((port != AW9523B_PORT_0 && port != AW9523B_PORT_1) ||
        pin >= AW9523B_PINS_PER_PORT ||
        (direction != AW9523B_GPIO_OUTPUT && direction != AW9523B_GPIO_INPUT)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!aw9523b_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result;
    const uint8_t bit_mask = (uint8_t)(1U << pin);
    const uint8_t led_mode_register = port == AW9523B_PORT_1
                                          ? AW9523B_REG_LED_MODE_PORT1_0x13
                                          : AW9523B_REG_LED_MODE_PORT0_0x12;
    const uint8_t config_register = port == AW9523B_PORT_1
                                        ? AW9523B_REG_CONFIG_PORT1_0x05
                                        : AW9523B_REG_CONFIG_PORT0_0x04;

    xSemaphoreTake(aw9523b_mutex, portMAX_DELAY);

    /* LED/GPIO 模式寄存器对应位写 1，确保该引脚工作在 GPIO 模式。 */
    uint8_t led_mode = 0;
    result = read_register_unlocked(led_mode_register, &led_mode);
    if (result == ESP_OK) {
        led_mode |= bit_mask;
        result = write_register_unlocked(led_mode_register, led_mode);
    }

    /* 方向寄存器对应位：1=输入，0=输出。 */
    uint8_t gpio_direction = 0;
    if (result == ESP_OK) {
        result = read_register_unlocked(config_register, &gpio_direction);
    }
    if (result == ESP_OK) {
        if (direction == AW9523B_GPIO_INPUT) {
            gpio_direction |= bit_mask;
        } else {
            gpio_direction &= (uint8_t)~bit_mask;
        }
        result = write_register_unlocked(config_register, gpio_direction);
    }

    xSemaphoreGive(aw9523b_mutex);
    return result;
}

esp_err_t aw9523b_write_gpio(aw9523b_port_t port,
                             aw9523b_pin_t pin,
                             bool high)
{
    if ((port != AW9523B_PORT_0 && port != AW9523B_PORT_1) ||
        pin >= AW9523B_PINS_PER_PORT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!aw9523b_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result;
    const uint8_t bit_mask = (uint8_t)(1U << pin);
    const uint8_t output_register = port == AW9523B_PORT_1
                                        ? AW9523B_REG_OUTPUT_PORT1_0x03
                                        : AW9523B_REG_OUTPUT_PORT0_0x02;

    xSemaphoreTake(aw9523b_mutex, portMAX_DELAY);

    uint8_t output_value = 0;
    result = read_register_unlocked(output_register, &output_value);
    if (result == ESP_OK) {
        if (high) {
            output_value |= bit_mask;
        } else {
            output_value &= (uint8_t)~bit_mask;
        }
        result = write_register_unlocked(output_register, output_value);
    }

    xSemaphoreGive(aw9523b_mutex);
    return result;
}

esp_err_t aw9523b_read_gpio(aw9523b_port_t port,
                            aw9523b_pin_t pin,
                            bool *high)
{
    if ((port != AW9523B_PORT_0 && port != AW9523B_PORT_1) ||
        pin >= AW9523B_PINS_PER_PORT || high == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!aw9523b_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result;
    const uint8_t input_register = port == AW9523B_PORT_1
                                       ? AW9523B_REG_INPUT_PORT1_0x01
                                       : AW9523B_REG_INPUT_PORT0_0x00;

    xSemaphoreTake(aw9523b_mutex, portMAX_DELAY);
    uint8_t input_value = 0;
    result = read_register_unlocked(input_register, &input_value);
    xSemaphoreGive(aw9523b_mutex);

    if (result == ESP_OK) {
        *high = (input_value & (1U << pin)) != 0;
    }
    return result;
}

esp_err_t aw9523b_read_all_inputs(uint16_t *input_levels)
{
    if (input_levels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!aw9523b_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result;
    xSemaphoreTake(aw9523b_mutex, portMAX_DELAY);
    result = read_all_inputs_unlocked(input_levels);
    xSemaphoreGive(aw9523b_mutex);
    return result;
}

esp_err_t aw9523b_set_box3_led(aw9523b_p1_pin_t led, bool on)
{
    if (led != AW9523B_BOX3_LED_RED && led != AW9523B_BOX3_LED_BLUE) {
        return ESP_ERR_INVALID_ARG;
    }

    /* P1 已在 aw9523b_init() 中配置为输出；红蓝 LED 均为低电平点亮。 */
    return aw9523b_write_gpio(AW9523B_PORT_1, led, !on);
}

esp_err_t aw9523b_interrupt_init(void)
{
    if (!aw9523b_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (aw9523b_interrupt_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result;

    /* AW9523B 中断寄存器中 0=使能、1=屏蔽；配置期间先全部屏蔽。 */
    result = aw9523b_write_register(AW9523B_REG_INTERRUPT_PORT0_0x06, 0xFF);
    if (result != ESP_OK) {
        return result;
    }
    result = aw9523b_write_register(AW9523B_REG_INTERRUPT_PORT1_0x07, 0xFF);
    if (result != ESP_OK) {
        return result;
    }

    /* 只有输入模式引脚能够产生 AW9523B 输入变化中断。 */
    for (unsigned pin_number = 0;
         pin_number < AW9523B_PINS_PER_PORT;
         ++pin_number) {
        if ((AW9523B_INTERRUPT_PORT0_PIN_MASK &
             (uint8_t)(1U << pin_number)) == 0) {
            continue;
        }

        result = aw9523b_configure_gpio(AW9523B_PORT_0,
                                         (aw9523b_pin_t)pin_number,
                                         AW9523B_GPIO_INPUT);
        if (result != ESP_OK) {
            return result;
        }
    }

    /* 读取当前电平，同时清除初始化前可能存在的 INTN 状态。 */
    result = aw9523b_read_register(AW9523B_REG_INPUT_PORT0_0x00,
                                    &previous_port0_input_levels);
    if (result != ESP_OK) {
        return result;
    }

    /* 仅将 K1/K2 对应位写 0，其余端口保持屏蔽。 */
    const uint8_t port0_interrupt_mask =
        (uint8_t)~AW9523B_INTERRUPT_PORT0_PIN_MASK;
    const uint8_t port1_interrupt_mask =
        (uint8_t)~AW9523B_INTERRUPT_PORT1_PIN_MASK;
    result = aw9523b_write_register(AW9523B_REG_INTERRUPT_PORT0_0x06,
                                    port0_interrupt_mask);
    if (result == ESP_OK) {
        result = aw9523b_write_register(AW9523B_REG_INTERRUPT_PORT1_0x07,
                                        port1_interrupt_mask);
    }
    if (result != ESP_OK) {
        aw9523b_write_register(AW9523B_REG_INTERRUPT_PORT0_0x06, 0xFF);
        aw9523b_write_register(AW9523B_REG_INTERRUPT_PORT1_0x07, 0xFF);
        return result;
    }

    aw9523b_interrupt_initialized = true;
    ESP_LOGI(TAG, "INTN ready, P0 mask=0x%02X, P1 mask=0x%02X",
             AW9523B_INTERRUPT_PORT0_PIN_MASK,
             AW9523B_INTERRUPT_PORT1_PIN_MASK);
    return ESP_OK;
}

esp_err_t aw9523b_soft_reset(void)
{
    if (!aw9523b_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (aw9523b_interrupt_initialized) {
        ESP_LOGE(TAG, "Disable the AW9523B interrupt before software reset");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result;
    xSemaphoreTake(aw9523b_mutex, portMAX_DELAY);
    result = write_register_unlocked(AW9523B_REG_SOFT_RESET_0x7F, 0x00);
    xSemaphoreGive(aw9523b_mutex);

    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return result;
}
