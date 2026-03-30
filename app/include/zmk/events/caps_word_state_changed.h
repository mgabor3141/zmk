/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct zmk_caps_word_state_changed {
    bool state;
};

ZMK_EVENT_DECLARE(zmk_caps_word_state_changed);

static inline int raise_caps_word_state_changed(bool state) {
    return raise_zmk_caps_word_state_changed(
        (struct zmk_caps_word_state_changed){.state = state});
}
