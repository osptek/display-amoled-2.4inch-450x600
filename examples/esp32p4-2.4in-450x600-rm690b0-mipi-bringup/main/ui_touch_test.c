/*
 * Touch test screen: white background split into 50x50 cells.
 * The cell under the finger lights up and fades back to white after a short time.
 * A crosshair follows the finger and the top label shows the coordinates.
 */
#include <stdio.h>
#include "lvgl.h"
#include "lcd_rm690b0.h"

#define CELL        50
#define COLS        (LCD_H_RES / CELL)                  // 9
#define ROWS        (LCD_V_RES / CELL)                  // 12
#define HOLD_MS     800                                 // how long a cell stays colored after release

static lv_obj_t *s_cells[ROWS][COLS];
static uint32_t s_touched_at[ROWS][COLS];
static lv_obj_t *s_label;
static lv_obj_t *s_hline, *s_vline;
static uint32_t s_taps;

static const uint32_t s_palette[] = {
    0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759, 0x00C7BE, 0x007AFF, 0x5856D6, 0xAF52DE,
};

static void set_cell_white(lv_obj_t *c)
{
    lv_obj_set_style_bg_color(c, lv_color_white(), 0);
}

static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_taps++;
    }
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int col = p.x / CELL, row = p.y / CELL;
        if (col >= 0 && col < COLS && row >= 0 && row < ROWS) {
            lv_obj_set_style_bg_color(s_cells[row][col],
                                      lv_color_hex(s_palette[s_taps % (sizeof(s_palette) / sizeof(s_palette[0]))]), 0);
            s_touched_at[row][col] = lv_tick_get();
        }
        lv_obj_set_pos(s_hline, 0, p.y);
        lv_obj_set_pos(s_vline, p.x, 0);
        lv_obj_remove_flag(s_hline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_vline, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_label, "x=%3d  y=%3d   taps=%lu", (int)p.x, (int)p.y, (unsigned long)s_taps);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_add_flag(s_hline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_vline, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_label, "released at x=%3d  y=%3d   taps=%lu", (int)p.x, (int)p.y, (unsigned long)s_taps);
    }
}

static void fade_timer_cb(lv_timer_t *t)
{
    uint32_t now = lv_tick_get();
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (s_touched_at[r][c] && lv_tick_diff(now, s_touched_at[r][c]) > HOLD_MS) {
                s_touched_at[r][c] = 0;
                set_cell_white(s_cells[r][c]);
            }
        }
    }
}

void ui_touch_test_create(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, touch_event_cb, LV_EVENT_ALL, NULL);

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            lv_obj_t *cell = lv_obj_create(scr);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, CELL, CELL);
            lv_obj_set_pos(cell, c * CELL, r * CELL);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(0xE0E0E0), 0);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);
            s_cells[r][c] = cell;
        }
    }

    // crosshair
    s_hline = lv_obj_create(scr);
    lv_obj_remove_style_all(s_hline);
    lv_obj_set_size(s_hline, LCD_H_RES, 1);
    lv_obj_set_style_bg_opa(s_hline, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_hline, lv_color_black(), 0);
    lv_obj_remove_flag(s_hline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_hline, LV_OBJ_FLAG_HIDDEN);

    s_vline = lv_obj_create(scr);
    lv_obj_remove_style_all(s_vline);
    lv_obj_set_size(s_vline, 1, LCD_V_RES);
    lv_obj_set_style_bg_opa(s_vline, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_vline, lv_color_black(), 0);
    lv_obj_remove_flag(s_vline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_vline, LV_OBJ_FLAG_HIDDEN);

    s_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_label, lv_color_white(), 0);
    lv_obj_set_style_pad_all(s_label, 4, 0);
    lv_obj_remove_flag(s_label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_label, "Touch test - tap anywhere");
    lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, 8);

    lv_timer_create(fade_timer_cb, 100, NULL);
}
