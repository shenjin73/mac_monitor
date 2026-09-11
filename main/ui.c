/*
 * 4 ring-gauge pages (CPU / GPU / MEM / FAN) on an lv_tileview, swiped
 * horizontally. Visual layout follows kimi_monitor's SystemView.swift:
 * bold grey title, single-colour ring with dim track, big centre value +
 * small sub line; the fan page adds 3 sensor rows at the bottom.
 * (The SwiftUI 3-D tube shading relies on blur and is approximated here by
 * a solid round-capped arc over a dim track.)
 *
 * Note: labels are ASCII-only — the stock Montserrat fonts have no CJK
 * glyphs, so the Chinese labels of the macOS app become English here.
 */

#include "ui.h"
#include "serial_input.h"
#include "lamp_imgs.h"

#include "lvgl.h"
#include <stdio.h>

#define PAGE_COUNT 7
#define STALE_MS 10000
#define FAN_MAX_RPM 5500

/* Same accent colours as the macOS app */
#define COL_CPU 0x0A84FF
#define COL_GPU 0xBF5AF2
#define COL_MEM 0x30D158
#define COL_FAN 0xFF9F0A
#define COL_KIMI 0x32ADE6
#define COL_OK 0x32ADE6
#define COL_WARN 0xFF9F0A
#define COL_CRIT 0xFF453A

/* Session light states (host sends sess=0..3) */
#define SESS_NONE 0
#define SESS_IDLE 1
#define SESS_WORKING 2
#define SESS_WAITING 3

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *center;
    lv_obj_t *sub;
} gauge_t;

/* KIMI page: outer big ring = weekly quota, inner small ring = 5-hour window */
typedef struct {
    lv_obj_t *arc_week;
    lv_obj_t *arc_5h;
    lv_obj_t *center;
    lv_obj_t *sub;
} kimi_gauge_t;

/* DeepSeek page: balance + today's tokens, no ring */
typedef struct {
    lv_obj_t *center;
    lv_obj_t *sub;
    lv_obj_t *row_val;
} ds_gauge_t;

static gauge_t g_cpu, g_gpu, g_mem, g_fan;
static kimi_gauge_t g_kimi;
static ds_gauge_t g_ds;
static lv_obj_t *g_sess_light, *g_sess_state, *g_sess_count;
static lv_obj_t *g_fan_row_val[3]; /* CPU temp / GPU temp / power values */
static lv_obj_t *g_tiles[PAGE_COUNT];
static lv_obj_t *g_dots[PAGE_COUNT];

static void gauge_create(lv_obj_t *tile, const char *title, lv_color_t color, gauge_t *g)
{
    lv_obj_t *title_lbl = lv_label_create(tile);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *arc = lv_arc_create(tile);
    lv_obj_set_size(arc, 164, 164);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *center = lv_label_create(arc);
    lv_obj_set_style_text_font(center, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(center, lv_color_white(), 0);
    lv_label_set_text(center, "--");
    lv_obj_align(center, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *sub = lv_label_create(arc);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8E8E93), 0);
    lv_label_set_text(sub, "");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 24);

    g->arc = arc;
    g->center = center;
    g->sub = sub;
}

static void fan_rows_create(lv_obj_t *tile)
{
    static const char *names[3] = {"CPU temp", "GPU temp", "Power"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *label = lv_label_create(tile);
        lv_label_set_text(label, names[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x8E8E93), 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, 214 + i * 18);

        lv_obj_t *value = lv_label_create(tile);
        lv_label_set_text(value, "-");
        lv_obj_set_style_text_font(value, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(value, lv_color_white(), 0);
        lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -24, 214 + i * 18);
        g_fan_row_val[i] = value;
    }
}

static lv_obj_t *ring_create(lv_obj_t *parent, int size, int width, lv_color_t color, lv_coord_t y)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, y);
    return arc;
}

static void title_create(lv_obj_t *tile, const char *title)
{
    lv_obj_t *title_lbl = lv_label_create(tile);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 12);
}

