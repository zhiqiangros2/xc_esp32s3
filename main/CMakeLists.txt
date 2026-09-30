idf_component_register(
    SRCS
        "main.c"
        "bsp/src/bsp_info.c"
        "bsp/src/i2c.c"
        "bsp/src/interrupt_manager.c"
        "bsp/src/key_interrupt.c"
        "bsp/src/aw9523b.c"
        "bsp/src/spi.c"
        "bsp/src/lcd.c"
        "bsp/src/display.c"
        "bsp/src/sd_fatfs.c"
        "bsp/src/fatfs.c"
        "bsp/src/littlefs.c"
        "bsp/src/tp.c"
        "app/src/lvgl_port.c"
        "app/src/lvgl_ui.c"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_sdspi esp_driver_spi esp_lcd esp_psram esp_system esp_timer fatfs heap littlefs lvgl nvs_flash sdmmc spi_flash wear_levelling
    INCLUDE_DIRS "bsp/inc" "app/inc"
)
