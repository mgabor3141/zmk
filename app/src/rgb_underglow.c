/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/behavior.h>

#include <zmk/hid_indicators.h>
#include <zmk/usb.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>
#include <drivers/behavior.h>

#include <zmk/rgb_underglow.h>
#include <zmk/rgb_underglow_layer.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/underglow_color_changed.h>

#include <zmk/workqueue.h>
#include <zmk/events/split_peripheral_layer_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/peripheral_layers.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)

#error "A zmk,underglow chosen node must be declared"

#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_underglow_layer) && IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER)
#define UNDERGLOW_LAYER_ENABLED 1
static void zmk_rgb_underglow_set_layer(uint8_t layer, bool wakeup);
#endif

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

BUILD_ASSERT(CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN <= CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX,
             "ERROR: RGB underglow maximum brightness is less than minimum brightness");

enum rgb_underglow_effect {
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    UNDERGLOW_EFFECT_LAYER_INDICATORS,
#endif
    UNDERGLOW_EFFECT_NUMBER // Used to track number of underglow effects
};

struct rgb_underglow_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
    bool status_active;
    bool layer_enabled;
    uint16_t status_animation_step;
};

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];
static struct led_rgb status_pixels[STRIP_NUM_PIXELS];

static struct rgb_underglow_state state;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

void zmk_rgb_set_ext_power(void);

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX / BRT_MAX;
    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;

    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

    return rgb;
}

static void zmk_rgb_underglow_effect_solid(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(state.color));
    }
}

static void zmk_rgb_underglow_effect_breathe(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.b = abs(state.animation_step - 1200) / 12;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step += state.animation_speed * 10;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_rgb_underglow_effect_spectrum(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_rgb_underglow_effect_swirl(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (HUE_MAX / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed * 2;
    state.animation_step = state.animation_step % HUE_MAX;
}

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
static void zmk_rgb_underglow_effect_layer(void) {
    bool active = false;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i].r -= state.animation_speed < pixels[i].r ? state.animation_speed : pixels[i].r;
        pixels[i].g -= state.animation_speed < pixels[i].g ? state.animation_speed : pixels[i].g;
        pixels[i].b -= state.animation_speed < pixels[i].b ? state.animation_speed : pixels[i].b;
        if (pixels[i].r || pixels[i].g || pixels[i].b) {
            active = true;
        }
    }
    state.animation_step += state.animation_speed;

    if (state.animation_step > 255 || !active) {
        zmk_rgb_underglow_transient_off();
    }
}
#endif // IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

static int zmk_led_generate_status(void);

static void zmk_led_write_pixels(void) {
    static struct led_rgb led_buffer[STRIP_NUM_PIXELS];
    int bat0;
    int blend = 0;
    int reset_ext_power = 0;

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    bat0 = zmk_battery_state_of_charge();
#else
    bat0 = 100;
#endif

    if (state.status_active) {
        blend = zmk_led_generate_status();
    }

    // fast path: no status indicators, battery level OK
    if (blend == 0 && bat0 >= 20) {
        led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
        return;
    }
    // battery below minimum charge
    if (bat0 < 10) {
        memset(pixels, 0, sizeof(struct led_rgb) * STRIP_NUM_PIXELS);
        if (state.on) {
            int c_power = ext_power_get(ext_power);
            if (c_power && !state.status_active) {
                // power is on, RGB underglow is on, but battery is too low
                state.on = false;
                reset_ext_power = true;
            }
        }
    }

    if (blend == 0) {
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            led_buffer[i] = pixels[i];
        }
    } else if (blend >= 256) {
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            led_buffer[i] = status_pixels[i];
        }
    } else if (blend < 256) {
        uint16_t blend_l = blend;
        uint16_t blend_r = 256 - blend;
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            led_buffer[i].r =
                ((status_pixels[i].r * blend_l) >> 8) + ((pixels[i].r * blend_r) >> 8);
            led_buffer[i].g =
                ((status_pixels[i].g * blend_l) >> 8) + ((pixels[i].g * blend_r) >> 8);
            led_buffer[i].b =
                ((status_pixels[i].b * blend_l) >> 8) + ((pixels[i].b * blend_r) >> 8);
        }
    }

    // battery below 20%, reduce LED brightness
    if (bat0 < 20) {
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            led_buffer[i].r = led_buffer[i].r >> 1;
            led_buffer[i].g = led_buffer[i].g >> 1;
            led_buffer[i].b = led_buffer[i].b >> 1;
        }
    }

    int err = led_strip_update_rgb(led_strip, led_buffer, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }

    if (reset_ext_power) {
        zmk_rgb_set_ext_power();
    }
}

#define UNDERGLOW_INDICATORS DT_PATH(underglow_indicators)

#if defined(DT_N_S_underglow_indicators_EXISTS)
#define UNDERGLOW_INDICATORS_ENABLED 1
#else
#define UNDERGLOW_INDICATORS_ENABLED 0
#endif

#if !UNDERGLOW_INDICATORS_ENABLED
static int zmk_led_generate_status(void) { return 0; }
#else

const uint8_t underglow_layer_state[] = DT_PROP(UNDERGLOW_INDICATORS, layer_state);
const uint8_t underglow_ble_state[] = DT_PROP(UNDERGLOW_INDICATORS, ble_state);
const uint8_t underglow_bat_lhs[] = DT_PROP(UNDERGLOW_INDICATORS, bat_lhs);
const uint8_t underglow_bat_rhs[] = DT_PROP(UNDERGLOW_INDICATORS, bat_rhs);

#define HEXRGB(R, G, B)                                                                            \
    ((struct led_rgb){                                                                             \
        r : (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX * (R)) / 0xff,                                       \
        g : (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX * (G)) / 0xff,                                       \
        b : (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX * (B)) / 0xff                                        \
    })
const struct led_rgb red = HEXRGB(0xff, 0x00, 0x00);
const struct led_rgb yellow = HEXRGB(0xff, 0xff, 0x00);
const struct led_rgb green = HEXRGB(0x00, 0xff, 0x00);
const struct led_rgb dull_green = HEXRGB(0x00, 0xff, 0x68);
const struct led_rgb magenta = HEXRGB(0xff, 0x00, 0xff);
const struct led_rgb white = HEXRGB(0xff, 0xff, 0xff);
const struct led_rgb lilac = HEXRGB(0x6b, 0x1f, 0xce);

