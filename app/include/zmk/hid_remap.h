/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/hid.h>

/**
 * Apply HID report remapping for the current endpoint's OS configuration.
 * Modifies the report body in-place (caller should pass a copy).
 */
void zmk_hid_remap_apply(struct zmk_hid_keyboard_report_body *body);

/**
 * Set the OS type for the current endpoint. Persists to flash.
 * 0 = Linux (no remap), 1 = macOS
 */
int zmk_hid_remap_set_os(uint8_t os_type);

/**
 * Get the OS type for the current endpoint.
 */
uint8_t zmk_hid_remap_get_os(void);
