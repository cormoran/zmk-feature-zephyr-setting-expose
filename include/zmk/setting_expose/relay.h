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
 * IMPORTANT sizing note: ZMK's relay transmits the ENTIRE carrier struct
 * (`sizeof(struct ...)`, memcpy'd whole) as one event -- it does NOT trim to the
 * `len` bytes actually used, and the wire `event_data_size` field is a **uint8
 * (max 255)**. So the carrier's fixed size IS the on-wire event size for every
 * relay, even a 6-byte `list` request. It must therefore be kept SMALL: large
 * enough for a typical key+value entry, but small enough to (a) stay well under
 * 255 and (b) not exhaust the BLE connection's TX buffers when the relay chunks
 * it across the split link (a ~240 B event was observed to fail with ENOMEM).
 * Hence DATA_LEN defaults to 128, not the transport's theoretical max. A value
 * that does not fit is streamed as a `too_large` marker instead (the central's
 * own synchronous path stream-encodes and is never bounded this way). Both
 * halves run the same little-endian CPU, so the encoded protobuf is portable.
 */

/* On-wire relay event size (= carrier struct size, since ZMK sends it whole).
 * Falls back to the module Kconfig default when the relay is not built (e.g. the
 * native_sim unit test just needs the constants). Keep <= 255 (uint8 limit). */
#if defined(CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN)
#define SE_RELAY_PAYLOAD_MAX CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN
#else
#define SE_RELAY_PAYLOAD_MAX 128
#endif

/* Fixed __packed carrier header: source(1) + req_id(1) + len(2). */
#define SE_RELAY_HEADER_BYTES 4

/* Data buffer = event size minus the carrier header. */
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
BUILD_ASSERT(offsetof(struct se_relay_reply, data) == SE_RELAY_HEADER_BYTES,
             "se_relay carrier header size drifted; update SE_RELAY_HEADER_BYTES");
/* The relay's wire event_data_size is a uint8, so the whole carrier must be
 * <= 255 bytes or the size field silently overflows. */
BUILD_ASSERT(sizeof(struct se_relay_reply) <= 255 && sizeof(struct se_relay_query) <= 255,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN too large: relay event_data_size is uint8 "
             "(<=255); keep it small (~128) to fit BLE TX buffers too");
/* Enough for a short key + small value; larger values relay as `too_large`. */
BUILD_ASSERT(SE_RELAY_REPLY_DATA_MAX >= 48,
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
