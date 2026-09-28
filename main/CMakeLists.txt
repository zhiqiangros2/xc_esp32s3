idf_component_register(
    SRCS
        "main.c"
        "src/i2c.c"
        "src/interrupt_manager.c"
        "src/key_interrupt.c"
        "src/aw9523b.c"
        "src/spi.c"
        "src/lcd.c"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd esp_psram esp_system heap spi_flash
    INCLUDE_DIRS "inc"
)