static void zmk_led_battery_level(int bat_level, const uint8_t *addresses, size_t addresses_len) {
    struct led_rgb bat_colour;

    if (bat_level > 40) {
        bat_colour = green;
    } else if (bat_level > 20) {
        bat_colour = yellow;
    } else {
        bat_colour = red;
    }

    // originally, six levels, 0 .. 100

    for (int i = 0; i < addresses_len; i++) {
        int min_level = (i * 100) / (addresses_len - 1);
        if (bat_level >= min_level) {
            status_pixels[addresses[i]] = bat_colour;
        }
    }
}

static void zmk_led_fill(struct led_rgb color, const uint8_t *addresses, size_t addresses_len) {
    for (int i = 0; i < addresses_len; i++) {
        status_pixels[addresses[i]] = color;
    }
}

#define ZMK_LED_NUMLOCK_BIT BIT(0)
#define ZMK_LED_CAPSLOCK_BIT BIT(1)
#define ZMK_LED_SCROLLLOCK_BIT BIT(2)

static int zmk_led_generate_status(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        status_pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    // BATTERY STATUS
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    zmk_led_battery_level(zmk_battery_state_of_charge(), underglow_bat_lhs,
                          DT_PROP_LEN(UNDERGLOW_INDICATORS, bat_lhs));
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t peripheral_level = 0;
    int rc = zmk_split_central_get_peripheral_battery_level(0, &peripheral_level);

    if (rc == 0) {
        zmk_led_battery_level(peripheral_level, underglow_bat_rhs,
                              DT_PROP_LEN(UNDERGLOW_INDICATORS, bat_rhs));
    } else if (rc == -ENOTCONN) {
        zmk_led_fill(red, underglow_bat_rhs, DT_PROP_LEN(UNDERGLOW_INDICATORS, bat_rhs));
    } else if (rc == -EINVAL) {
        LOG_ERR("Invalid peripheral index requested for battery level read: 0");
    }
#endif // CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING
#endif // CONFIG_ZMK_BATTERY_REPORTING

    // CAPSLOCK/NUMLOCK/SCROLLOCK STATUS
    zmk_hid_indicators_t led_flags = zmk_hid_indicators_get_current_profile();

    if (led_flags & ZMK_LED_CAPSLOCK_BIT)
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, capslock)] = red;
    if (led_flags & ZMK_LED_NUMLOCK_BIT)
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, numlock)] = red;
    if (led_flags & ZMK_LED_SCROLLLOCK_BIT)
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, scrolllock)] = red;

    // LAYER STATUS
    for (uint8_t i = 0; i < DT_PROP_LEN(UNDERGLOW_INDICATORS, layer_state); i++) {
        if (zmk_keymap_layer_active(i))
            status_pixels[underglow_layer_state[i]] = magenta;
    }

    struct zmk_endpoint_instance active_endpoint = zmk_endpoints_selected();

    if (!zmk_endpoints_preferred_transport_is_active())
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, output_fallback)] = red;

#if IS_ENABLED(CONFIG_ZMK_BLE)
    int active_ble_profile_index = zmk_ble_active_profile_index();
    for (uint8_t i = 0;
         i < MIN(ZMK_BLE_PROFILE_COUNT, DT_PROP_LEN(UNDERGLOW_INDICATORS, ble_state)); i++) {
        int8_t status = zmk_ble_profile_status(i);
        int ble_pixel = underglow_ble_state[i];
        if (status == 2 && active_endpoint.transport == ZMK_TRANSPORT_BLE &&
            active_ble_profile_index == i) { // connected AND active
            status_pixels[ble_pixel] = white;
        } else if (status == 2) { // connected
            status_pixels[ble_pixel] = dull_green;
        } else if (status == 1) { // paired
            status_pixels[ble_pixel] = red;
        } else if (status == 0) { // unused
            status_pixels[ble_pixel] = lilac;
        }
    }
#endif

    enum zmk_usb_conn_state usb_state = zmk_usb_get_conn_state();
    if (usb_state == ZMK_USB_CONN_HID &&
        active_endpoint.transport == ZMK_TRANSPORT_USB) { // connected AND active
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, usb_state)] = white;
    } else if (usb_state == ZMK_USB_CONN_HID) { // connected
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, usb_state)] = dull_green;
    } else if (usb_state == ZMK_USB_CONN_POWERED) { // powered
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, usb_state)] = red;
    } else if (usb_state == ZMK_USB_CONN_NONE) { // disconnected
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, usb_state)] = lilac;
    }

    int16_t blend = 256;
    if (state.status_animation_step < (500 / 25)) {
        blend = ((state.status_animation_step * 256) / (500 / 25));
    } else if (state.status_animation_step > (8000 / 25)) {
        blend = 256 - (((state.status_animation_step - (8000 / 25)) * 256) / (2000 / 25));
    }
    if (blend < 0)
        blend = 0;
    if (blend > 256)
        blend = 256;

    return blend;
}
#endif // underglow_indicators exists

static inline struct led_rgb hue_sat(int hue, int sat) {
    struct zmk_led_hsb hsb = state.color;
    hsb.h = hue;
    hsb.s = sat;
    return hsb_to_rgb(hsb_scale_min_max(hsb));
}

static void zmk_rgb_underglow_tick(struct k_work *work) {
    switch (state.current_effect) {
    case UNDERGLOW_EFFECT_SOLID:
        zmk_rgb_underglow_effect_solid();
        break;
    case UNDERGLOW_EFFECT_BREATHE:
        zmk_rgb_underglow_effect_breathe();
        break;
    case UNDERGLOW_EFFECT_SPECTRUM:
        zmk_rgb_underglow_effect_spectrum();
        break;
    case UNDERGLOW_EFFECT_SWIRL:
        zmk_rgb_underglow_effect_swirl();
        break;
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    case UNDERGLOW_EFFECT_LAYER_INDICATORS:
        zmk_rgb_underglow_effect_layer();
        break;
#endif
    }

    zmk_led_write_pixels();
}

