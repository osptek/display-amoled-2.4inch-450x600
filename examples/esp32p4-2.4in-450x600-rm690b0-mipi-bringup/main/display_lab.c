/*
 * Display lab: one flash, many experiments. Each step runs for a few seconds and is logged as
 *   "LAB step N/M: ..."
 * so the person watching the panel can say which step looks right.
 *
 * Hypotheses being tested:
 *  1. Frame-buffer -> DSI bridge stream lags by a few bytes (colors rotated, ~2px right shift,
 *     last pixels of the previous frame wrap to the top-left). Tested by writing the pattern into
 *     the frame buffer shifted by N bytes, with NO color swizzle.
 *  2. The horizontal seam (also visible on the DSI host's own color bar) depends on the DSI video
 *     mode type or on LP transitions during blanking.
 */
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "hal/mipi_dsi_host_ll.h"
#include "lcd_rm690b0.h"
#include "display_lab.h"

#if CONFIG_LCD_DISPLAY_LAB

static const char *TAG = "lab";

#define FB_SIZE (LCD_H_RES * LCD_V_RES * 3)

typedef enum { MODE_BURST, MODE_NB_PULSES, MODE_NB_EVENTS } vmode_t;

static void set_video(vmode_t mode, bool lp)
{
    static const mipi_dsi_ll_video_burst_type_t t[] = {
        MIPI_DSI_LL_VIDEO_BURST_WITH_SYNC_PULSES,
        MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES,
        MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_EVENTS,
    };
    mipi_dsi_host_ll_enable_video_mode(&MIPI_DSI_HOST, false);
    mipi_dsi_host_ll_dpi_set_video_burst_type(&MIPI_DSI_HOST, t[mode]);
    mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(&MIPI_DSI_HOST, lp, lp);
    mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(&MIPI_DSI_HOST, lp, lp, lp, lp);
    mipi_dsi_host_ll_enable_video_mode(&MIPI_DSI_HOST, true);
}

static void put_px(uint8_t *img, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = img + (y * LCD_H_RES + x) * 3;
    p[0] = b;   // LVGL RGB888 memory order
    p[1] = g;
    p[2] = r;
}

// Same pattern as geometry_test(): left RED, right BLUE, top GREEN, bottom YELLOW, grid 50/100
static void build_geometry(uint8_t *img)
{
    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < LCD_H_RES; x++) {
            uint8_t v = 0;
            if (x % 100 == 0 || y % 100 == 0) {
                v = 0xFF;
            } else if (x % 50 == 0 || y % 50 == 0) {
                v = 0x60;
            }
            put_px(img, x, y, v, v, v);
        }
        for (int x = 0; x < LCD_H_RES; x++) {
            if (y < 2) put_px(img, x, y, 0, 0xFF, 0);
            if (y >= LCD_V_RES - 2) put_px(img, x, y, 0xFF, 0xFF, 0);
        }
        put_px(img, 0, y, 0xFF, 0, 0);
        put_px(img, 1, y, 0xFF, 0, 0);
        put_px(img, LCD_H_RES - 2, y, 0, 0, 0xFF);
        put_px(img, LCD_H_RES - 1, y, 0, 0, 0xFF);
    }
}

// 3x5 digit font, drawn big in the middle of the pattern so photos label themselves
static const uint8_t s_digits[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};

static void draw_number(uint8_t *img, int num)
{
    const int scale = 16, w = 3 * scale, h = 5 * scale, gap = scale;
    char txt[4];
    int len = snprintf(txt, sizeof(txt), "%d", num);
    int total = len * w + (len - 1) * gap;
    int x0 = (LCD_H_RES - total) / 2, y0 = 210;
    // dark backing box
    for (int y = y0 - 12; y < y0 + h + 12; y++) {
        for (int x = x0 - 12; x < x0 + total + 12; x++) {
            put_px(img, x, y, 0x20, 0x20, 0x20);
        }
    }
    for (int c = 0; c < len; c++) {
        const uint8_t *g = s_digits[txt[c] - '0'];
        int cx = x0 + c * (w + gap);
        for (int r = 0; r < 5; r++) {
            for (int b = 0; b < 3; b++) {
                if (!(g[r] & (4 >> b))) {
                    continue;
                }
                for (int yy = 0; yy < scale; yy++) {
                    for (int xx = 0; xx < scale; xx++) {
                        put_px(img, cx + b * scale + xx, y0 + r * scale + yy, 0xFF, 0xFF, 0xFF);
                    }
                }
            }
        }
    }
}

