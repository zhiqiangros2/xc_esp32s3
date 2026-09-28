idf_component_register(
    SRCS
        "main.c"
        "src/i2c.c"
        "src/interrupt_manager.c"
        "src/key_interrupt.c"
        "src/aw9523b.c"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_psram esp_system heap spi_flash
    INCLUDE_DIRS "inc"
)
