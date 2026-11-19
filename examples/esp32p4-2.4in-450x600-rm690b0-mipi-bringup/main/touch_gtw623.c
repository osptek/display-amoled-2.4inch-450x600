/*
 * GENIX GTW623 capacitive touch controller
 *
 * The GTW623 datasheet has no register map. Reverse engineered on this panel from probe logs:
 *  - I2C 7-bit address 0x2C (the device that disappears while RESET is held low)
 *  - FocalTech FT5x06-style point report, read from offset 0 (see read_report()):
 *      0x02  TD_STATUS  [3:0] number of touch points
 *      0x03  P1_XH      [7:6] event (0 = down, 1 = up, 2 = contact)  [3:0] X[11:8]
 *      0x04  P1_XL      X[7:0]
 *      0x05  P1_YH      [7:4] touch id  [3:0] Y[11:8]
 *      0x06  P1_YL      Y[7:0]
 *  - INT pulses low every ~10ms while a finger is down, so the report is simply polled
 *  - coordinates already match the 450x600 display (no swap/mirror needed)
 *
 * Only the [00 00] pointer write is sent; no configuration register is written.
 */
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "touch_gtw623.h"

static const char *TAG = "gtw623";


#define EVT_DOWN            0
#define EVT_UP              1

#define MAX_POINTS          5
#define I2C_TIMEOUT_MS      50

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint16_t s_x_max, s_y_max;
static touch_point_t s_last;

/*
 * The chip does not answer a repeated-start read (it echoes the register byte back), so:
 * write the register pointer as 2 bytes [0x00, reg] -> STOP -> short pause -> separate read.
 * This exact sequence ([00 00] then read) is what returned valid touch reports in the probe logs.
 */
static esp_err_t read_report(uint8_t *data, size_t len)
{
    uint8_t ptr[2] = {0x00, 0x00};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, ptr, sizeof(ptr), I2C_TIMEOUT_MS), TAG, "ptr");
    esp_rom_delay_us(50);
    return i2c_master_receive(s_dev, data, len, I2C_TIMEOUT_MS);
}

#if CONFIG_CTP_DEBUG_LOG
static void log_scan(const char *when)
{
    char line[128];
    int n = snprintf(line, sizeof(line), "I2C scan %-12s:", when);
    for (int a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_bus, a, 10) == ESP_OK && n < (int)sizeof(line) - 6) {
            n += snprintf(line + n, sizeof(line) - n, " 0x%02X", a);
        }
    }
    ESP_LOGI(TAG, "%s", line);
}
#endif

static void hw_reset(void)
{
#if CONFIG_CTP_PIN_RST >= 0
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_CTP_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    // datasheet 3.5: RESETn low >= 1ms
    gpio_set_level(CONFIG_CTP_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
#if CONFIG_CTP_DEBUG_LOG
    log_scan("RST low");
#endif
    gpio_set_level(CONFIG_CTP_PIN_RST, 1);
#endif
    vTaskDelay(pdMS_TO_TICKS(CONFIG_CTP_BOOT_DELAY_MS));
#if CONFIG_CTP_DEBUG_LOG
    log_scan("RST released");
#endif
}

esp_err_t touch_gtw623_init(uint16_t x_max, uint16_t y_max)
{
    s_x_max = x_max;
    s_y_max = y_max;
    memset(&s_last, 0, sizeof(s_last));

#if CONFIG_CTP_PIN_INT >= 0
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_CTP_PIN_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // INTn is open drain
    };
    ESP_ERROR_CHECK(gpio_config(&int_cfg));
#endif

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_CTP_PIN_SDA,
        .scl_io_num = CONFIG_CTP_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    hw_reset();

    ESP_RETURN_ON_ERROR(i2c_master_probe(s_bus, CONFIG_CTP_I2C_ADDR, I2C_TIMEOUT_MS), TAG,
                        "no ACK at 0x%02X", CONFIG_CTP_I2C_ADDR);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_CTP_I2C_ADDR,
        .scl_speed_hz = CONFIG_CTP_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "add dev");

    uint8_t r[7];
    esp_err_t e = read_report(r, sizeof(r));
    ESP_LOGI(TAG, "Touch at 0x%02X (INT GPIO%d, RST GPIO%d), idle report (%s): %02X %02X %02X %02X %02X %02X %02X",
             CONFIG_CTP_I2C_ADDR, CONFIG_CTP_PIN_INT, CONFIG_CTP_PIN_RST, esp_err_to_name(e),
             r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
    return ESP_OK;
}

esp_err_t touch_gtw623_read(touch_point_t *pt)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t r[7];   // [0..1] -, [2] TD_STATUS, [3] XH, [4] XL, [5] YH, [6] YL
    esp_err_t err = read_report(r, sizeof(r));
    if (err != ESP_OK) {
        *pt = s_last;
        return err;
    }
    const uint8_t *d = &r[2];

    uint8_t n = d[0] & 0x0F;
    uint8_t evt = d[1] >> 6;
    // n > MAX_POINTS filters the occasional garbage report (seen: TD_STATUS 0x10, coords 0xFFFF)
    if (n == 0 || n > MAX_POINTS || evt == EVT_UP) {
#if CONFIG_CTP_DEBUG_LOG
        if (s_last.pressed) {
            ESP_LOGI(TAG, "up   (%d, %d)", s_last.x, s_last.y);
        }
#endif
        s_last.pressed = false;
        *pt = s_last;
        return ESP_OK;
    }

    int32_t x = ((d[1] & 0x0F) << 8) | d[2];
    int32_t y = ((d[3] & 0x0F) << 8) | d[4];
#if CONFIG_CTP_SWAP_XY
    int32_t t = x; x = y; y = t;
#endif
#if CONFIG_CTP_MIRROR_X
    x = s_x_max - 1 - x;
#endif
#if CONFIG_CTP_MIRROR_Y
    y = s_y_max - 1 - y;
#endif
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= s_x_max) x = s_x_max - 1;
    if (y >= s_y_max) y = s_y_max - 1;

#if CONFIG_CTP_DEBUG_LOG
    if (!s_last.pressed || s_last.x != x || s_last.y != y) {
        ESP_LOGI(TAG, "%s (%ld, %ld) n=%d id=%d", evt == EVT_DOWN ? "down" : "move", x, y, n, d[3] >> 4);
    }
#endif
    s_last.x = x;
    s_last.y = y;
    s_last.pressed = true;
    *pt = s_last;
    return ESP_OK;
}
