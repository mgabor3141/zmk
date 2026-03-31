/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/hid_remap.h>

#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/modifiers.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define OS_LINUX 0
#define OS_MACOS 1

/* Per-endpoint OS type, stored in flash */
static uint8_t endpoint_os[ZMK_ENDPOINT_COUNT] = {0};
static uint8_t current_os = OS_LINUX;

/* --- Key override table for macOS --- */

/*
 * Applied AFTER the modifier bit swap. Each entry matches a modifier+key
 * combination and replaces the modifier bits. If exclude_mods are present
 * in the report, the override is skipped.
 *
 * The key field uses HID usage codes. key=0 is unused (no wildcard needed).
 */
struct key_override {
    uint8_t match_mods;    /* modifier bits that must be present */
    uint8_t exclude_mods;  /* if ANY of these are present, skip this override */
    uint8_t match_key;     /* HID keycode to match */
    uint8_t out_mods;      /* replacement modifier bits (replaces match_mods) */
    uint8_t out_key;       /* replacement key (0 = keep original) */
};

#define K_LEFT   HID_USAGE_KEY_KEYBOARD_LEFTARROW
#define K_RIGHT  HID_USAGE_KEY_KEYBOARD_RIGHTARROW
#define K_TAB    HID_USAGE_KEY_KEYBOARD_TAB
#define K_BSPC   HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE
#define K_DEL    HID_USAGE_KEY_KEYBOARD_DELETE_FORWARD
#define K_HOME   HID_USAGE_KEY_KEYBOARD_HOME
#define K_END    HID_USAGE_KEY_KEYBOARD_END
#define K_F13    HID_USAGE_KEY_KEYBOARD_F13
#define K_F21    HID_USAGE_KEY_KEYBOARD_F21
#define K_M      HID_USAGE_KEY_KEYBOARD_M
#define K_S      HID_USAGE_KEY_KEYBOARD_S

#define HYPER (MOD_LCTL | MOD_LSFT | MOD_LALT | MOD_LGUI)

static const struct key_override mac_overrides[] = {
    /* Word navigation: Cmd+arrow/bspc/del -> Alt+same (but NOT with Shift) */
    {MOD_LGUI, MOD_LSFT | MOD_RSFT, K_LEFT,  MOD_LALT, 0},
    {MOD_LGUI, MOD_LSFT | MOD_RSFT, K_RIGHT, MOD_LALT, 0},
    {MOD_LGUI, MOD_LSFT | MOD_RSFT, K_BSPC,  MOD_LALT, 0},
    {MOD_LGUI, MOD_LSFT | MOD_RSFT, K_DEL,   MOD_LALT, 0},

    /* Tab un-swap: Cmd+Tab -> Ctrl+Tab, Ctrl+Tab -> Cmd+Tab */
    {MOD_LGUI, 0, K_TAB, MOD_LCTL, 0},
    {MOD_LCTL, 0, K_TAB, MOD_LGUI, 0},

    /* Home/End -> Cmd+Left/Right (beginning/end of line on macOS) */
    {0, 0xFF, K_HOME, MOD_LGUI, K_LEFT},   /* exclude_mods=0xFF: only bare Home */
    {0, 0xFF, K_END,  MOD_LGUI, K_RIGHT},  /* exclude_mods=0xFF: only bare End */

    /* F13 -> Hyper+M (for Hammerspoon mic mute) */
    {0, 0xFF, K_F13, HYPER, K_M},

    /* F21 -> Hyper+S (for Hammerspoon sleep) */
    {0, 0xFF, K_F21, HYPER, K_S},
};

/* --- Modifier bit swap --- */

static zmk_mod_flags_t remap_mods(zmk_mod_flags_t mods) {
    zmk_mod_flags_t out = mods;

    /* Clear the bits we're remapping */
    out &= ~(MOD_LCTL | MOD_LGUI | MOD_RGUI);

    /* LCTRL -> LGUI */
    if (mods & MOD_LCTL) {
        out |= MOD_LGUI;
    }
    /* LGUI -> LCTRL */
    if (mods & MOD_LGUI) {
        out |= MOD_LCTL;
    }
    /* RGUI -> LCTRL */
    if (mods & MOD_RGUI) {
        out |= MOD_LCTL;
    }

    return out;
}

/* --- Key scanning helpers --- */

/*
 * Find a key in the HKRO report array. Returns the index, or -1 if not found.
 * For NKRO, the report is a bitmask; this function handles both formats.
 */
#if IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_NKRO)

static bool report_has_key(const struct zmk_hid_keyboard_report_body *body, uint8_t key) {
    if (key == 0) {
        return false;
    }
    uint8_t byte = key / 8;
    uint8_t bit = key % 8;
    if (byte >= sizeof(body->keys)) {
        return false;
    }
    return body->keys[byte] & BIT(bit);
}

static void report_remove_key(struct zmk_hid_keyboard_report_body *body, uint8_t key) {
    uint8_t byte = key / 8;
    uint8_t bit = key % 8;
    if (byte < sizeof(body->keys)) {
        body->keys[byte] &= ~BIT(bit);
    }
}

