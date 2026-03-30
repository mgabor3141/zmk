/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_default_layer

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/settings/settings.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>

/* Per-endpoint default layer, stored in flash */
static uint8_t endpoint_layers[ZMK_ENDPOINT_COUNT] = {0};

static int get_endpoint_index(void) {
    struct zmk_endpoint_instance ep = zmk_endpoint_get_selected();
    return zmk_endpoint_instance_to_index(ep);
}

static int apply_default_layer(int ep_idx) {
    if (ep_idx < 0 || ep_idx >= ZMK_ENDPOINT_COUNT) {
        return -EINVAL;
    }

    uint8_t layer = endpoint_layers[ep_idx];
    if (layer >= ZMK_KEYMAP_LAYERS_LEN) {
        LOG_WRN("Stored default layer %d out of range, resetting to 0", layer);
        endpoint_layers[ep_idx] = 0;
        layer = 0;
    }

    LOG_INF("Applying default layer %d for endpoint %d", layer, ep_idx);
    return zmk_keymap_layer_to(layer, false);
}

static int save_settings(void) {
    return settings_save_one("def_layer/endpoints", &endpoint_layers, sizeof(endpoint_layers));
}

/* Settings load handler */
static int default_layer_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                      void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "endpoints", &next) && !next) {
        if (len != sizeof(endpoint_layers)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &endpoint_layers, sizeof(endpoint_layers)) >= 0 ? 0 : -EIO;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(def_layer, "def_layer", NULL, default_layer_settings_set, NULL,
                               NULL);

/* Boot: load settings then apply for current endpoint */
static int default_layer_init(void) {
    apply_default_layer(get_endpoint_index());
    return 0;
}

SYS_INIT(default_layer_init, APPLICATION, 99);

/* Endpoint change listener: auto-switch layer */
static int endpoint_changed_cb(const zmk_event_t *eh) {
    struct zmk_endpoint_changed *evt = as_zmk_endpoint_changed(eh);
    if (evt) {
        int idx = zmk_endpoint_instance_to_index(evt->endpoint);
        apply_default_layer(idx);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(default_layer_endpoint, endpoint_changed_cb);
ZMK_SUBSCRIPTION(default_layer_endpoint, zmk_endpoint_changed);

/* Behavior: &default_layer N saves layer N for current endpoint and applies it */
static int behavior_default_layer_init(const struct device *dev) { return 0; }

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    int ep_idx = get_endpoint_index();
    if (ep_idx < 0) {
        return ep_idx;
    }

    uint8_t layer = binding->param1;
    if (layer >= ZMK_KEYMAP_LAYERS_LEN) {
        LOG_ERR("Default layer %d out of range", layer);
        return -EINVAL;
    }

    endpoint_layers[ep_idx] = layer;

    int ret = save_settings();
    if (ret < 0) {
        LOG_ERR("Failed to save default layer setting: %d", ret);
    }

    LOG_INF("Set default layer %d for endpoint %d", layer, ep_idx);
    return apply_default_layer(ep_idx);
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_default_layer_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_default_layer_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_default_layer_api);
