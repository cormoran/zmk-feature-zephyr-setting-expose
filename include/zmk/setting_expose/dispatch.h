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
 * @param req              decoded request (one of the request_type oneof).
 * @param resp             response to fill (response_type oneof).
 * @param list_byte_budget for a `list` request, an upper bound in bytes on the
 *                         encoded entries of one page (the encoder stops early
 *                         and sets has_more once a further entry would exceed
 *                         it). 0 == unbounded (central Studio path, which
 *                         streams straight to the transport). The relay path
 *                         passes the reply buffer size so a page always fits.
 * @return 0 on success, negative errno on failure (caller renders ErrorResponse).
 */
int setting_expose_dispatch(const zmk_setting_expose_Request *req,
                            zmk_setting_expose_Response *resp, uint32_t list_byte_budget);
