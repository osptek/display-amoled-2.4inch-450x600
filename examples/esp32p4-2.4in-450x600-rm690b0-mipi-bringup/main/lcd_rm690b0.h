/*
 * AM240M450600ZS (2.4" AMOLED, 450x600, RM690B0) over MIPI-DSI 1 lane, ESP32-P4
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_H_RES   450
#define LCD_V_RES   600

typedef struct {
    esp_lcd_dsi_bus_handle_t  dsi_bus;
    esp_lcd_panel_io_handle_t io;      // DBI (DCS command) channel
    esp_lcd_panel_handle_t    panel;   // DPI (video stream) panel
} lcd_rm690b0_t;

/**
 * Power up the PMIC (ENA), reset + init the RM690B0, then start the DPI video stream.
 */
esp_err_t lcd_rm690b0_init(lcd_rm690b0_t *out);

/** Set brightness through DCS 0x51 (0..255) */
esp_err_t lcd_rm690b0_set_brightness(lcd_rm690b0_t *lcd, uint8_t level);

/** Display on/off through DCS 0x29/0x28 */
esp_err_t lcd_rm690b0_display_on(lcd_rm690b0_t *lcd, bool on);

/**
 * Power-off sequence per datasheet: DISPOFF -> SLPIN -> wait >5 frames -> RESX low -> ENA low.
 * Call this before cutting power / entering deep sleep. Re-run lcd_rm690b0_init() flow to power on again.
 */
esp_err_t lcd_rm690b0_power_off(lcd_rm690b0_t *lcd);

#ifdef __cplusplus
}
#endif
