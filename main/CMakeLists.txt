idf_component_register(
    SRCS
        "main.c"
        "src/bsp_info.c"
        "src/i2c.c"
        "src/interrupt_manager.c"
        "src/key_interrupt.c"
        "src/aw9523b.c"
        "src/spi.c"
        "src/lcd.c"
        "src/display.c"
        "src/sd_fatfs.c"
        "src/fatfs.c"
        "src/littlefs.c"
        "src/tp.c"
        "src/lvgl_port.c"
        "src/lvgl_ui.c"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_sdspi esp_driver_spi esp_lcd esp_psram esp_system esp_timer fatfs heap littlefs lvgl nvs_flash sdmmc spi_flash wear_levelling
    INCLUDE_DIRS "inc"
)