K_WORK_DEFINE(underglow_tick_work, zmk_rgb_underglow_tick);

static void zmk_rgb_underglow_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_tick_work);
}

K_TIMER_DEFINE(underglow_tick, zmk_rgb_underglow_tick_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            if (state.on) {
                k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
            }
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
            if (state.layer_enabled) {
                zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), true);
            }
#endif
            return 0;
        }

        return rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_underglow, "rgb/underglow", NULL, rgb_settings_set, NULL, NULL);

static void zmk_rgb_underglow_save_state_work(struct k_work *_work) {
    settings_save_one("rgb/underglow/state", &state, sizeof(state));
}

static struct k_work_delayable underglow_save_work;
#endif

/* ------------------------------------------------------------------ */
/* Go60 startup animation                                             */
/* ------------------------------------------------------------------ */

static uint32_t anim_rng_state;
static uint32_t anim_rand(void) {
    anim_rng_state ^= anim_rng_state << 13;
    anim_rng_state ^= anim_rng_state >> 17;
    anim_rng_state ^= anim_rng_state << 5;
    return anim_rng_state;
}
/* 0..65535 mapped to 0..255 */
static uint8_t anim_randf_byte(void) { return (uint8_t)(anim_rand() >> 24); }



#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

/*
 * Go60 startup animation: 3D particle rain + face logo.
 *
 * Both halves: sparse cyan particles fall with gravity through the key grid.
 * Particles have three depth layers (front/mid/back) giving the illusion of
 * depth via brightness and speed differences.
 *
 * Right half only: after the rain, a "journey pixel" falls down C1, swings
 * around the bottom row, rises up C3, then settles into R2C2 (the eye).
 * The surrounding logo fades in as white pixels.
 *
 * Logo (RH C1-C4, R1-R4):
 *   WHT WHT WHT BLK
 *   WHT CYN WHT BLK
 *   WHT WHT WHT WHT
 *   WHT WHT WHT BLK
 */

/* --- Fixed point 8.8 helpers --- */
#define FP8           8
#define FP(x)         ((int16_t)((x) * (1 << FP8)))  /* float literal to fp */
#define FP_INT(x)     ((x) >> FP8)                    /* fp to integer */
#define FP_ABS(x)     ((x) < 0 ? -(x) : (x))
/* Multiply: keep full precision */
#define FP_MUL(a, b)  ((int16_t)(((int32_t)(a) * (b)) >> FP8))
/* Decay a uint8_t: val * (n/256), e.g. n=171 for 0.67 */
#define DECAY8(v, n)  ((uint8_t)(((uint16_t)(v) * (n)) >> 8))

/* --- Per-half key layout tables --- */

struct rain_key {
    uint8_t col;     /* 0-5 */
    uint8_t row;     /* 0-5, index into key_buf */
    int16_t y_fp;    /* virtual y in fixed point (rows 0-3=main, 4=R5/T1) */
    uint8_t key_pos; /* ZMK key position for inverse lookup */
};

/*
 * Physical column layout (left to right on the PCB):
 *   LH: C6(col0) C5(col1) C4(col2) C3(col3) C2(col4) C1(col5)
 *   RH: C1(col0) C2(col1) C3(col2) C4(col3) C5(col4) C6(col5)
 */
#if defined(CONFIG_BOARD_GO60_LH)
static const struct rain_key rain_keys[] = {
    /* Main grid: 4 rows x 6 cols */
    {0,0,FP(0),0},  {1,0,FP(0),1},  {2,0,FP(0),2},  {3,0,FP(0),3},  {4,0,FP(0),4},  {5,0,FP(0),5},
    {0,1,FP(1),12}, {1,1,FP(1),13}, {2,1,FP(1),14}, {3,1,FP(1),15}, {4,1,FP(1),16}, {5,1,FP(1),17},
    {0,2,FP(2),24}, {1,2,FP(2),25}, {2,2,FP(2),26}, {3,2,FP(2),27}, {4,2,FP(2),28}, {5,2,FP(2),29},
    {0,3,FP(3),36}, {1,3,FP(3),37}, {2,3,FP(3),38}, {3,3,FP(3),39}, {4,3,FP(3),40}, {5,3,FP(3),41},
    /* R5: C4=col2, C3=col3, C2=col4 */
    {2,4,FP(4),48}, {3,4,FP(4),49}, {4,4,FP(4),50},
    /* T1: C1=col5 */
    {5,5,FP(4),54},
};
#define NUM_RAIN_COLS  6
static const uint8_t rain_cols[] = {0, 1, 2, 3, 4, 5};
#else /* CONFIG_BOARD_GO60_RH */
static const struct rain_key rain_keys[] = {
    /* Main grid: 4 rows x 6 cols */
    {0,0,FP(0),6},  {1,0,FP(0),7},  {2,0,FP(0),8},  {3,0,FP(0),9},  {4,0,FP(0),10}, {5,0,FP(0),11},
    {0,1,FP(1),18}, {1,1,FP(1),19}, {2,1,FP(1),20}, {3,1,FP(1),21}, {4,1,FP(1),22}, {5,1,FP(1),23},
    {0,2,FP(2),30}, {1,2,FP(2),31}, {2,2,FP(2),32}, {3,2,FP(2),33}, {4,2,FP(2),34}, {5,2,FP(2),35},
    {0,3,FP(3),42}, {1,3,FP(3),43}, {2,3,FP(3),44}, {3,3,FP(3),45}, {4,3,FP(3),46}, {5,3,FP(3),47},
    /* R5: C2=col1, C3=col2, C4=col3 */
    {1,4,FP(4),51}, {2,4,FP(4),52}, {3,4,FP(4),53},
    /* T1 (innermost thumb, physically pos 59 after pixel-lookup fix) */
    {0,5,FP(4),59},
};
/* RH excludes col3 (C4, black keycap column) from rain */
#define NUM_RAIN_COLS  5
static const uint8_t rain_cols[] = {0, 1, 2, 4, 5};
#endif

#define NUM_RAIN_KEYS  (sizeof(rain_keys) / sizeof(rain_keys[0]))

