/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * @brief Shared setting_expose request dispatch.
 *
 * The per-operation logic (list/read/write/delete/storage_info/gc/clear_all)
 * lives here so it can run in two places against the SAME local Zephyr settings
 * store:
 *   - the central's Studio RPC handler (src/studio/setting_expose_handler.c),
 *   - the split peripheral's relay answer path (src/split/setting_expose_relay.c),
 *     which decodes a relayed Request, runs it against the peripheral's own
 *     store, and relays the encoded Response back.
 *
 * This file is compiled on BOTH split roles (the peripheral image has no
 * ZMK_STUDIO), so it must not depend on the Studio RPC subsystem.
 */

#pragma once

#include <zmk/setting_expose/setting_expose.pb.h>

/* Magic Request.target value meaning "every half" (mirrors proto Target). */
#define SETTING_EXPOSE_TARGET_CENTRAL 0u
#define SETTING_EXPOSE_TARGET_ALL 0xFFFFu

/**
 * Run a decoded setting_expose Request against the LOCAL settings store and
 * populate @p resp. Does not touch Request.target / req_id -- targeting and
 * async delivery are the caller's concern.
 *
 * Used for the central's synchronous path and for the peripheral's answer to a
 * non-`list` relayed request. A relayed `list` streams one entry at a time via
 * setting_expose_entry_at() instead (see the split relay), so it does not go
 * through here.
 *
 * @param req   decoded request (one of the request_type oneof).
 * @param resp  response to fill (response_type oneof).
 * @return 0 on success, negative errno on failure (caller renders ErrorResponse).
 */
int setting_expose_dispatch(const zmk_setting_expose_Request *req,
                            zmk_setting_expose_Response *resp);

/**
 * Fetch the @p index-th setting from the LOCAL store (0-based, iteration order)
 * into @p out with its typed value. This backs the memory-frugal relayed
 * `list`, which streams one SettingEntry per notification rather than buffering
 * a page.
 *
 * @return 1 if an entry exists at @p index (and @p out is filled), 0 if @p index
 *         is past the last entry, negative errno on error.
 */
int setting_expose_entry_at(uint32_t index, zmk_setting_expose_SettingEntry *out);
