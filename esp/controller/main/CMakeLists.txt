idf_component_register(SRCS "main.c"
                            "ble_kbd.c"
                            "buttons.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio
                       PRIV_REQUIRES bt esp_hid nvs_flash)
