/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * @brief ZMK Setting Expose public API
 *
 * Allows firmware code to annotate Zephyr settings keys with type information.
 * The type hints are used by the web UI to render appropriate editors
 * (e.g., number input for INT32, checkbox for BOOL, text box for STRING).
 *
 * By default all settings are exposed as raw bytes (BYTES type).
 *
 * Usage example:
 *   // Register a setting at key "mymod/volume" with INT32 type hint
 *   ZMK_SETTING_EXPOSE_REGISTER(mymod_volume, "mymod/volume", ZMK_SETTING_TYPE_INT32);
 *
 *   // Save the value as usual via Zephyr settings API
 *   int32_t vol = 75;
 *   settings_save_one("mymod/volume", &vol, sizeof(vol));
 */

#pragma once

#include <zephyr/sys/iterable_sections.h>

/**
 * Type hints for setting values, mirroring the SettingType enum in the proto.
 */
enum zmk_setting_type {
    ZMK_SETTING_TYPE_BYTES = 0,  /**< Raw bytes (default) */
    ZMK_SETTING_TYPE_INT32 = 1,  /**< 4-byte little-endian signed integer */
    ZMK_SETTING_TYPE_BOOL = 2,   /**< 1-byte boolean (0 = false, non-zero = true) */
    ZMK_SETTING_TYPE_STRING = 3, /**< UTF-8 string (may or may not be null-terminated) */
};

/**
 * Internal structure stored in an iterable section.
 * Use ZMK_SETTING_EXPOSE_REGISTER() / ZMK_SETTING_EXPOSE_REGISTER_PREFIX() instead of this
 * directly.
 */
struct zmk_setting_expose_entry {
    const char *key;
    enum zmk_setting_type type;
    bool is_prefix; /**< When true, key is matched as a prefix against setting keys. */
};

/**
 * Register a type hint for a Zephyr settings key (exact match).
 *
 * @param _name  C identifier used as the variable name (must be unique).
 * @param _key   Full Zephyr settings key string (e.g. "mymod/brightness").
 * @param _type  One of the zmk_setting_type enum values.
 */
#define ZMK_SETTING_EXPOSE_REGISTER(_name, _key, _type)                                            \
    STRUCT_SECTION_ITERABLE(zmk_setting_expose_entry, zmk_setting_expose_entry_##_name) = {        \
        .key = (_key),                                                                             \
        .type = (_type),                                                                           \
        .is_prefix = false,                                                                        \
    }

/**
 * Register a type hint for all Zephyr settings keys that start with a given prefix.
 *
 * Exact-match registrations (ZMK_SETTING_EXPOSE_REGISTER) take priority over prefix matches.
 *
 * @param _name    C identifier used as the variable name (must be unique).
 * @param _prefix  Key prefix string (e.g. "behavior/local_id/").  Any key that starts with this
 *                 string will be assigned _type.
 * @param _type    One of the zmk_setting_type enum values.
 */
#define ZMK_SETTING_EXPOSE_REGISTER_PREFIX(_name, _prefix, _type)                                  \
    STRUCT_SECTION_ITERABLE(zmk_setting_expose_entry, zmk_setting_expose_entry_##_name) = {        \
        .key = (_prefix),                                                                          \
        .type = (_type),                                                                           \
        .is_prefix = true,                                                                         \
    }
