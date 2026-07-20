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
 * an encoded inner Request; `se_relay_reply` travels peripheral -> central
 * (identifier "SEr") and carries an encoded inner Notification. The peripheral
 * answers a non-`list` request with the SAME setting_expose_dispatch the central
 * uses; a `list` is streamed one SettingEntry per reply (plus a final
 * list_done). The central stamps `source` on each reply and forwards it to the
 * connected Studio client. See src/split/setting_expose_relay.c.
 *
 * These carriers use ZMK's serialize/deserialize relay macros
 * (ZMK_RELAY_EVENT_*_SERIALIZE / ZMK_RELAY_EVENT_HANDLE_DESERIALIZE, cormoran/zmk
 * PR #36): the serialize callback writes ONLY `data[0..len]` (the encoded
 * protobuf) to the wire and returns its length, so each relay event is only as
 * big as the message -- a ~8-byte list request is an ~8-byte event, not a padded
 * buffer. `data[]` and `len` are therefore just a RAM staging buffer (not sent
 * whole); `source` is carried by the relay layer itself (loop guard on send,
 * stamped to the peripheral index+1 on receive) and is not serialized. `req_id`
 * is not carried here either -- it lives inside the encoded Request/Notification.
 *
 * The wire event_data_size is a uint8, so one message is capped at 255 bytes and
 * at CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN (the serialize max_size + these RAM
 * buffers). A value that would exceed it streams as a `too_large` SettingEntry
 * marker instead. Both halves are little-endian, so the encoded protobuf is
 * portable as-is.
 */

/* RAM staging capacity for one message; also the serialize max_size (= DATA_LEN).
 * Falls back to the Kconfig default when the relay is not built (native_sim). */
#if defined(CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN)
#define SE_RELAY_MAX_DATA CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN
#else
#define SE_RELAY_MAX_DATA 192
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
BUILD_ASSERT(SE_RELAY_MAX_DATA <= 255,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN must be <= 255 (relay event_data_size is a "
             "uint8); keep it modest so a chunked event fits the BLE TX buffers too");
#endif

struct se_relay_query {
    uint8_t source; /* relay loop guard / receive stamp (not serialized) */
    uint16_t len;   /* bytes of `data` in use (the encoded Request) */
    uint8_t data[SE_RELAY_MAX_DATA];
};

struct se_relay_reply {
    uint8_t source; /* stamped to the peripheral index+1 on receive */
    uint16_t len;   /* bytes of `data` in use (the encoded Notification) */
    uint8_t data[SE_RELAY_MAX_DATA];
};

ZMK_EVENT_DECLARE(se_relay_query);
ZMK_EVENT_DECLARE(se_relay_reply);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/*
 * Central entry point for a targeted (target != TARGET_CENTRAL) request, called
 * from the Studio RPC handler. Broadcasts the encoded Request to the
 * peripheral(s) and sets @p resp to an AckResponse. Per-half results arrive
 * asynchronously as Notifications; the central's OWN store is read via the
 * synchronous path (target 0). Returns 0 on success, negative errno otherwise.
 */
int setting_expose_relay_dispatch(const zmk_setting_expose_Request *req,
                                  zmk_setting_expose_Response *resp);
#endif
