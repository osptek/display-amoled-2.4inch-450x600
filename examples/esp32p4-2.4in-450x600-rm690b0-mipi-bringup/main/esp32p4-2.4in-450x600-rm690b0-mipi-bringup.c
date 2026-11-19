/*
 * ESP32-P4 + AM240M450600ZS (2.4" AMOLED 450x600, RM690B0, MIPI-DSI 1 lane) + LVGL 9 bring-up
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>
#include "esp_cache.h"
#include "lcd_rm690b0.h"
#include "display_lab.h"
#include "touch_gtw623.h"
#include "demos/lv_demos.h"

void ui_touch_test_create(lv_display_t *disp);

static const char *TAG = "main";

#define LVGL_DRAW_BUF_LINES    (LCD_V_RES / 10)
#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_STACK_SIZE   (8 * 1024)
#define LVGL_TASK_PRIORITY     2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)

static lcd_rm690b0_t s_lcd;
static _lock_t s_lvgl_lock;

void lvgl_lock(void)   { _lock_acquire(&s_lvgl_lock); }
void lvgl_unlock(void) { _lock_release(&s_lvgl_lock); }

/*
 * Observed on hardware: sending R,G,B,W showed B,R,G,W, i.e. with LVGL's RGB888 byte order
 * (memory = B,G,R) the panel takes byte0 as GREEN, byte1 as RED, byte2 as BLUE.
 * Rotate each pixel to (G,R,B) so colors come out right.
 */
static inline void lcd_fix_color_order(uint8_t *buf, size_t pixels)
{
#if CONFIG_LCD_COLOR_ORDER_FIX
    for (size_t i = 0; i < pixels; i++, buf += 3) {
        uint8_t b = buf[0], g = buf[1], r = buf[2];
        buf[0] = g;
        buf[1] = r;
        buf[2] = b;
    }
#endif
}

#if CONFIG_LCD_FB_BYTE_LAG > 0
/*
 * The DPI bridge stream lags the frame buffer by CONFIG_LCD_FB_BYTE_LAG bytes (circularly), so pixel
 * data meant for frame offset j is written to fb[j - LAG]. Writes go straight into the frame buffer.
 */
#define FB_SIZE (LCD_H_RES * LCD_V_RES * 3)
static uint8_t *s_fb;

