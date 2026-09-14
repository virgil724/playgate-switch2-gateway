#include "display_ui_internal.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if CONFIG_PLAYGATE_ENABLE_DISPLAY

#define UI_H_RES 320
#define UI_V_RES 172

/* 320x40 RGB565 = 25,600 B. Static DMA-capable DRAM; also the LVGL draw buffer. */
#define UI_BUF_LINES 40

#define LCD_PIN_MOSI 6
#define LCD_PIN_CLK 7
#define LCD_PIN_CS 14
#define LCD_PIN_DC 15
#define LCD_PIN_RST 21
#define LCD_PIN_BL 22

/* The 172 px wide panel sits inside a 240 px wide controller; portrait offset is 34. */
#define LCD_GAP_PORTRAIT 34

#define LCD_BACKLIGHT_DUTY_MAX 255u

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_disp_drv_t s_disp_drv;
static uint16_t s_buf[UI_H_RES * UI_BUF_LINES];

/* panel_clear() in display_ui_port_init() queues real DMA transfers before the LVGL
 * driver exists. lv_disp_flush_ready() dereferences drv->draw_buf, so the ISR must not
 * run before lv_disp_drv_register() has populated it. */
static volatile bool s_flush_ready_armed;

static bool on_flush_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata,
                          void *user_ctx)
{
    (void)io;
    (void)edata;
    if (s_flush_ready_armed) {
        /* Runs in ISR context. trans_queue_depth is 1 with a single draw buffer, so the
         * transfer that just completed is the one this flush waits for. */
        lv_disp_flush_ready((lv_disp_drv_t *)user_ctx);
    }
    return false;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void)drv;
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1,
                              area->y1,
                              area->x2 + 1,
                              area->y2 + 1,
                              color_p);
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t error = ledc_timer_config(&timer);
    if (error != ESP_OK) {
        return error;
    }

    const ledc_channel_config_t channel = {
        .gpio_num = LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel);
}

static esp_err_t backlight_set(uint32_t percent)
{
    const uint32_t duty = percent * LCD_BACKLIGHT_DUTY_MAX / 100u;
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (error != ESP_OK) {
        return error;
    }
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* Paint the whole panel with the UI background so power-on RAM noise never shows. */
static esp_err_t panel_clear(void)
{
    const uint16_t background = 0x1c17u; /* little-endian RGB565 for #14171c */
    for (size_t i = 0; i < UI_H_RES * UI_BUF_LINES; ++i) {
        s_buf[i] = background;
    }

    for (int y = 0; y < UI_V_RES; y += UI_BUF_LINES) {
        int lines = UI_V_RES - y;
        if (lines > UI_BUF_LINES) {
            lines = UI_BUF_LINES;
        }
        esp_err_t error = esp_lcd_panel_draw_bitmap(s_panel, 0, y, UI_H_RES, y + lines, s_buf);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

esp_err_t display_ui_port_init(void)
{
    esp_err_t error = backlight_init();
    if (error != ESP_OK) {
        return error;
    }

    const spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = UI_H_RES * UI_BUF_LINES * 2,
    };
    error = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        return error;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_PLAYGATE_DISPLAY_SPI_MHZ * 1000000,
        .trans_queue_depth = 1,
        .on_color_trans_done = on_flush_done,
        .user_ctx = &s_disp_drv,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    error = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &s_io);
    if (error != ESP_OK) {
        return error;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    error = esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel);
    if (error != ESP_OK) {
        return error;
    }

    error = esp_lcd_panel_reset(s_panel);
    if (error == ESP_OK) {
        error = esp_lcd_panel_init(s_panel);
    }
    /* IPS panel: the same board is driven with INVON by vendor examples. */
    if (error == ESP_OK) {
        error = esp_lcd_panel_invert_color(s_panel, true);
    }
    if (error == ESP_OK) {
        error = esp_lcd_panel_swap_xy(s_panel, true);
    }
    if (error == ESP_OK) {
        error = esp_lcd_panel_mirror(s_panel, true, false);
    }
    /* Landscape: the 34 px of controller RAM the 172 px panel does not cover lands on y. */
    if (error == ESP_OK) {
        error = esp_lcd_panel_set_gap(s_panel, 0, LCD_GAP_PORTRAIT);
    }
    if (error == ESP_OK) {
        error = esp_lcd_panel_disp_on_off(s_panel, true);
    }
    if (error == ESP_OK) {
        error = panel_clear();
    }
    if (error != ESP_OK) {
        return error;
    }

    return backlight_set(CONFIG_PLAYGATE_DISPLAY_BACKLIGHT_PERCENT);
}

esp_err_t display_ui_port_lvgl_init(void)
{
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_buf, NULL, UI_H_RES * UI_BUF_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = UI_H_RES;
    s_disp_drv.ver_res = UI_V_RES;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&s_disp_drv);
    s_flush_ready_armed = true;

    return ESP_OK;
}

#endif /* CONFIG_PLAYGATE_ENABLE_DISPLAY */
