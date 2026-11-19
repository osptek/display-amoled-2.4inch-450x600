/*
 * AM240M450600ZS (2.4" AMOLED, 450x600, RM690B0) over MIPI-DSI 1 lane, ESP32-P4
 *
 * Init sequence taken from reference/2.4_mipi_1lane/国宇/2.4_mipi_1lane.c (vendor, verified on SSD2828 bridge),
 * video timing taken from reference/2.4_mipi_1lane/国宇/2.4_mipi_1lane.dsw.
 */
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_check.h"
#include "lcd_rm690b0.h"
#include "hal/mipi_dsi_host_ll.h"

static const char *TAG = "rm690b0";

// Video timing (from vendor .dsw: Hsync=2 Hbp=40 Hfp=10 / Vsync=2 Vbp=10 Vfp=10)
/*
 * Video timing (pixels / lines).
 * Vendor .dsw (SSD2828): HSYNC 2, HBP 40, HFP 10 / VSYNC 2, VBP 10, VFP 10.
 *
 * The panel positions pixels by counting BYTES after HSYNC, not by time. In burst mode ESP-IDF turns
 * HBP into lane byte clocks: HBP * (lane_Mbps / 8) / pclk_MHz = HBP * 62.5 / 18.75 = HBP * 3.33 bytes.
 *   HBP 40 -> 133 bytes = 44 px + 1 byte -> picture ~13 px right AND RGB rotated by one byte
 *             (needed LCD_COLOR_ORDER_FIX)
 *   HBP 26 ->  87 bytes = 29 px exactly  -> correct position, correct colors, no swizzle
 * If the lane rate or pixel clock changes, re-check: HBP * 3.33 (rounded) must be a multiple of 3.
 * Keep HSYNC + HBP + HFP = 52 so the line length / refresh rate stays the same.
 */
#define LCD_HSYNC   2
#define LCD_HBP     26
#define LCD_HFP     24
#define LCD_VSYNC   2
#define LCD_VBP     10
#define LCD_VFP     10

// VDD_MIPI_DPHY must be 2.5V, from internal LDO channel 3 on most P4 boards
#define MIPI_DPHY_LDO_CHAN       3
#define MIPI_DPHY_LDO_VOLTAGE_MV 2500