static void fb_write_wrapped(int off, const uint8_t *src, int len)
{
    off = ((off % FB_SIZE) + FB_SIZE) % FB_SIZE;
    int first = MIN(len, FB_SIZE - off);
    memcpy(s_fb + off, src, first);
    if (first < len) {
        memcpy(s_fb, src + first, len - first);
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int w = lv_area_get_width(area);
    const int row_bytes = w * 3;
    int start = (area->y1 * LCD_H_RES + area->x1) * 3 - CONFIG_LCD_FB_BYTE_LAG;
    for (int y = area->y1; y <= area->y2; y++) {
        fb_write_wrapped((y * LCD_H_RES + area->x1) * 3 - CONFIG_LCD_FB_BYTE_LAG, px_map, row_bytes);
        px_map += row_bytes;
    }
    int end = (area->y2 * LCD_H_RES + area->x2 + 1) * 3 - CONFIG_LCD_FB_BYTE_LAG;
    if (start >= 0) {
        esp_cache_msync(s_fb + start, end - start, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    } else {
        esp_cache_msync(s_fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    lv_display_flush_ready(disp);
}
#else
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    lcd_fix_color_order(px_map, lv_area_get_size(area));
    // copies the draw buffer into the DPI frame buffer; flush ready is signalled by on_color_trans_done
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}
#endif

static bool notify_flush_ready(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

#if CONFIG_CTP_ENABLE
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    touch_point_t pt;
    if (touch_gtw623_read(&pt) == ESP_OK && pt.pressed) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    while (1) {
        lvgl_lock();
        uint32_t next_ms = lv_timer_handler();
        lvgl_unlock();
        next_ms = MAX(next_ms, LVGL_TASK_MIN_DELAY_MS);
        next_ms = MIN(next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * next_ms);
    }
}

#if CONFIG_LCD_SHOW_COLOR_TEST
// Full-screen solid colors through the same path as LVGL (RGB888, bytes in memory = B,G,R)
static void color_test(void)
{
    const struct { const char *name; uint8_t r, g, b; } colors[] = {
        {"RED",   0xFF, 0x00, 0x00},
        {"GREEN", 0x00, 0xFF, 0x00},
        {"BLUE",  0x00, 0x00, 0xFF},
        {"WHITE", 0xFF, 0xFF, 0xFF},
        {"GRAY50",0x80, 0x80, 0x80},
        {"BLACK", 0x00, 0x00, 0x00},
    };
    const int lines = 60;
    uint8_t *buf = heap_caps_malloc(LCD_H_RES * lines * 3, MALLOC_CAP_DEFAULT);
    assert(buf);
    for (size_t c = 0; c < sizeof(colors) / sizeof(colors[0]); c++) {
        for (int i = 0; i < LCD_H_RES * lines; i++) {
            buf[i * 3 + 0] = colors[c].b;
            buf[i * 3 + 1] = colors[c].g;
            buf[i * 3 + 2] = colors[c].r;
        }
        lcd_fix_color_order(buf, LCD_H_RES * lines);
        for (int y = 0; y < LCD_V_RES; y += lines) {
            esp_lcd_panel_draw_bitmap(s_lcd.panel, 0, y, LCD_H_RES, y + lines, buf);
        }
        ESP_LOGI(TAG, "Color test: %s", colors[c].name);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    free(buf);
}
#endif

#if CONFIG_LCD_SHOW_GEOMETRY_TEST
static void put_px(uint8_t *line, int x, uint8_t r, uint8_t g, uint8_t b)
{
    line[x * 3 + 0] = b;
    line[x * 3 + 1] = g;
    line[x * 3 + 2] = r;
}

// Edges: left col RED, right col BLUE, top row GREEN, bottom row YELLOW.
// Grid: gray every 50px, white every 100px. 2px-wide edges so a 1px loss is still visible.
static void geometry_test(void)
{
    const int lines = 60;
    uint8_t *buf = heap_caps_malloc(LCD_H_RES * lines * 3, MALLOC_CAP_DEFAULT);
    assert(buf);
    for (int y0 = 0; y0 < LCD_V_RES; y0 += lines) {
        for (int l = 0; l < lines; l++) {
            int y = y0 + l;
            uint8_t *row = buf + l * LCD_H_RES * 3;
            for (int x = 0; x < LCD_H_RES; x++) {
                uint8_t v = 0;
                if (x % 100 == 0 || y % 100 == 0) {
                    v = 0xFF;
                } else if (x % 50 == 0 || y % 50 == 0) {
                    v = 0x60;
                }
                put_px(row, x, v, v, v);
            }
            if (y < 2) {
                for (int x = 0; x < LCD_H_RES; x++) put_px(row, x, 0, 0xFF, 0);
            }
            if (y >= LCD_V_RES - 2) {
                for (int x = 0; x < LCD_H_RES; x++) put_px(row, x, 0xFF, 0xFF, 0);
            }
            put_px(row, 0, 0xFF, 0, 0);
            put_px(row, 1, 0xFF, 0, 0);
            put_px(row, LCD_H_RES - 2, 0, 0, 0xFF);
            put_px(row, LCD_H_RES - 1, 0, 0, 0xFF);
        }
        lcd_fix_color_order(buf, LCD_H_RES * lines);
        esp_lcd_panel_draw_bitmap(s_lcd.panel, 0, y0, LCD_H_RES, y0 + lines, buf);
    }
    free(buf);
    ESP_LOGI(TAG, "Geometry test: left=RED right=BLUE top=GREEN bottom=YELLOW, grid 50/100px (%ds)",
             CONFIG_LCD_GEOMETRY_TEST_SECONDS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_LCD_GEOMETRY_TEST_SECONDS * 1000));
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(lcd_rm690b0_init(&s_lcd));

#if CONFIG_LCD_SHOW_COLOR_BAR
    // DSI host built-in pattern: verifies the link/timing without LVGL
    ESP_LOGI(TAG, "Showing color bar");
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(s_lcd.panel, MIPI_DSI_PATTERN_BAR_VERTICAL));
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(s_lcd.panel, MIPI_DSI_PATTERN_NONE));
#endif
#if CONFIG_LCD_SHOW_COLOR_TEST
    color_test();
#endif
#if CONFIG_LCD_SHOW_GEOMETRY_TEST
    geometry_test();
#endif
#if CONFIG_LCD_DISPLAY_LAB
    display_lab_run(&s_lcd);
#endif

    ESP_LOGI(TAG, "Initialize LVGL %d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(disp, s_lcd.panel);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB888);

    size_t buf_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * 3;
    // internal RAM draw buffers for faster rendering; fall back to PSRAM
    void *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        free(buf1);
        free(buf2);
        buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);
        buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);
    }
    assert(buf1 && buf2);
    lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
#if CONFIG_LCD_FB_BYTE_LAG > 0
    {
        void *fb = NULL;
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_lcd.panel, 1, &fb));
        s_fb = fb;
        memset(s_fb, 0, FB_SIZE);
        esp_cache_msync(s_fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        ESP_LOGW(TAG, "Frame buffer byte-lag compensation: %d bytes", CONFIG_LCD_FB_BYTE_LAG);
    }
#endif

    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = notify_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_lcd.panel, &cbs, disp));

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

#if CONFIG_CTP_ENABLE
    if (touch_gtw623_init(LCD_H_RES, LCD_V_RES) == ESP_OK) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(indev, disp);
        lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
    } else {
        ESP_LOGE(TAG, "Touch init failed, running without touch");
    }
#endif

    xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    lvgl_lock();
#if CONFIG_APP_DEMO_TOUCH_TEST
    ui_touch_test_create(disp);
#else
    lv_demo_widgets();
#endif
    lvgl_unlock();
}
