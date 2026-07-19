/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/setting_expose/setting_expose.pb.h>

/*
 * Split event-relay carriers for setting_expose.
 *
 * `se_relay_query` travels central -> peripheral (identifier "SEq") and carries
 * an opaque nanopb-encoded inner Request; `se_relay_reply` travels
 * peripheral -> central (identifier "SEr") and carries an encoded inner
 * Notification event. The peripheral answers a non-`list` request with the SAME
 * setting_expose_dispatch the central uses; a `list` is streamed one
 * SettingEntry per reply (plus a final list_done) so no per-page buffer is
 * needed. The central just stamps `source` on each reply and forwards it to the
 * connected Studio client. See src/split/setting_expose_relay.c.
 *
 * The whole struct is memcpy'd into the relay payload, so it must fit
 * CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN. The data buffers are therefore derived
 * from that ceiling (payload minus the fixed carrier header) rather than
 * hard-coded, so they automatically track whatever the config resolves to. If a
 * single Notification does not fit (e.g. a large setting value with a small
 * DATA_LEN), the peripheral streams an `entry_too_large` marker instead -- the
 * central's own synchronous path stream-encodes and is not bounded this way.
 * Both halves run the same little-endian CPU, so the encoded protobuf is
 * portable as-is.
 */

/* Relay payload ceiling. Falls back to the module Kconfig default when the
 * relay is not built (e.g. the native_sim unit test just needs the constants). */
#if defined(CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN)
#define SE_RELAY_PAYLOAD_MAX CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN
#else
#define SE_RELAY_PAYLOAD_MAX 512
#endif

/* Fixed __packed carrier header: source(1) + req_id(1) + len(2). */
#define SE_RELAY_HEADER_BYTES 4

/* Data buffer = payload ceiling minus the carrier header. */
#define SE_RELAY_QUERY_DATA_MAX (SE_RELAY_PAYLOAD_MAX - SE_RELAY_HEADER_BYTES)
#define SE_RELAY_REPLY_DATA_MAX (SE_RELAY_PAYLOAD_MAX - SE_RELAY_HEADER_BYTES)

struct se_relay_query {
    uint8_t source; /* ZMK_RELAY_EVENT_SOURCE_SELF on send; sender index+1 on receive */
    uint8_t req_id; /* echoed back in the reply for client correlation */
    uint16_t len;   /* bytes of `data` in use */
    uint8_t data[SE_RELAY_QUERY_DATA_MAX]; /* encoded inner Request */
} __packed;

struct se_relay_reply {
    uint8_t source;
    uint8_t req_id;
    uint16_t len;
    uint8_t data[SE_RELAY_REPLY_DATA_MAX]; /* encoded inner Notification event */
} __packed;

/*
 * The data buffers are derived from the payload ceiling, so the carriers fit by
 * construction. These asserts just catch a drifted header size and a DATA_LEN
 * too small to carry even a minimal keyed entry (below which every value would
 * relay as `entry_too_large`).
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
BUILD_ASSERT(offsetof(struct se_relay_reply, data) == SE_RELAY_HEADER_BYTES,
             "se_relay carrier header size drifted; update SE_RELAY_HEADER_BYTES");
BUILD_ASSERT(sizeof(struct se_relay_reply) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "se_relay carrier does not fit the relay payload ceiling");
BUILD_ASSERT(SE_RELAY_REPLY_DATA_MAX >= 128,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for setting_expose");
#endif

ZMK_EVENT_DECLARE(se_relay_query);
ZMK_EVENT_DECLARE(se_relay_reply);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/*
 * Central entry point for a targeted (target != TARGET_CENTRAL) request, called
 * from the Studio RPC handler. Broadcasts the request to the peripheral(s) and
 * sets @p resp to an AckResponse. Per-half results arrive asynchronously as
 * Notifications; the central's OWN store is read via the synchronous path
 * (target 0), so TARGET_ALL only relays to peripherals here. Returns 0 on
 * success, negative errno otherwise.
 */
int setting_expose_relay_dispatch(const zmk_setting_expose_Request *req,
                                  zmk_setting_expose_Response *resp);
#endif
