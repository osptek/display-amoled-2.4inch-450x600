#pragma once

#include "lcd_rm690b0.h"

#if CONFIG_LCD_DPI_DISABLE_LP
#define LAB_LP_DISABLED 1
#else
#define LAB_LP_DISABLED 0
#endif

/** Cycle through video-mode / frame-buffer-offset experiments, logging each step. */
void display_lab_run(lcd_rm690b0_t *lcd);
