#include "display_ui_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#if CONFIG_PLAYGATE_ENABLE_DISPLAY

#define UI_H_RES 320

#define C_BG     lv_color_hex(0x14171c)
#define C_PANEL  lv_color_hex(0x1d222a)
#define C_LINE   lv_color_hex(0x2c333e)
#define C_TEXT   lv_color_hex(0xe6eaf0)
#define C_DIM    lv_color_hex(0x8d98a8)
#define C_ACCENT lv_color_hex(0x4ea1ff)
#define C_ON     lv_color_hex(0x2ea86a)
#define C_WARN   lv_color_hex(0xd9a13b)

static const char *const HID_STATE_NAMES[] = {"idle", "discoverable", "connecting", "connected"};
static const char *const SOURCE_NAMES[] = {"none", "serial", "udp", "ble"};
#define HID_STATE_COUNT (sizeof(HID_STATE_NAMES) / sizeof(HID_STATE_NAMES[0]))
#define SOURCE_COUNT (sizeof(SOURCE_NAMES) / sizeof(SOURCE_NAMES[0]))

/* The panel's 12 px rounded corners require content near the top and bottom edges
 * to stay within x 14..306. */
#define UI_EDGE_INSET 14
#define UI_BAR_LEFT UI_EDGE_INSET
#define UI_BAR_RIGHT (UI_H_RES - UI_EDGE_INSET)

#define UI_HID_W 124
#define UI_SRC_W 66
#define UI_SRC_X (UI_BAR_LEFT + UI_HID_W + 1)

#define UI_CHIP_GAP 2
#define UI_CHIP_W_NEUT 32
#define UI_CHIP_W_WIFI 31
#define UI_CHIP_W_BLE 24
#define UI_BLE_X (UI_BAR_RIGHT - UI_CHIP_W_BLE)
#define UI_WIFI_X (UI_BLE_X - UI_CHIP_GAP - UI_CHIP_W_WIFI)
#define UI_NEUT_X (UI_WIFI_X - UI_CHIP_GAP - UI_CHIP_W_NEUT)

/* Five top-bar fields, four network fields and ten metric values. */
#define TEXT_SLOT_COUNT 19
static lv_obj_t *s_text_slots[TEXT_SLOT_COUNT];
static uint8_t s_text_slot_count;

static uint8_t s_bar_hid;
static uint8_t s_bar_src;
static uint8_t s_bar_neutral;
static uint8_t s_bar_wifi;
static uint8_t s_bar_ble;

static uint8_t s_net_wifi;
static uint8_t s_net_ssid;
static uint8_t s_net_ip;
static uint8_t s_net_uptime;
static uint8_t s_metric_left[5];
static uint8_t s_metric_right[5];

static lv_obj_t *label_make(lv_obj_t *parent,
                            lv_coord_t x,
                            lv_coord_t y,
                            lv_coord_t width,
                            lv_coord_t height,
                            const lv_font_t *font,
                            lv_color_t color,
                            lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}

static uint8_t slot_register(lv_obj_t *label)
{
    if (s_text_slot_count >= TEXT_SLOT_COUNT) {
        return 0;
    }
    const uint8_t slot = s_text_slot_count++;
    s_text_slots[slot] = label;
    return slot;
}

