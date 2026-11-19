idf_component_register(SRCS "esp32p4-2.4in-450x600-rm690b0-mipi-bringup.c"
                            "lcd_rm690b0.c"
                            "touch_gtw623.c"
                            "ui_touch_test.c"
                            "display_lab.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_timer esp_mm)