/* Does this column have keys at virtual y=4? (R5 or T1) */
static bool col_has_y4(uint8_t col) {
    for (int i = 0; i < (int)NUM_RAIN_KEYS; i++) {
        if (rain_keys[i].col == col && FP_INT(rain_keys[i].y_fp) == 4)
            return true;
    }
    return false;
}

/* --- Rain particle --- */

#define MAX_PARTICLES 24
#define FRAME_MS      25

/* Depth layers: front (75%), mid (12.5%), back (12.5%) */
struct depth_layer {
    int16_t z_fp;     /* depth: affects gravity and speed */
    uint8_t bright;   /* max brightness */
};
static const struct depth_layer layers[] = {
    { FP(1),    255 },  /* front */
    { FP(0.55), 160 },  /* mid */
    { FP(0.25),  90 },  /* back */
};
/* Round-robin pattern: 6 front, 1 mid, 1 back */
static const uint8_t layer_pattern[] = {0, 0, 0, 0, 0, 0, 1, 2};
#define LAYER_PATTERN_LEN 8

struct particle {
    int16_t y_fp;       /* vertical position, fixed point */
    int16_t vy_fp;      /* vertical velocity, fixed point */
    int16_t z_fp;       /* depth (affects gravity scaling) */
    uint8_t col;        /* column index */
    uint8_t bright;     /* max brightness */
    uint8_t max_y;      /* 3 or 4 depending on column */
    uint8_t alive;
};

/* --- Animation tuning constants (matching JS prototype) --- */
#define SPAWN_RATE_FP   FP(0.13)    /* particles per frame */
#define GRAVITY_FP      FP(0.020)   /* rows/frame^2 */
#define PROXIMITY_FP    FP(0.60)    /* key activation radius */
#define DECAY_NUM       171         /* 171/256 = 0.668 */
#define TRAIL_DECAY_NUM 141         /* 141/256 = 0.551 */
#define HEAT_SELF_FP    FP(1.0)
#define HEAT_NEIGH_FP   FP(0.5)
#define HEAT_DECAY_NUM  230         /* 230/256 = 0.898 */

/* Timeline frames */
#define RAMP_END    10
#define FULL_END    60
#define STOP_BACK   70
#define STOP_MID    80
#define JOURNEY_START 95

/* --- Per-key RGB buffer --- */
struct key_rgb {
    uint8_t r, g, b;
};

static void decay_key_buf(struct key_rgb buf[][6], int rows, uint8_t factor) {
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < 6; c++) {
            buf[r][c].r = DECAY8(buf[r][c].r, factor);
            buf[r][c].g = DECAY8(buf[r][c].g, factor);
            buf[r][c].b = DECAY8(buf[r][c].b, factor);
        }
}

static void flush_key_buf(struct key_rgb buf[][6], const int8_t *key_to_led) {
    memset(pixels, 0, sizeof(struct led_rgb) * STRIP_NUM_PIXELS);
    for (int i = 0; i < (int)NUM_RAIN_KEYS; i++) {
        int led = key_to_led[rain_keys[i].key_pos];
        if (led >= 0) {
            uint8_t row = rain_keys[i].row;
            uint8_t col = rain_keys[i].col;
            pixels[led].r = buf[row][col].r;
            pixels[led].g = buf[row][col].g;
            pixels[led].b = buf[row][col].b;
        }
    }
    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

/* --- Heat map for column selection --- */

static bool cols_are_neighbors(uint8_t a, uint8_t b) {
    /* Columns are neighbors only if both are in rain_cols and adjacent */
    bool a_found = false, b_found = false;
    for (int i = 0; i < NUM_RAIN_COLS; i++) {
        if (rain_cols[i] == a) a_found = true;
        if (rain_cols[i] == b) b_found = true;
    }
    if (!a_found || !b_found) return false;
    return (a == b + 1) || (b == a + 1);
}

/* --- Main animation --- */

static void startup_anim_handler(void *p1, void *p2, void *p3) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    ext_power_enable(ext_power);
    k_msleep(100);
#endif

    /* Build inverse lookup: key position -> LED index */
    int8_t key_to_led[60];
    memset(key_to_led, -1, sizeof(key_to_led));
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int pos = rgb_pixel_lookup(i);
        if (pos >= 0 && pos < 60)
            key_to_led[pos] = (int8_t)i;
    }

    anim_rng_state = k_uptime_get_32() ^ 0xFA4CE;
    memset(pixels, 0, sizeof(struct led_rgb) * STRIP_NUM_PIXELS);

    struct key_rgb key_buf[6][6];  /* rows 0-5, cols 0-5 */
    memset(key_buf, 0, sizeof(key_buf));

    struct particle parts[MAX_PARTICLES];
    int n_parts = 0;

    int16_t heat[6] = {0};
    int16_t spawn_accum = 0;
    uint8_t layer_iter = 0;

#if defined(CONFIG_BOARD_GO60_RH)
    /* Journey pixel state */
    int16_t j_y_fp = 0, j_vy_fp = FP(0.04);
    int j_seg = 0;
    bool journey_active = false;
    bool journey_phys_done = false;
#endif

#if defined(CONFIG_BOARD_GO60_RH)
    /* Journey path: physics-driven portion (fall, swing, rise) */
    static const struct { uint8_t row; uint8_t col; int16_t y_fp; } j_path[] = {
        {0, 0, FP(0)},  /* R1C1 */
        {1, 0, FP(1)},  /* R2C1 */
        {2, 0, FP(2)},  /* R3C1 */
        {3, 0, FP(3)},  /* R4C1 */
        {5, 0, FP(4)},  /* T1 */
        {4, 1, FP(4)},  /* R5C2 */
        {4, 2, FP(4)},  /* R5C3 */
        {3, 2, FP(3)},  /* R4C3 */
        {2, 2, FP(2)},  /* R3C3 */
        {1, 2, FP(1)},  /* R2C3 */
        {0, 2, FP(0)},  /* R1C3 - apex */
    };
    #define J_PATH_LEN (sizeof(j_path) / sizeof(j_path[0]))
    #define J_GRAV_FP  FP(0.025)

    /* Keyframe landing: {row, col, hold_frames} */
    static const struct { uint8_t row; uint8_t col; uint8_t frames; } j_landing[] = {
        {0, 2, 8},   /* R1C3 - pause at apex */
        {0, 1, 6},   /* R1C2 - drift */
        {1, 1, 1},   /* R2C2 - eye */
    };
    #define J_LANDING_LEN (sizeof(j_landing) / sizeof(j_landing[0]))


