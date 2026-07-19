/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

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
 * CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN (asserted by the relay macros). Setting
 * values are up to 256 bytes, so one encoded Notification (one SettingEntry, or
 * a read Response) can be ~360 bytes; the data buffers are sized to hold that
 * plus framing, and the reassembled relay payload ceiling is raised to match
 * (see Kconfig). Both halves run the same little-endian CPU, so the encoded
 * protobuf is portable as-is.
 */

/* Encoded inner Request (largest: a Write with a 256-byte value + framing). */
#define SE_RELAY_QUERY_DATA_MAX 400
/* Encoded inner Notification event (largest: one SettingEntry / a Read + framing). */
#define SE_RELAY_REPLY_DATA_MAX 400

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
 * Both halves must build with a relay payload ceiling large enough for these
 * carriers. The module sets CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=512 as a
 * Kconfig default, but a `default` can lose parse-order to ZMK's own default,
 * so set it explicitly in your config if this assert fires. (ZMK's own
 * __ZMK_RELAY_ASSERT_SIZE also guards this; this one just explains the fix.)
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
BUILD_ASSERT(sizeof(struct se_relay_reply) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "setting_expose relay replies need CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=512 "
             "on both split halves");
BUILD_ASSERT(sizeof(struct se_relay_query) <= CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "setting_expose relay queries need CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=512 "
             "on both split halves");
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