/* KIMI page: big outer ring = weekly quota, small inner ring = 5-hour window */
static void kimi_page_create(lv_obj_t *tile)
{
    title_create(tile, "KIMI");

    g_kimi.arc_week = ring_create(tile, 164, 14, lv_color_hex(COL_KIMI), 40);
    g_kimi.arc_5h = ring_create(tile, 116, 10, lv_color_hex(COL_KIMI), 64);

    g_kimi.center = lv_label_create(g_kimi.arc_5h);
    lv_obj_set_style_text_font(g_kimi.center, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(g_kimi.center, lv_color_white(), 0);
    lv_label_set_text(g_kimi.center, "-");
    lv_obj_align(g_kimi.center, LV_ALIGN_CENTER, 0, -8);

    g_kimi.sub = lv_label_create(g_kimi.arc_5h);
    lv_obj_set_style_text_font(g_kimi.sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_kimi.sub, lv_color_hex(0x8E8E93), 0);
    lv_label_set_text(g_kimi.sub, "");
    lv_obj_align(g_kimi.sub, LV_ALIGN_CENTER, 0, 18);
}

/* Sessions page: one big status light for all CLI sessions.
 * The lamp is a pre-rendered RGB565 bitmap (tools/gen_lamp.py): metallic
 * bezel, radial-gradient lens with an LED dot matrix; the waiting state
 * blinks by swapping between the lit and dim frames. */
static void sess_page_create(lv_obj_t *tile)
{
    title_create(tile, "SESS");

    g_sess_light = lv_img_create(tile);
    lv_img_set_src(g_sess_light, &lamp_gray);
    lv_obj_clear_flag(g_sess_light, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(g_sess_light, LV_ALIGN_TOP_MID, 0, 40); /* same top as the ring gauges */

    g_sess_state = lv_label_create(tile);
    lv_obj_set_style_text_font(g_sess_state, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_sess_state, lv_color_white(), 0);
    lv_label_set_text(g_sess_state, "--");
    lv_obj_align(g_sess_state, LV_ALIGN_TOP_MID, 0, 222);

    g_sess_count = lv_label_create(tile);
    lv_obj_set_style_text_font(g_sess_count, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_sess_count, lv_color_hex(0x8E8E93), 0);
    lv_label_set_text(g_sess_count, "");
    lv_obj_align(g_sess_count, LV_ALIGN_TOP_MID, 0, 244);
}

/* DeepSeek page: big balance + today's token count, no ring */
static void ds_page_create(lv_obj_t *tile)
{
    title_create(tile, "DEEPSEEK");

    g_ds.center = lv_label_create(tile);
    lv_obj_set_style_text_font(g_ds.center, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(g_ds.center, lv_color_white(), 0);
    lv_label_set_text(g_ds.center, "-");
    lv_obj_align(g_ds.center, LV_ALIGN_CENTER, 0, -28);

    g_ds.sub = lv_label_create(tile);
    lv_obj_set_style_text_font(g_ds.sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_ds.sub, lv_color_hex(0x8E8E93), 0);
    lv_label_set_text(g_ds.sub, "BALANCE (CNY)");
    lv_obj_align(g_ds.sub, LV_ALIGN_CENTER, 0, 16);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, "Today tokens");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, 214);

    g_ds.row_val = lv_label_create(tile);
    lv_label_set_text(g_ds.row_val, "-");
    lv_obj_set_style_text_font(g_ds.row_val, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_ds.row_val, lv_color_white(), 0);
    lv_obj_align(g_ds.row_val, LV_ALIGN_TOP_RIGHT, -24, 214);
}

static void dots_create(lv_obj_t *parent)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, i == 0 ? lv_color_white() : lv_color_hex(0x444444), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, (i - (PAGE_COUNT - 1) / 2.0f) * 16, -6);
        g_dots[i] = dot;
    }
}

static void tileview_event_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t *act = lv_tileview_get_tile_act(tv);
    int idx = 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (g_tiles[i] == act) idx = i;
    }
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_set_style_bg_color(g_dots[i],
                                  i == idx ? lv_color_white() : lv_color_hex(0x444444), 0);
    }
}

/* Wrap around: track how far past either edge the user dragged, then jump
 * to the other end when the gesture is released (jumping mid-drag fights
 * the ongoing indev scrolling and snaps back). */
static lv_coord_t s_drag_min = 0;
static lv_coord_t s_drag_max = 0;

static void tileview_scroll_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_coord_t x = lv_obj_get_scroll_x(tv);
    if (x < s_drag_min) s_drag_min = x;
    if (x > s_drag_max) s_drag_max = x;
}

static void tileview_scroll_end_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    lv_coord_t page_w = lv_obj_get_width(tv);
    if (s_drag_min < -50) {
        lv_obj_set_tile_id(tv, PAGE_COUNT - 1, 0, LV_ANIM_ON);
    } else if (s_drag_max > (PAGE_COUNT - 1) * page_w + 50) {
        lv_obj_set_tile_id(tv, 0, 0, LV_ANIM_ON);
    }
    s_drag_min = 0;
    s_drag_max = (PAGE_COUNT - 1) * page_w;
}