#endif

    /* ---- Main frame loop ---- */
    int frame = 0;
    bool spawning = true;

    while (1) {
        /* Determine spawn parameters based on timeline */
        int16_t spawn_rate = SPAWN_RATE_FP;
        int16_t min_z = 0;

        if (frame < RAMP_END) {
            spawn_rate = (int16_t)((int32_t)SPAWN_RATE_FP * frame / RAMP_END);
        } else if (frame < FULL_END) {
            /* full rate, all layers */
        } else if (frame < STOP_BACK) {
            min_z = layers[2].z_fp + 1;  /* exclude back */
        } else if (frame < STOP_MID) {
            min_z = layers[1].z_fp + 1;  /* exclude back + mid */
        } else {
            spawning = false;
        }

#if defined(CONFIG_BOARD_GO60_RH)
        /* Exclude cols 0-2 on RH near journey start */
        bool exclude_inner = (frame >= STOP_BACK);

        /* Start journey pixel */
        if (frame == JOURNEY_START && !journey_active) {
            journey_active = true;
            j_y_fp = 0;
            j_vy_fp = FP(0.04);
            j_seg = 0;
            journey_phys_done = false;
        }
#endif

        /* Decay key buffer */
        decay_key_buf(key_buf, 6, DECAY_NUM);

        /* Decay heat */
        for (int c = 0; c < 6; c++)
            heat[c] = DECAY8(heat[c], HEAT_DECAY_NUM);

        /* Spawn particles */
        if (spawning) {
            /* Scale rate by column count / 6 */
            int16_t adj_rate = (int16_t)((int32_t)spawn_rate * NUM_RAIN_COLS / 6);
            /* Jitter: +/- 30% */
            int16_t jitter = (int16_t)((int32_t)adj_rate * ((int8_t)(anim_randf_byte() - 128)) * 3 / 512);
            spawn_accum += adj_rate + jitter;

            while (spawn_accum >= FP(1) && n_parts < MAX_PARTICLES) {
                spawn_accum -= FP(1);

                /* Pick column via heat-weighted selection */
                int16_t weights[6];
                int32_t total = 0;
                for (int i = 0; i < NUM_RAIN_COLS; i++) {
                    uint8_t c = rain_cols[i];
#if defined(CONFIG_BOARD_GO60_RH)
                    if (exclude_inner && c <= 2) {
                        weights[i] = 0;
                        continue;
                    }
#endif
                    weights[i] = (int16_t)(FP(1) / (FP(1) + FP_MUL(heat[c], FP(3))) + 1);
                    total += weights[i];
                }
                if (total == 0) break;

                int32_t r = (int32_t)(anim_rand() & 0xFFFF) * total >> 16;
                uint8_t col = rain_cols[0];
                for (int i = 0; i < NUM_RAIN_COLS; i++) {
                    r -= weights[i];
                    if (r <= 0) {
                        col = rain_cols[i];
                        break;
                    }
                }

                /* Apply heat */
                heat[col] = (heat[col] + HEAT_SELF_FP > 32767) ? 32767 : heat[col] + HEAT_SELF_FP;
                for (int c = 0; c < 6; c++) {
                    if (c != col && cols_are_neighbors(c, col))
                        heat[c] = (heat[c] + HEAT_NEIGH_FP > 32767) ? 32767 : heat[c] + HEAT_NEIGH_FP;
                }

                /* Pick layer (round-robin) */
                const struct depth_layer *layer;
                bool found = false;
                for (int attempt = 0; attempt < LAYER_PATTERN_LEN; attempt++) {
                    layer = &layers[layer_pattern[layer_iter % LAYER_PATTERN_LEN]];
                    layer_iter++;
                    if (layer->z_fp >= min_z) { found = true; break; }
                }
                if (!found) continue;

                /* Create particle */
                struct particle *p = &parts[n_parts++];
                p->col = col;
                p->z_fp = layer->z_fp;
                p->bright = layer->bright;
                int16_t start_offset = (int16_t)(-(anim_rand() % 384) - 128); /* -0.5 to -2.0 */
                p->y_fp = start_offset;
                p->vy_fp = FP(0.01) + FP_MUL(p->z_fp, FP(0.02));
                p->max_y = col_has_y4(col) ? 4 : 3;
                p->alive = 1;
            }
        }

        /* Update particles */
        for (int i = 0; i < n_parts; i++) {
            struct particle *p = &parts[i];
            if (!p->alive) continue;

            p->vy_fp += FP_MUL(GRAVITY_FP, p->z_fp);
            p->y_fp += p->vy_fp;

            if (p->y_fp > FP(p->max_y + 1) + FP(0.5)) {
                p->alive = 0;
                continue;
            }

            /* Light keys within proximity */
            for (int k = 0; k < (int)NUM_RAIN_KEYS; k++) {
                if (rain_keys[k].col != p->col) continue;
                int16_t dist = FP_ABS(p->y_fp - rain_keys[k].y_fp);
                if (dist < PROXIMITY_FP) {
                    int intensity = (int)p->bright * (PROXIMITY_FP - dist) / PROXIMITY_FP;
                    uint8_t row = rain_keys[k].row;
                    uint8_t col = rain_keys[k].col;
                    int g = key_buf[row][col].g + intensity;
                    int b = key_buf[row][col].b + intensity;
                    key_buf[row][col].g = g > 255 ? 255 : (uint8_t)g;
                    key_buf[row][col].b = b > 255 ? 255 : (uint8_t)b;
                }
            }
        }

        /* Compact dead particles */
        int write = 0;
        for (int read = 0; read < n_parts; read++) {
            if (parts[read].alive) {
                if (write != read) parts[write] = parts[read];
                write++;
            }
        }
        n_parts = write;

#if defined(CONFIG_BOARD_GO60_RH)
        /* Journey pixel physics */
        if (journey_active && !journey_phys_done) {
            int seg = j_seg < (int)J_PATH_LEN - 1 ? j_seg : (int)J_PATH_LEN - 2;
            int16_t dy = j_path[seg + 1].y_fp - j_path[seg].y_fp;

            if (dy > 0) j_vy_fp += J_GRAV_FP;        /* falling */
            else if (dy < 0) j_vy_fp -= J_GRAV_FP;   /* rising */
            /* horizontal: maintain speed */

            if (j_vy_fp <= 0) {
                journey_phys_done = true;
            } else {
                j_y_fp += j_vy_fp;
                j_seg = FP_INT(j_y_fp);
                if (j_seg >= (int)J_PATH_LEN - 1) {
                    j_seg = (int)J_PATH_LEN - 1;
                    journey_phys_done = true;
                }
            }

            /* Trail decay (faster than rain) */
            decay_key_buf(key_buf, 6, TRAIL_DECAY_NUM);

            /* Light nearest key */
            int si = j_seg;
            if (si >= (int)J_PATH_LEN) si = (int)J_PATH_LEN - 1;
            key_buf[j_path[si].row][j_path[si].col] =
                (struct key_rgb){.r = 0, .g = 255, .b = 255};
        }
#endif

        /* Flush to LEDs */
        flush_key_buf(key_buf, key_to_led);
        k_msleep(FRAME_MS);
        frame++;

        /* Exit condition */
        bool rain_done = !spawning && n_parts == 0;
#if defined(CONFIG_BOARD_GO60_RH)
        if (rain_done && journey_phys_done) break;
#else
        if (rain_done) break;
#endif
    }