static void ui_set_text(uint8_t slot, const char *fmt, ...)
{
    char text[32];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    lv_obj_t *label = s_text_slots[slot];
    if (strcmp(lv_label_get_text(label), text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

static void ui_set_color(uint8_t slot, lv_color_t color)
{
    lv_obj_t *label = s_text_slots[slot];
    if (lv_obj_get_style_text_color(label, 0).full == color.full) {
        return;
    }
    lv_obj_set_style_text_color(label, color, 0);
}

static void ui_set_hidden(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *hid_state_name(gateway_hid_state_t state)
{
    const size_t index = (size_t)state;
    return index < HID_STATE_COUNT ? HID_STATE_NAMES[index] : HID_STATE_NAMES[0];
}

static const char *source_name(input_source_t source)
{
    const size_t index = (size_t)source;
    return index < SOURCE_COUNT ? SOURCE_NAMES[index] : SOURCE_NAMES[0];
}

static lv_color_t hid_state_color(gateway_hid_state_t state)
{
    switch (state) {
    case GATEWAY_HID_CONNECTED:
        return C_ON;
    case GATEWAY_HID_CONNECTING:
        return C_ACCENT;
    case GATEWAY_HID_DISCOVERABLE:
        return C_WARN;
    case GATEWAY_HID_IDLE:
    default:
        return C_DIM;
    }
}

static void set_latency(uint8_t slot, uint32_t micros)
{
    if (micros == 0 || micros >= 60000u) {
        ui_set_text(slot, "--");
    } else {
        ui_set_text(slot, "%u us", (unsigned)micros);
    }
}

static uint8_t row_make(lv_obj_t *parent,
                        lv_coord_t y,
                        lv_coord_t name_x,
                        lv_coord_t name_w,
                        lv_coord_t value_x,
                        lv_coord_t value_w,
                        const char *name)
{
    lv_label_set_text(label_make(parent, name_x, y + 4, name_w, 14, &lv_font_montserrat_12,
                                 C_DIM, LV_TEXT_ALIGN_LEFT),
                      name);
    return slot_register(label_make(parent, value_x, y, value_w, 18, &lv_font_montserrat_16,
                                    C_TEXT, LV_TEXT_ALIGN_RIGHT));
}

void display_ui_view_update(const display_ui_snapshot_t *snapshot)
{
    ui_set_text(s_bar_hid, "%s", hid_state_name(snapshot->link.hid_state));
    ui_set_color(s_bar_hid, hid_state_color(snapshot->link.hid_state));
    ui_set_text(s_bar_src, "src %s", source_name(snapshot->metrics.active_source));
    ui_set_hidden(s_text_slots[s_bar_neutral], !snapshot->metrics.timeout_neutral);
    ui_set_color(s_bar_wifi, snapshot->link.wifi_connected ? C_ON : C_DIM);
    ui_set_color(s_bar_ble, snapshot->link.ble_connected ? C_ON : C_DIM);

    if (!snapshot->net.started) {
        ui_set_text(s_net_wifi, "wifi: off");
        ui_set_color(s_net_wifi, C_DIM);
    } else {
        ui_set_text(s_net_wifi, "wifi: %s", snapshot->net.ap_mode ? "ap" : "sta");
        ui_set_color(s_net_wifi, snapshot->net.connected ? C_ON : C_DIM);
    }
    ui_set_text(s_net_ssid, "%s", snapshot->net.ssid[0] == '\0' ? "--" : snapshot->net.ssid);
    ui_set_text(s_net_ip, "ip %s", snapshot->net.ip[0] == '\0' ? "--" : snapshot->net.ip);

    const int64_t seconds = snapshot->uptime_us / 1000000;
    ui_set_text(s_net_uptime, "up %uh%02um", (unsigned)(seconds / 3600),
                (unsigned)((seconds % 3600) / 60));

    ui_set_text(s_metric_left[0], "%u", (unsigned)snapshot->metrics.accepted_total);
    ui_set_text(s_metric_left[1], "%u", (unsigned)snapshot->metrics.invalid);
    ui_set_text(s_metric_left[2], "%u", (unsigned)snapshot->metrics.stale);
    ui_set_text(s_metric_left[3], "%u", (unsigned)snapshot->metrics.conflicts);
    set_latency(s_metric_left[4], snapshot->metrics.last_receive_to_submit_us);

    ui_set_text(s_metric_right[0], "%u", (unsigned)snapshot->console.report_submits);
    ui_set_text(s_metric_right[1], "%u", (unsigned)snapshot->console.missed_deadlines);
    ui_set_text(s_metric_right[2], "%u", (unsigned)snapshot->console.notify_errors);
    ui_set_text(s_metric_right[3], "%u", (unsigned)snapshot->console.mbuf_unavailable);
    ui_set_text(s_metric_right[4], "%uK", (unsigned)(snapshot->free_heap / 1024u));
}

void display_ui_view_create(void)
{
    static const char *const left_names[5] = {
        "accepted", "invalid", "stale", "conflicts", "latency",
    };
    static const char *const right_names[5] = {
        "reports", "missed", "notify", "mbuf", "heap",
    };

    s_text_slot_count = 0;

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, UI_H_RES, 22);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, C_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);

    s_bar_hid = slot_register(label_make(bar, UI_BAR_LEFT, 3, UI_HID_W, 16,
                                         &lv_font_montserrat_16, C_DIM, LV_TEXT_ALIGN_LEFT));
    s_bar_src = slot_register(label_make(bar, UI_SRC_X, 4, UI_SRC_W, 14,
                                         &lv_font_montserrat_12, C_TEXT, LV_TEXT_ALIGN_LEFT));
    s_bar_neutral = slot_register(label_make(bar, UI_NEUT_X, 5, UI_CHIP_W_NEUT, 12,
                                             &lv_font_montserrat_12, C_WARN, LV_TEXT_ALIGN_RIGHT));
    lv_label_set_text(s_text_slots[s_bar_neutral], "neut");
    lv_obj_add_flag(s_text_slots[s_bar_neutral], LV_OBJ_FLAG_HIDDEN);
    s_bar_wifi = slot_register(label_make(bar, UI_WIFI_X, 5, UI_CHIP_W_WIFI, 12,
                                          &lv_font_montserrat_12, C_DIM, LV_TEXT_ALIGN_RIGHT));
    lv_label_set_text(s_text_slots[s_bar_wifi], "wifi");
    s_bar_ble = slot_register(label_make(bar, UI_BLE_X, 5, UI_CHIP_W_BLE, 12,
                                         &lv_font_montserrat_12, C_DIM, LV_TEXT_ALIGN_RIGHT));
    lv_label_set_text(s_text_slots[s_bar_ble], "ble");

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_set_pos(content, 0, 22);
    lv_obj_set_size(content, UI_H_RES, 150);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    s_net_wifi = slot_register(label_make(content, 14, 4, 74, 20, &lv_font_montserrat_16,
                                          C_DIM, LV_TEXT_ALIGN_LEFT));
    s_net_ssid = slot_register(label_make(content, 94, 6, 212, 16, &lv_font_montserrat_12,
                                          C_TEXT, LV_TEXT_ALIGN_LEFT));
    s_net_ip = slot_register(label_make(content, 14, 26, 176, 20, &lv_font_montserrat_16,
                                        C_TEXT, LV_TEXT_ALIGN_LEFT));
    s_net_uptime = slot_register(label_make(content, 190, 26, 116, 20,
                                            &lv_font_montserrat_16, C_TEXT, LV_TEXT_ALIGN_RIGHT));

    lv_obj_t *separator = lv_obj_create(content);
    lv_obj_set_pos(separator, 14, 48);
    lv_obj_set_size(separator, 292, 1);
    lv_obj_set_style_radius(separator, 0, 0);
    lv_obj_set_style_bg_color(separator, C_LINE, 0);
    lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(separator, 0, 0);
    lv_obj_set_style_pad_all(separator, 0, 0);
    lv_obj_clear_flag(separator, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < 5; ++i) {
        const lv_coord_t y = 52 + (lv_coord_t)i * 18;
        s_metric_left[i] = row_make(content, y, 14, 54, 68, 88, left_names[i]);
        s_metric_right[i] = row_make(content, y, 164, 50, 214, 92, right_names[i]);
    }

    display_ui_snapshot_t snapshot;
    display_ui_snapshot_take(&snapshot);
    display_ui_view_update(&snapshot);
}

#endif /* CONFIG_PLAYGATE_ENABLE_DISPLAY */
