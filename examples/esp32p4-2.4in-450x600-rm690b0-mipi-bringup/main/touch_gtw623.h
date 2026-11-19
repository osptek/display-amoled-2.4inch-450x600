/*
 * GENIX GTW623 capacitive touch controller (Zinitix-compatible I2C protocol)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} touch_point_t;

/**
 * Reset the controller, find it on the I2C bus and configure it.
 * @param x_max  display width  (written to the chip's X resolution register)
 * @param y_max  display height (written to the chip's Y resolution register)
 */
esp_err_t touch_gtw623_init(uint16_t x_max, uint16_t y_max);

/**
 * Read the first finger. Only touches the bus when INT is asserted (low),
 * otherwise returns the last known state.
 */
esp_err_t touch_gtw623_read(touch_point_t *pt);

#ifdef __cplusplus
}
#endif