typedef struct {
    uint8_t cmd;
    uint8_t data[4];
    uint8_t len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t s_init_cmds[] = {
    // ---- CMD2 password ----
    {0xFE, {0x20}, 1, 0},
    {0xF4, {0x5A}, 1, 0},
    {0xF5, {0x59}, 1, 0},

    // ---- ELVDD/ELVSS (SWIRE PMIC) +/-3.3V ----
    {0xFE, {0x40}, 1, 0},
    {0x12, {0xFE}, 1, 0},
    {0x13, {0x0C}, 1, 0},
    {0xC9, {0x54}, 1, 0},   // normal ELVSS = -3.3V
    {0x96, {0x2B}, 1, 0},   // normal ELVDD = 3.3V
    {0x97, {0x02}, 1, 0},
    {0xA5, {0xFF}, 1, 0},
    {0xAA, {0x2F}, 1, 0},   // HBM ELVDD = 3.7V
    {0xAB, {0x50}, 1, 0},   // HBM ELVSS = -3.7V
    {0x98, {0x00}, 1, 0},
    {0xA7, {0x2B}, 1, 0},   // AOD ELVDD = 3.3V
    {0xA9, {0x54}, 1, 0},   // AOD ELVSS = -3.3V

    // ---- SWIRE timing ----
    {0xFE, {0x80}, 1, 0},
    {0x67, {0x1C}, 1, 0},
    {0x68, {0x04}, 1, 0},

    // ---- User command set ----
    {0xFE, {0x00}, 1, 0},
    {0x66, {0x11}, 1, 0},   // 0x02 HBM, 0x11 normal
    {0x35, {0x00}, 1, 0},   // TE on (V-blank)
    {0x53, {0x20}, 1, 0},   // brightness control on
    {0x51, {0xFF}, 1, 0},   // brightness
    {0x63, {0xFF}, 1, 0},   // HBM brightness
    {0x2A, {0x00, 0x00, (LCD_H_RES - 1) >> 8, (LCD_H_RES - 1) & 0xFF}, 4, 0},  // 0..449
    {0x2B, {0x00, 0x00, (LCD_V_RES - 1) >> 8, (LCD_V_RES - 1) & 0xFF}, 4, 0},  // 0..599

    {0x11, {0}, 0, 120},    // sleep out
    {0x29, {0}, 0, 50},     // display on
};

static void enable_dsi_phy_power(void)
{
    static esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    if (ldo_mipi_phy) {
        return;
    }
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DPHY_LDO_CHAN,
        .voltage_mv = MIPI_DPHY_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY powered on (LDO%d %dmV)", MIPI_DPHY_LDO_CHAN, MIPI_DPHY_LDO_VOLTAGE_MV);
}

static void power_and_reset(void)
{
    uint64_t out_mask = 0;
#if CONFIG_LCD_PIN_RST >= 0
    out_mask |= 1ULL << CONFIG_LCD_PIN_RST;
#endif
#if CONFIG_LCD_PIN_ENA >= 0
    out_mask |= 1ULL << CONFIG_LCD_PIN_ENA;
#endif
#if CONFIG_LCD_PIN_BLK_BOOST_EN >= 0
    out_mask |= 1ULL << CONFIG_LCD_PIN_BLK_BOOST_EN;
#endif
    if (out_mask) {
        gpio_config_t cfg = {
            .pin_bit_mask = out_mask,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
    }
#if CONFIG_LCD_PIN_BLK_BOOST_EN >= 0
    // LCD backlight boost is useless for AMOLED (LEDA/LEDK NC on the adapter), keep it off
    gpio_set_level(CONFIG_LCD_PIN_BLK_BOOST_EN, 0);
#endif
#if CONFIG_LCD_PIN_IC_OK >= 0
    gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_LCD_PIN_IC_OK,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));
#endif

    // Hold the driver IC in reset, PMIC off
#if CONFIG_LCD_PIN_RST >= 0
    gpio_set_level(CONFIG_LCD_PIN_RST, 0);
#endif
#if CONFIG_LCD_PIN_ENA >= 0
    gpio_set_level(CONFIG_LCD_PIN_ENA, 0);
#endif
    vTaskDelay(pdMS_TO_TICKS(20));

    // ENA must be high before the RM690B0 starts driving SWIRE
#if CONFIG_LCD_PIN_ENA >= 0
    gpio_set_level(CONFIG_LCD_PIN_ENA, 1);
    ESP_LOGI(TAG, "PMIC ENA -> high (GPIO%d)", CONFIG_LCD_PIN_ENA);
#else
    ESP_LOGW(TAG, "ENA GPIO not configured, make sure ENA is pulled high externally");
#endif
    vTaskDelay(pdMS_TO_TICKS(20));

    // Reset pulse (low -> high), then wait for the IC to come up
#if CONFIG_LCD_PIN_RST >= 0
    gpio_set_level(CONFIG_LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CONFIG_LCD_PIN_RST, 1);
#endif
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t send_init_cmds(esp_lcd_panel_io_handle_t io)
{
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const lcd_init_cmd_t *c = &s_init_cmds[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, c->cmd, c->len ? c->data : NULL, c->len),
                            TAG, "send cmd 0x%02X failed", c->cmd);
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    return ESP_OK;
}

static void read_panel_id(esp_lcd_panel_io_handle_t io)
{
    uint8_t id[3] = {0};
    // RDDID 0x04; a failure here does not stop bring-up
    if (esp_lcd_panel_io_rx_param(io, 0x04, id, sizeof(id)) == ESP_OK) {
        ESP_LOGI(TAG, "RDDID: %02X %02X %02X", id[0], id[1], id[2]);
    } else {
        ESP_LOGW(TAG, "RDDID read failed (BTA not answered?)");
    }
    uint8_t pwr = 0;
    if (esp_lcd_panel_io_rx_param(io, 0x0A, &pwr, 1) == ESP_OK) {
        // bit7 booster, bit4 sleep out, bit2 display on
        ESP_LOGI(TAG, "Power mode 0x0A = 0x%02X", pwr);
    }
}

esp_err_t lcd_rm690b0_init(lcd_rm690b0_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "null arg");
    memset(out, 0, sizeof(*out));

    enable_dsi_phy_power();
    power_and_reset();

    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 1,
        .lane_bit_rate_mbps = CONFIG_LCD_MIPI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &out->dsi_bus), TAG, "new dsi bus");

    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(out->dsi_bus, &dbi_cfg, &out->io), TAG, "new dbi io");

#if CONFIG_LCD_PIN_RST < 0
    // No hardware reset line (15-pin DSI connector has no RESET), adapter board does RC power-on reset;
    // issue a DCS soft reset so the IC always starts from a known state.
    ESP_LOGI(TAG, "No RST GPIO, sending SWRESET (0x01)");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(out->io, 0x01, NULL, 0), TAG, "swreset");
    vTaskDelay(pdMS_TO_TICKS(120));