#if defined(CONFIG_BOARD_GO60_RH)
    /* ---- Keyframe landing ---- */
    for (int k = 0; k < (int)J_LANDING_LEN; k++) {
        for (int f = 0; f < j_landing[k].frames; f++) {
            decay_key_buf(key_buf, 6, TRAIL_DECAY_NUM);
            key_buf[j_landing[k].row][j_landing[k].col] =
                (struct key_rgb){.r = 0, .g = 255, .b = 255};
            flush_key_buf(key_buf, key_to_led);
            k_msleep(FRAME_MS);
        }
    }

    /* Flash the eye */
    for (int f = 0; f < 3; f++) {
        key_buf[1][1] = (struct key_rgb){.r = 0, .g = 255, .b = 255};
        flush_key_buf(key_buf, key_to_led);
        k_msleep(40);
        key_buf[1][1] = (struct key_rgb){.r = 0, .g = 180, .b = 180};
        flush_key_buf(key_buf, key_to_led);
        k_msleep(40);
    }

    /* ---- Ripple outward from eye within 4x4 face area ---- */
    /* Keys grouped by Chebyshev distance from eye (row=1, col=1) */
    static const uint8_t ring1[][2] = {
        {0,0},{0,1},{0,2},{1,0},{1,2},{2,0},{2,1},{2,2},
    };
    static const uint8_t ring2[][2] = {
        {0,3},{1,3},{2,3},{3,0},{3,1},{3,2},{3,3},
    };
    #define N_RING1 (sizeof(ring1) / sizeof(ring1[0]))
    #define N_RING2 (sizeof(ring2) / sizeof(ring2[0]))

    static const struct { const uint8_t (*keys)[2]; int count; } rings[] = {
        { ring1, N_RING1 },
        { ring2, N_RING2 },
    };

    for (int ring = 0; ring < 2; ring++) {
        /* Fade in this ring (white at 75% = 192) */
        for (int f = 0; f < 4; f++) {
            decay_key_buf(key_buf, 6, 179);  /* 179/256 ~ 0.70 */
            uint8_t bright = (uint8_t)((f + 1) * 48);  /* up to 192 */
            for (int k = 0; k < rings[ring].count; k++) {
                uint8_t r = rings[ring].keys[k][0];
                uint8_t c = rings[ring].keys[k][1];
                int rv = key_buf[r][c].r + bright;
                int gv = key_buf[r][c].g + bright;
                int bv = key_buf[r][c].b + bright;
                key_buf[r][c].r = rv > 192 ? 192 : (uint8_t)rv;
                key_buf[r][c].g = gv > 192 ? 192 : (uint8_t)gv;
                key_buf[r][c].b = bv > 192 ? 192 : (uint8_t)bv;
            }
            key_buf[1][1] = (struct key_rgb){.r = 0, .g = 255, .b = 255};
            flush_key_buf(key_buf, key_to_led);
            k_msleep(FRAME_MS);
        }
        /* Let it decay */
        for (int f = 0; f < 3; f++) {
            decay_key_buf(key_buf, 6, 153);  /* 153/256 ~ 0.60 */
            key_buf[1][1] = (struct key_rgb){.r = 0, .g = 255, .b = 255};
            flush_key_buf(key_buf, key_to_led);
            k_msleep(FRAME_MS);
        }
    }

    /* Hold eye as ripple fades */
    for (int f = 0; f < 15; f++) {
        decay_key_buf(key_buf, 6, 179);
        key_buf[1][1] = (struct key_rgb){.r = 0, .g = 255, .b = 255};
        flush_key_buf(key_buf, key_to_led);
        k_msleep(FRAME_MS);
    }

    /* Hold eye alone */
    memset(key_buf, 0, sizeof(key_buf));
    key_buf[1][1] = (struct key_rgb){.r = 0, .g = 255, .b = 255};
    flush_key_buf(key_buf, key_to_led);
    k_msleep(800);

    /* Fade out eye */
    for (int f = 0; f < 20; f++) {
        key_buf[1][1].g = key_buf[1][1].g > 14 ? key_buf[1][1].g - 14 : 0;
        key_buf[1][1].b = key_buf[1][1].b > 14 ? key_buf[1][1].b - 14 : 0;
        flush_key_buf(key_buf, key_to_led);
        k_msleep(FRAME_MS);
    }
#endif /* CONFIG_BOARD_GO60_RH */

    memset(pixels, 0, sizeof(struct led_rgb) * STRIP_NUM_PIXELS);
    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!state.on && !state.layer_enabled) {
        ext_power_disable(ext_power);
    }