// fb[(j - lag) mod N] = img[j]  ->  the panel, which shows fb[j - lag] at position j, shows img[j]
static void write_fb_with_lag(uint8_t *fb, const uint8_t *img, int lag, bool swizzle)
{
    for (int j = 0; j < FB_SIZE; j += 3) {
        uint8_t b = img[j], g = img[j + 1], r = img[j + 2];
        uint8_t o0 = b, o1 = g, o2 = r;
        if (swizzle) {          // the old workaround: (B,G,R) -> (G,R,B)
            o0 = g; o1 = r; o2 = b;
        }
        int k = j - lag;
        fb[(k + FB_SIZE) % FB_SIZE]     = o0;
        fb[(k + 1 + FB_SIZE) % FB_SIZE] = o1;
        fb[(k + 2 + FB_SIZE) % FB_SIZE] = o2;
    }
    esp_cache_msync(fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void display_lab_run(lcd_rm690b0_t *lcd)
{
    void *fbv = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(lcd->panel, 1, &fbv));
    uint8_t *fb = fbv;
    uint8_t *img = heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM);
    assert(img);

    const int ms = CONFIG_LCD_LAB_STEP_SECONDS * 1000;
    static const char *mode_name[] = {"BURST", "NON-BURST pulses", "NON-BURST events"};

    struct {
        bool bar;           // DSI host built-in color bar (no frame buffer involved)
        vmode_t mode;
        bool lp;
        int lag;            // frame buffer byte lag compensation
        bool swizzle;
    } steps[] = {
        // Seam steps (host color bar, various video modes) were run already: the seam moves with the
        // DSI timing / rolls in non-burst mode, so it is a tear between our stream and the panel's own
        // refresh. Only the byte-lag steps are kept, all in burst mode with LP on.
        {false, MODE_BURST, true, 0,  true },   // 1: current firmware (swizzle workaround)
        {false, MODE_BURST, true, 0,  false},   // 2: raw
        {false, MODE_BURST, true, 4,  false},   // 3
        {false, MODE_BURST, true, 7,  false},   // 4: expected fix
        {false, MODE_BURST, true, 10, false},   // 5
    };
    const int n = sizeof(steps) / sizeof(steps[0]);

    ESP_LOGW(TAG, "==== Display lab: %d steps x %ds. Note which step number looks right ====",
             n, CONFIG_LCD_LAB_STEP_SECONDS);
    for (int i = 0; i < n; i++) {
        set_video(steps[i].mode, steps[i].lp);
        if (steps[i].bar) {
            esp_lcd_dpi_panel_set_pattern(lcd->panel, MIPI_DSI_PATTERN_BAR_VERTICAL);
            ESP_LOGW(TAG, "LAB step %d/%d: HOST COLOR BAR | %s | LP %s  -> is the seam there?",
                     i + 1, n, mode_name[steps[i].mode], steps[i].lp ? "on" : "off");
        } else {
            esp_lcd_dpi_panel_set_pattern(lcd->panel, MIPI_DSI_PATTERN_NONE);
            build_geometry(img);
            draw_number(img, i + 1);
            write_fb_with_lag(fb, img, steps[i].lag, steps[i].swizzle);
            ESP_LOGW(TAG, "LAB step %d/%d: GEOMETRY | %s | LP %s | byte lag %d | swizzle %s",
                     i + 1, n, mode_name[steps[i].mode], steps[i].lp ? "on" : "off",
                     steps[i].lag, steps[i].swizzle ? "on" : "off");
        }
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    esp_lcd_dpi_panel_set_pattern(lcd->panel, MIPI_DSI_PATTERN_NONE);
    // back to the configured mode for LVGL
#if CONFIG_LCD_DSI_NON_BURST_PULSES
    set_video(MODE_NB_PULSES, !LAB_LP_DISABLED);
#elif CONFIG_LCD_DSI_NON_BURST_EVENTS
    set_video(MODE_NB_EVENTS, !LAB_LP_DISABLED);
#else
    set_video(MODE_BURST, !LAB_LP_DISABLED);
#endif
    free(img);
    ESP_LOGW(TAG, "==== Display lab done ====");
}

#endif // CONFIG_LCD_DISPLAY_LAB
