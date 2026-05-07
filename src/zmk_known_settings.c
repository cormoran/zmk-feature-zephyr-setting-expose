/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Type hints for well-known ZMK and Zephyr settings keys.
 *
 * Without these entries every setting defaults to BYTES display.  Registering
 * the common integer and string keys improves readability in the ZMK Studio
 * settings web UI.
 */

#include <zmk/setting_expose.h>

/* ---- BLE ---------------------------------------------------------------- */

/* Active BLE profile index (0-based uint8_t stored in one byte) */
ZMK_SETTING_EXPOSE_REGISTER(ble_active_profile, "ble/active_profile", ZMK_SETTING_TYPE_INT32);

/* ---- Endpoints ---------------------------------------------------------- */

/* Preferred output transport (enum, 1 byte) */
ZMK_SETTING_EXPOSE_REGISTER(endpoints_preferred_transport, "endpoints/preferred_transport",
                            ZMK_SETTING_TYPE_INT32);

/* ---- Physical layouts --------------------------------------------------- */

/* Selected physical layout index (uint8_t) */
ZMK_SETTING_EXPOSE_REGISTER(physical_layouts_selected, "physical_layouts/selected",
                            ZMK_SETTING_TYPE_INT32);

/* ---- Behavior local IDs (device name strings) --------------------------- */

ZMK_SETTING_EXPOSE_REGISTER_PREFIX(behavior_local_id, "behavior/local_id/",
                                   ZMK_SETTING_TYPE_STRING);