#endif
}

#else /* !UNDERGLOW_LAYER_ENABLED */

/* Fallback: simple rainbow sweep when RGB layer not enabled */
static void startup_anim_handler(void *p1, void *p2, void *p3) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    ext_power_enable(ext_power);
    k_msleep(100);
#endif
    int brt = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX;
    for (int frame = 0; frame < 40; frame++) {
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            struct zmk_led_hsb hsb = {
                .h = (HUE_MAX / STRIP_NUM_PIXELS * i + HUE_MAX * frame / 40) % HUE_MAX,
                .s = 100,
                .b = brt * (40 - frame) / 40,
            };
            pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
        }
        led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
        k_msleep(25);
    }
    memset(pixels, 0, sizeof(struct led_rgb) * STRIP_NUM_PIXELS);
    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!state.on && !state.layer_enabled) {
        ext_power_disable(ext_power);
    }
#endif
}

#endif /* UNDERGLOW_LAYER_ENABLED */

/* Dedicated thread for startup animation so it doesn't block the
 * low-priority work queue (which ZMK uses for BLE/split init). */
#define ANIM_STACK_SIZE 2048
K_THREAD_STACK_DEFINE(anim_stack, ANIM_STACK_SIZE);
static struct k_thread anim_thread_data;

static int zmk_rgb_underglow_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("External power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif

    state = (struct rgb_underglow_state){
        color : {
            h : CONFIG_ZMK_RGB_UNDERGLOW_HUE_START,
            s : CONFIG_ZMK_RGB_UNDERGLOW_SAT_START,
            b : CONFIG_ZMK_RGB_UNDERGLOW_BRT_START,
        },
        animation_speed : CONFIG_ZMK_RGB_UNDERGLOW_SPD_START,
        current_effect : CONFIG_ZMK_RGB_UNDERGLOW_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_ON_START)
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&underglow_save_work, zmk_rgb_underglow_save_state_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

    if (state.on) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(25));
    }
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.layer_enabled) {
        zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), true);
    }
#endif

    // Kick off startup animation on the low-priority work queue
    k_thread_create(&anim_thread_data, anim_stack, ANIM_STACK_SIZE,
                    (k_thread_entry_t)startup_anim_handler,
                    NULL, NULL, NULL,
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);

    return 0;
}

int zmk_rgb_underglow_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

int zmk_rgb_underglow_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;

    *on_off = state.on || state.layer_enabled;
    return 0;
}

void zmk_rgb_set_ext_power(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power == NULL)
        return;
    int c_power = ext_power_get(ext_power);
    if (c_power < 0) {
        LOG_ERR("Unable to examine EXT_POWER: %d", c_power);
        c_power = 0;
    }
    int desired_state = state.on || state.status_active;

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    // force power off, when battery low (<10%)
    if (state.on && !state.status_active) {
        if (zmk_battery_state_of_charge() < 10) {
            desired_state = false;
        }
    }
#endif // CONFIG_ZMK_BATTERY_REPORTING

    if (desired_state && !c_power) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    } else if (!desired_state && c_power) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif // CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER
}

int zmk_rgb_underglow_on(void) {
    zmk_rgb_underglow_transient_on();
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.current_effect == UNDERGLOW_EFFECT_LAYER_INDICATORS) {
        state.layer_enabled = true;
    }
#endif
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_transient_on(void) {
    if (!led_strip)
        return -ENODEV;

    state.on = true;
    zmk_rgb_set_ext_power();

    state.animation_step = 0;
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(25));

    return 0;
}

static void zmk_rgb_underglow_off_handler(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    zmk_led_write_pixels();
}

K_WORK_DEFINE(underglow_off_work, zmk_rgb_underglow_off_handler);

int zmk_rgb_underglow_off(void) {
    zmk_rgb_underglow_transient_off();
    state.layer_enabled = false;
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_transient_off(void) {
    if (!led_strip)
        return -ENODEV;

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);

    k_timer_stop(&underglow_tick);
    state.on = false;
    zmk_rgb_set_ext_power();

    return 0;
}

int zmk_rgb_underglow_calc_effect(int direction) {
    return (state.current_effect + UNDERGLOW_EFFECT_NUMBER + direction) % UNDERGLOW_EFFECT_NUMBER;
}

int zmk_rgb_underglow_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;

    if (effect < 0 || effect >= UNDERGLOW_EFFECT_NUMBER) {
        return -EINVAL;
    }

    state.current_effect = effect;
    state.animation_step = 0;
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    state.layer_enabled = (effect == UNDERGLOW_EFFECT_LAYER_INDICATORS);
#endif
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_cycle_effect(int direction) {
    return zmk_rgb_underglow_select_effect(zmk_rgb_underglow_calc_effect(direction));
}

int zmk_rgb_underglow_toggle(void) {
    return state.on ? zmk_rgb_underglow_off() : zmk_rgb_underglow_on();
}

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

static struct led_rgb hex_to_rgb(uint8_t r, uint8_t g, uint8_t b) {
    struct zmk_led_hsb hsb = state.color;
    return (struct led_rgb){
        r : (hsb.b * (r)) / 0xff,
        g : (hsb.b * (g)) / 0xff,
        b : (hsb.b * (b)) / 0xff
    };
}

static int zmk_rgb_underglow_apply_rgbmap(const struct zmk_behavior_binding *bindings,
                                          size_t rgbmap_len, uint8_t layer) {
    int rc = 0;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        if (midx >= ZMK_KEYMAP_LEN) {
            LOG_DBG("out of range");
        } else {
            const struct device *dev = zmk_behavior_get_binding(bindings[midx].behavior_dev);

            if (dev == NULL) {
                continue;
            }

            const struct behavior_driver_api *api = (const struct behavior_driver_api *)dev->api;

            if (api->binding_pressed == NULL) {
                continue;
            }
            struct zmk_behavior_binding_event event = {
                .position = midx, .layer = layer, .timestamp = k_uptime_get()};

            int color = api->binding_pressed((struct zmk_behavior_binding *)&bindings[midx], event);

            if (color > 0) {
                pixels[i] =
                    hex_to_rgb((color & 0xFF0000) >> 16, (color & 0xFF00) >> 8, color & 0xFF);
                rc = 1;
            } else {
                pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
            }
        }
    }
    return rc;
}