static void report_add_key(struct zmk_hid_keyboard_report_body *body, uint8_t key) {
    uint8_t byte = key / 8;
    uint8_t bit = key % 8;
    if (byte < sizeof(body->keys)) {
        body->keys[byte] |= BIT(bit);
    }
}

#else /* HKRO */

static bool report_has_key(const struct zmk_hid_keyboard_report_body *body, uint8_t key) {
    if (key == 0) {
        return false;
    }
    for (int i = 0; i < sizeof(body->keys); i++) {
        if (body->keys[i] == key) {
            return true;
        }
    }
    return false;
}

static void report_remove_key(struct zmk_hid_keyboard_report_body *body, uint8_t key) {
    for (int i = 0; i < sizeof(body->keys); i++) {
        if (body->keys[i] == key) {
            body->keys[i] = 0;
            return;
        }
    }
}

static void report_add_key(struct zmk_hid_keyboard_report_body *body, uint8_t key) {
    for (int i = 0; i < sizeof(body->keys); i++) {
        if (body->keys[i] == 0) {
            body->keys[i] = key;
            return;
        }
    }
}

#endif /* CONFIG_ZMK_HID_REPORT_TYPE_NKRO */

/* --- Apply remapping --- */

void zmk_hid_remap_apply(struct zmk_hid_keyboard_report_body *body) {
    if (current_os != OS_MACOS) {
        return;
    }

    /* Phase 1: Modifier bit swap */
    body->modifiers = remap_mods(body->modifiers);

    /* Phase 2: Key-specific overrides */
    for (int i = 0; i < ARRAY_SIZE(mac_overrides); i++) {
        const struct key_override *ov = &mac_overrides[i];

        /* Check if the matched key is in the report */
        if (!report_has_key(body, ov->match_key)) {
            continue;
        }

        /* Check required modifiers are present */
        if ((body->modifiers & ov->match_mods) != ov->match_mods) {
            continue;
        }

        /* Check exclusion modifiers are absent */
        if (ov->exclude_mods && (body->modifiers & ~ov->match_mods & ov->exclude_mods)) {
            continue;
        }

        /* Apply: replace matched modifiers with output modifiers */
        body->modifiers = (body->modifiers & ~ov->match_mods) | ov->out_mods;

        /* Replace key if specified */
        if (ov->out_key) {
            report_remove_key(body, ov->match_key);
            report_add_key(body, ov->out_key);
        }

        /* Only apply one override per key */
        break;
    }
}

uint8_t zmk_hid_remap_get_os(void) { return current_os; }

/* --- Per-endpoint persistence --- */

static int get_endpoint_index(void) {
    struct zmk_endpoint_instance ep = zmk_endpoint_get_selected();
    return zmk_endpoint_instance_to_index(ep);
}

static void apply_current_os(void) {
    int idx = get_endpoint_index();
    if (idx >= 0 && idx < ZMK_ENDPOINT_COUNT) {
        current_os = endpoint_os[idx];
    } else {
        current_os = OS_LINUX;
    }
    LOG_INF("HID remap: endpoint %d, os=%s", idx, current_os == OS_MACOS ? "macOS" : "Linux");
}

int zmk_hid_remap_set_os(uint8_t os_type) {
    int idx = get_endpoint_index();
    if (idx < 0 || idx >= ZMK_ENDPOINT_COUNT) {
        return -EINVAL;
    }

    endpoint_os[idx] = os_type;
    current_os = os_type;

    int ret = settings_save_one("hid_remap/os", &endpoint_os, sizeof(endpoint_os));
    if (ret < 0) {
        LOG_ERR("Failed to save HID remap settings: %d", ret);
    }

    LOG_INF("Set endpoint %d to os=%d", idx, os_type);
    return ret;
}

/* --- Settings --- */

static int hid_remap_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "os", &next) && !next) {
        if (len != sizeof(endpoint_os)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &endpoint_os, sizeof(endpoint_os)) >= 0 ? 0 : -EIO;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(hid_remap, "hid_remap", NULL, hid_remap_settings_set, NULL, NULL);

/* --- Init and endpoint listener --- */

static int hid_remap_init(void) {
    apply_current_os();
    return 0;
}

SYS_INIT(hid_remap_init, APPLICATION, 99);

static int endpoint_changed_cb(const zmk_event_t *eh) {
    struct zmk_endpoint_changed *evt = as_zmk_endpoint_changed(eh);
    if (evt) {
        int idx = zmk_endpoint_instance_to_index(evt->endpoint);
        if (idx >= 0 && idx < ZMK_ENDPOINT_COUNT) {
            current_os = endpoint_os[idx];
            LOG_INF("HID remap: endpoint changed to %d, os=%s", idx,
                    current_os == OS_MACOS ? "macOS" : "Linux");
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hid_remap_endpoint, endpoint_changed_cb);
ZMK_SUBSCRIPTION(hid_remap_endpoint, zmk_endpoint_changed);