#endif

    read_panel_id(out->io);
    ESP_LOGI(TAG, "Sending init sequence");
    ESP_RETURN_ON_ERROR(send_init_cmds(out->io), TAG, "init sequence");
    read_panel_id(out->io);

#if CONFIG_LCD_PIN_IC_OK >= 0
    ESP_LOGI(TAG, "IC_OK (GPIO%d) level = %d", CONFIG_LCD_PIN_IC_OK, gpio_get_level(CONFIG_LCD_PIN_IC_OK));
#endif

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = CONFIG_LCD_DPI_CLK_KHZ / 1000.0f,
        .in_color_format = LCD_COLOR_FMT_RGB888,
        .out_color_format = LCD_COLOR_FMT_RGB888,
        .num_fbs = 1,
        #if CONFIG_LCD_DPI_DISABLE_LP
        .flags.disable_lp = 1,
#endif
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_pulse_width = LCD_HSYNC,
            .hsync_back_porch = LCD_HBP,
            .hsync_front_porch = LCD_HFP,
            .vsync_pulse_width = LCD_VSYNC,
            .vsync_back_porch = LCD_VBP,
            .vsync_front_porch = LCD_VFP,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(out->dsi_bus, &dpi_cfg, &out->panel), TAG, "new dpi panel");
#if CONFIG_LCD_DSI_NON_BURST_PULSES
    mipi_dsi_host_ll_dpi_set_video_burst_type(&MIPI_DSI_HOST, MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES);
    ESP_LOGW(TAG, "DSI video mode: NON-BURST with sync pulses");
#elif CONFIG_LCD_DSI_NON_BURST_EVENTS
    mipi_dsi_host_ll_dpi_set_video_burst_type(&MIPI_DSI_HOST, MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_EVENTS);
    ESP_LOGW(TAG, "DSI video mode: NON-BURST with sync events");
#else
    ESP_LOGI(TAG, "DSI video mode: burst with sync pulses");
#endif
#if CONFIG_LCD_DPI_DISABLE_LP
    ESP_LOGW(TAG, "DSI: LP disabled during blanking");
#endif
    // starts the video stream
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(out->panel), TAG, "dpi init");

    ESP_LOGI(TAG, "Timing H: sync %d bp %d fp %d | V: sync %d bp %d fp %d",
             LCD_HSYNC, LCD_HBP, LCD_HFP, LCD_VSYNC, LCD_VBP, LCD_VFP);
    ESP_LOGI(TAG, "Panel up: %dx%d, 1 lane @ %d Mbps, pclk %.2f MHz",
             LCD_H_RES, LCD_V_RES, CONFIG_LCD_MIPI_LANE_MBPS, CONFIG_LCD_DPI_CLK_KHZ / 1000.0f);
    return ESP_OK;
}

esp_err_t lcd_rm690b0_set_brightness(lcd_rm690b0_t *lcd, uint8_t level)
{
    return esp_lcd_panel_io_tx_param(lcd->io, 0x51, &level, 1);
}

esp_err_t lcd_rm690b0_display_on(lcd_rm690b0_t *lcd, bool on)
{
    return esp_lcd_panel_io_tx_param(lcd->io, on ? 0x29 : 0x28, NULL, 0);
}

esp_err_t lcd_rm690b0_power_off(lcd_rm690b0_t *lcd)
{
    ESP_RETURN_ON_FALSE(lcd && lcd->io, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    // 1. DISPOFF + SLPIN: the IC turns ELVDD/ELVSS off through SWIRE
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd->io, 0x28, NULL, 0), TAG, "dispoff");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(lcd->io, 0x10, NULL, 0), TAG, "slpin");

    // 2. wait > 5 frames (>83ms @60Hz) for the OLED rails to discharge
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3. RESX low
#if CONFIG_LCD_PIN_RST >= 0
    gpio_set_level(CONFIG_LCD_PIN_RST, 0);
#endif
    vTaskDelay(pdMS_TO_TICKS(10));

    // 4. power down the AMOLED PMIC (VCI/VDDIO are fixed 3V3 on this board, cannot be switched)
#if CONFIG_LCD_PIN_ENA >= 0
    gpio_set_level(CONFIG_LCD_PIN_ENA, 0);
#endif
    ESP_LOGI(TAG, "Panel powered off");
    return ESP_OK;
}