static void zmk_rgb_underglow_set_layer(uint8_t layer, bool wakeup) {
    LOG_DBG("state.layer: %d state.on: %d", state.layer_enabled, state.on);
    if (!state.layer_enabled)
        return;

    const struct zmk_behavior_binding *rgbmap = rgb_underglow_get_bindings(layer);
    if (rgbmap != NULL && zmk_rgb_underglow_apply_rgbmap(rgbmap, ZMK_KEYMAP_LEN, layer)) {
        if (!state.on) {
            if (!wakeup) {
                LOG_DBG("rgb off and no wakeup, abort refresh");
                return;
            }
            zmk_rgb_underglow_transient_on();
        }
        k_timer_stop(&underglow_tick);
        state.animation_step = 0;
        int fade_delay = zmk_rgbmap_fade_delay(layer);
        if (fade_delay >= 0) {
            k_timer_start(&underglow_tick, K_SECONDS(fade_delay), K_MSEC(50));
        }
        LOG_DBG("write pixels");
        zmk_led_write_pixels();
    } else {
        if (state.on)
            zmk_rgb_underglow_transient_off();
    }
}
#endif /* IS_ENABLED(UNDERGLOW_LAYER_ENABLED) */

static void zmk_led_write_pixels_work(struct k_work *work);
static void zmk_rgb_underglow_status_update(struct k_timer *timer);

K_WORK_DEFINE(underglow_write_work, zmk_led_write_pixels_work);
K_TIMER_DEFINE(underglow_status_update_timer, zmk_rgb_underglow_status_update, NULL);

static void zmk_rgb_underglow_status_update(struct k_timer *timer) {
    if (!state.status_active)
        return;
    state.status_animation_step++;
    if (state.status_animation_step > (10000 / 25)) {
        state.status_active = false;
        k_timer_stop(&underglow_status_update_timer);
    }
    if (!k_work_is_pending(&underglow_write_work))
        k_work_submit(&underglow_write_work);
}

static void zmk_led_write_pixels_work(struct k_work *work) {
    zmk_led_write_pixels();
    if (!state.status_active) {
        zmk_rgb_set_ext_power();
    }
}

int zmk_rgb_underglow_status(void) {
    if (!state.status_active) {
        state.status_animation_step = 0;
    } else {
        if (state.status_animation_step > (500 / 25)) {
            state.status_animation_step = 500 / 25;
        }
    }
    state.status_active = true;
    zmk_led_write_pixels();
    zmk_rgb_set_ext_power();

    k_timer_start(&underglow_status_update_timer, K_NO_WAIT, K_MSEC(25));

    return 0;
}

int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }

    state.color = color;

    return 0;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction) {
    struct zmk_led_hsb color = state.color;

    color.h += HUE_MAX + (direction * CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP);
    color.h %= HUE_MAX;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction) {
    struct zmk_led_hsb color = state.color;

    int s = color.s + (direction * CONFIG_ZMK_RGB_UNDERGLOW_SAT_STEP);
    if (s < 0) {
        s = 0;
    } else if (s > SAT_MAX) {
        s = SAT_MAX;
    }
    color.s = s;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction) {
    struct zmk_led_hsb color = state.color;

    int b = color.b + (direction * CONFIG_ZMK_RGB_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);

    return color;
}

int zmk_rgb_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_hue(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_sat(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_brt(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.animation_speed == 1 && direction < 0) {
        return 0;
    }

    state.animation_speed += direction;

    if (state.animation_speed > 5) {
        state.animation_speed = 5;
    }

    return zmk_rgb_underglow_save_state();
}

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||                                          \
    IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB) || IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
struct rgb_underglow_sleep_state {
    bool is_awake;
    bool rgb_state_before_sleeping;
};

static int rgb_underglow_auto_state(bool target_wake_state) {
    static struct rgb_underglow_sleep_state sleep_state = {
        is_awake : true,
        rgb_state_before_sleeping : false
    };

    // wake up event while awake, or sleep event while sleeping -> no-op
    if (target_wake_state == sleep_state.is_awake) {
        return 0;
    }
    sleep_state.is_awake = target_wake_state;

    if (sleep_state.is_awake) {
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
        if (state.layer_enabled) {
            zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), true);
            return 0;
        }
#endif
        if (sleep_state.rgb_state_before_sleeping) {
            return zmk_rgb_underglow_transient_on();
        } else {
            return zmk_rgb_underglow_transient_off();
        }
    } else {
        sleep_state.rgb_state_before_sleeping = state.on;
        return zmk_rgb_underglow_transient_off();
    }
}

static int rgb_underglow_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (as_zmk_split_peripheral_layer_changed(eh)) {
        const struct zmk_split_peripheral_layer_changed *ev =
            as_zmk_split_peripheral_layer_changed(eh);
        LOG_DBG("zmk_split_peripheral_layer_changed: %08x", ev->layers);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        set_peripheral_layers_state(ev->layers);
#endif
        uint8_t layer = rgb_underglow_top_layer();
        LOG_DBG("top layer: %d", layer);
        zmk_rgb_underglow_set_layer(layer, true);
        return 0;
    }
    if (as_zmk_underglow_color_changed(eh)) {
        const struct zmk_underglow_color_changed *ev = as_zmk_underglow_color_changed(eh);
        uint8_t layer = rgb_underglow_top_layer();
        LOG_DBG("refresh layers %d, current: %d, wakeup: %d", ev->layers, layer, ev->wakeup);
        if ((ev->layers & (BIT(layer))) == BIT(layer)) {
            zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), ev->wakeup);
        }
        return 0;
    }
#endif /* UNDERGLOW_LAYER_ENABLED */

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_usb_is_powered());
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(rgb_underglow, rgb_underglow_event_listener);
#endif // IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||
       // IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB) ||
       // IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_usb_conn_state_changed);
#endif

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_split_peripheral_layer_changed);
ZMK_SUBSCRIPTION(rgb_underglow, zmk_underglow_color_changed);
#endif

SYS_INIT(zmk_rgb_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