static void set_gauge(gauge_t *g, int pct, const char *center, const char *sub)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_arc_set_value(g->arc, pct);
    lv_label_set_text(g->center, center);
    lv_label_set_text(g->sub, sub);
}

static void fmt_freq(char *buf, size_t len, int mhz)
{
    if (mhz <= 0) buf[0] = '\0'; /* unknown: leave blank rather than a stray '-' */
    else if (mhz >= 1000) snprintf(buf, len, "%.1f GHz", mhz / 1000.0);
    else snprintf(buf, len, "%d MHz", mhz);
}

static void fmt_tokens(char *buf, size_t len, int32_t n)
{
    if (n < 0) snprintf(buf, len, "-");
    else if (n >= 1000000) snprintf(buf, len, "%.1fM", n / 1000000.0);
    else if (n >= 1000) snprintf(buf, len, "%.1fK", n / 1000.0);
    else snprintf(buf, len, "%d", (int)n);
}

/* macOS app quota colouring: >70% orange, >90% red */
static lv_color_t quota_color(int pct)
{
    if (pct > 90) return lv_color_hex(COL_CRIT);
    if (pct > 70) return lv_color_hex(COL_WARN);
    return lv_color_hex(COL_OK);
}

static void ring_update(lv_obj_t *arc, int pct)
{
    lv_color_t color = quota_color(pct);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_arc_set_value(arc, pct);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    mac_stats_t s;
    bool have = stats_get_snapshot(&s);
    char center[24], sub[32];

    if (!have || stats_is_stale(STALE_MS)) {
        set_gauge(&g_cpu, 0, "--", "");
        set_gauge(&g_gpu, 0, "--", "");
        set_gauge(&g_mem, 0, "--", "");
        set_gauge(&g_fan, 0, "--", "");
        for (int i = 0; i < 3; i++) {
            lv_label_set_text(g_fan_row_val[i], "-");
            lv_obj_set_style_text_color(g_fan_row_val[i], lv_color_white(), 0);
        }
        lv_arc_set_value(g_kimi.arc_week, 0);
        lv_arc_set_value(g_kimi.arc_5h, 0);
        lv_label_set_text(g_kimi.center, "--");
        lv_label_set_text(g_kimi.sub, "");
        lv_label_set_text(g_ds.center, "--");
        lv_obj_set_style_text_color(g_ds.center, lv_color_white(), 0);
        lv_label_set_text(g_ds.row_val, "-");
        lv_img_set_src(g_sess_light, &lamp_gray);
        lv_label_set_text(g_sess_state, "--");
        lv_label_set_text(g_sess_count, "");
        return;
    }

    /* CPU */
    if (s.cpu >= 0) {
        snprintf(center, sizeof(center), "%d%%", s.cpu);
        fmt_freq(sub, sizeof(sub), s.cpufreq_mhz);
        set_gauge(&g_cpu, s.cpu, center, sub);
    } else {
        set_gauge(&g_cpu, 0, "-", "");
    }

    /* GPU */
    if (s.gpu >= 0) {
        snprintf(center, sizeof(center), "%d%%", s.gpu);
        fmt_freq(sub, sizeof(sub), s.gpufreq_mhz);
        set_gauge(&g_gpu, s.gpu, center, sub);
    } else {
        set_gauge(&g_gpu, 0, "-", "");
    }

    /* MEM */
    if (s.mem >= 0) {
        snprintf(center, sizeof(center), "%d%%", s.mem);
        if (s.mem_used_gb >= 0 && s.mem_total_gb > 0) {
            snprintf(sub, sizeof(sub), "%.1f/%.0f GB", s.mem_used_gb, s.mem_total_gb);
        } else {
            sub[0] = '\0';
        }
        set_gauge(&g_mem, s.mem, center, sub);
    } else {
        set_gauge(&g_mem, 0, "-", "");
    }

    /* FAN */
    if (s.fan_rpm >= 0) {
        snprintf(center, sizeof(center), "%d", s.fan_rpm);
        lv_obj_set_style_text_font(g_fan.center, &lv_font_montserrat_40, 0);
        int pct = s.fan_rpm * 100 / FAN_MAX_RPM;
        set_gauge(&g_fan, pct, center, "RPM");
    } else {
        lv_obj_set_style_text_font(g_fan.center, &lv_font_montserrat_16, 0);
        set_gauge(&g_fan, 0, "NO FAN", "");
    }

    /* Fan page sensor rows */
    if (s.cpu_temp_c >= 0) snprintf(center, sizeof(center), "%.1f C", s.cpu_temp_c);
    else snprintf(center, sizeof(center), "-");
    lv_label_set_text(g_fan_row_val[0], center);

    if (s.gpu_temp_c >= 0) snprintf(center, sizeof(center), "%.1f C", s.gpu_temp_c);
    else snprintf(center, sizeof(center), "-");
    lv_label_set_text(g_fan_row_val[1], center);

    if (s.power_w >= 0) snprintf(center, sizeof(center), "%.1f W", s.power_w);
    else snprintf(center, sizeof(center), "-");
    lv_label_set_text(g_fan_row_val[2], center);
    lv_obj_set_style_text_color(g_fan_row_val[2],
                                s.power_w > 50 ? lv_color_hex(0xFF453A) : lv_color_white(), 0);

    /* KIMI: big ring = weekly, small ring = 5-hour window */
    if (s.kimiweek >= 0) {
        ring_update(g_kimi.arc_week, s.kimiweek);
        snprintf(center, sizeof(center), "%d%%", s.kimiweek);
        lv_label_set_text(g_kimi.center, center);
    } else {
        ring_update(g_kimi.arc_week, 0);
        lv_label_set_text(g_kimi.center, "-");
    }
    if (s.kimi5h >= 0) {
        ring_update(g_kimi.arc_5h, s.kimi5h);
        snprintf(sub, sizeof(sub), "5h %d%%", s.kimi5h);
        lv_label_set_text(g_kimi.sub, sub);
    } else {
        ring_update(g_kimi.arc_5h, 0);
        lv_label_set_text(g_kimi.sub, "");
    }

    /* DeepSeek: balance + today's tokens */
    if (s.dsbal >= 0) {
        snprintf(center, sizeof(center), "%.2f", s.dsbal);
        lv_label_set_text(g_ds.center, center);
        /* macOS app thresholds: <=5 orange, <=1 red */
        lv_color_t c = s.dsbal <= 1 ? lv_color_hex(COL_CRIT)
                     : s.dsbal <= 5 ? lv_color_hex(COL_WARN)
                                    : lv_color_white();
        lv_obj_set_style_text_color(g_ds.center, c, 0);
    } else {
        lv_label_set_text(g_ds.center, "-");
        lv_obj_set_style_text_color(g_ds.center, lv_color_white(), 0);
    }
    fmt_tokens(sub, sizeof(sub), s.dstok);
    lv_label_set_text(g_ds.row_val, sub);

    /* Sessions light: red blink = waiting, yellow = working,
     * green = idle, grey = no live session. */
    static const struct {
        const lv_img_dsc_t *img;
        const char *word;
    } sess_map[] = {
        [SESS_NONE] = {&lamp_gray, "NO SESSION"},
        [SESS_IDLE] = {&lamp_green, "IDLE"},
        [SESS_WORKING] = {&lamp_yellow, "WORKING"},
        [SESS_WAITING] = {&lamp_red, "WAITING"},
    };
    int sess = (s.sess >= SESS_NONE && s.sess <= SESS_WAITING) ? s.sess : SESS_NONE;
    static bool blink_on;
    blink_on = !blink_on; /* 500 ms toggle → 1 s blink */
    const lv_img_dsc_t *img = (sess == SESS_WAITING && !blink_on)
                                  ? &lamp_red_dim : sess_map[sess].img;
    if (lv_img_get_src(g_sess_light) != img) {
        lv_img_set_src(g_sess_light, img);
    }
    lv_label_set_text(g_sess_state, sess_map[sess].word);
    if (sess == SESS_NONE) {
        lv_label_set_text(g_sess_count, "");
    } else {
        snprintf(sub, sizeof(sub), "%d sess  %d work  %d wait", s.sessn, s.sessw, s.sessp);
        lv_label_set_text(g_sess_count, sub);
    }
}

void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tv, tileview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(tv, tileview_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(tv, tileview_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    static const struct {
        const char *title;
        uint32_t color;
        gauge_t *gauge;
    } pages[] = {
        {"CPU", COL_CPU, &g_cpu},
        {"GPU", COL_GPU, &g_gpu},
        {"MEM", COL_MEM, &g_mem},
        {"FAN", COL_FAN, &g_fan},
    };

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *tile = lv_tileview_add_tile(tv, i, 0, LV_DIR_HOR);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
        g_tiles[i] = tile;
    }
    sess_page_create(g_tiles[0]);
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        gauge_create(g_tiles[i + 1], pages[i].title, lv_color_hex(pages[i].color), pages[i].gauge);
    }
    fan_rows_create(g_tiles[4]);
    kimi_page_create(g_tiles[5]);
    ds_page_create(g_tiles[6]);

    dots_create(scr);

    lv_timer_create(refresh_cb, 500, NULL);
}
