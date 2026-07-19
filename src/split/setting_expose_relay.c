/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Split event-relay glue for setting_expose. Compiled into BOTH split roles;
 * the relay direction macros are self-role-gating. See DESIGN.md.
 *
 *   central  --SEq (request)-->  peripheral   (CENTRAL_TO_PERIPHERAL + HANDLE)
 *   peripheral --SEr (reply)-->  central      (PERIPHERAL_TO_CENTRAL + HANDLE)
 *
 * The peripheral answers a relayed non-`list` request with the shared
 * setting_expose_dispatch against its OWN store; a `list` is streamed one
 * SettingEntry per reply (plus a final list_done) so it never buffers a page.
 * Each reply carries an encoded type-safe `Notification` event. The central
 * stamps `source` on each reply and forwards it to the connected Studio client.
 * A TARGET_ALL delete/clear_all is special: the central deletes the peripherals
 * first (broadcast) and its OWN store last, only after every peripheral has
 * replied or a timeout elapses, then emits a Notification with `complete`.
 *
 * All heavy work runs on ZMK's shared low-priority work queue, ONE notification
 * (or one streamed entry) per work cycle: the Studio BLE GATT transport drains
 * its TX ring buffer on that same queue, so emitting a burst without yielding
 * would self-deadlock. Re-submitting the work item between items yields the
 * queue so the transport can drain. All relay state is confined to this
 * single-threaded queue, so it needs no locking.
 */

#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>
#include <zmk/workqueue.h>
#include <zmk/setting_expose/setting_expose.pb.h>
#include <zmk/setting_expose/dispatch.h>
#include <zmk/setting_expose/relay.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SETTING_EXPOSE)
#include <zmk/studio/custom.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ZMK_EVENT_IMPL for both carriers lives in setting_expose_relay_events.c. */

/*
 * Wire both carriers in both directions + both HANDLE macros. The direction
 * macros are self-role-gating and the HANDLE macros only fire on a matching
 * identifier, so listing all four is safe on either role.
 */
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(se_relay_query, SEq, source)
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(se_relay_reply, SEr, source)
ZMK_RELAY_EVENT_HANDLE(se_relay_query, SEq, source)
ZMK_RELAY_EVENT_HANDLE(se_relay_reply, SEr, source)

/* Encode one Notification event into a relay reply and ship it to the central. */
static void se_relay_send_reply(uint8_t req_id, const zmk_setting_expose_Notification *n) {
    struct se_relay_reply reply = {.source = ZMK_RELAY_EVENT_SOURCE_SELF, .req_id = req_id};
    pb_ostream_t os = pb_ostream_from_buffer(reply.data, sizeof(reply.data));
    if (!pb_encode(&os, zmk_setting_expose_Notification_fields, n)) {
        LOG_ERR("Failed to encode relay notification: %s", PB_GET_ERROR(&os));
        return;
    }
    reply.len = (uint16_t)os.bytes_written;
    raise_se_relay_reply(reply);
}

/* ---- Peripheral side: answer relayed requests --------------------------- */

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

K_MSGQ_DEFINE(se_relay_query_msgq, sizeof(struct se_relay_query), 4, 4);

static zmk_setting_expose_Request answer_req;
static zmk_setting_expose_Response answer_resp;
static zmk_setting_expose_Notification answer_notif;
static struct se_relay_query answer_query;

/*
 * State for an in-progress streamed `list`. Streaming one entry per work cycle
 * keeps peripheral RAM tiny (no page buffer) and paces the relay link. Only one
 * stream at a time -- the central serializes requests.
 */
static struct {
    bool active;
    uint8_t req_id;
    uint32_t offset;
} pstream;

static void se_relay_stream_work_handler(struct k_work *work);
static K_WORK_DEFINE(se_relay_stream_work, se_relay_stream_work_handler);

static void se_relay_stream_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!pstream.active) {
        return;
    }

    answer_notif = (zmk_setting_expose_Notification)zmk_setting_expose_Notification_init_zero;
    answer_notif.req_id = pstream.req_id;

    int rc = setting_expose_entry_at(pstream.offset, &answer_notif.event.entry);
    if (rc == 1) {
        answer_notif.which_event = zmk_setting_expose_Notification_entry_tag;
        se_relay_send_reply(pstream.req_id, &answer_notif);
        pstream.offset++;
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &se_relay_stream_work);
    } else {
        /* Past the last entry (or read error): terminate the stream. */
        answer_notif.which_event = zmk_setting_expose_Notification_list_done_tag;
        se_relay_send_reply(pstream.req_id, &answer_notif);
        pstream.active = false;
    }
}

static void se_relay_answer_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    while (k_msgq_get(&se_relay_query_msgq, &answer_query, K_NO_WAIT) == 0) {
        answer_req = (zmk_setting_expose_Request)zmk_setting_expose_Request_init_zero;
        pb_istream_t is = pb_istream_from_buffer(answer_query.data, answer_query.len);
        if (!pb_decode(&is, zmk_setting_expose_Request_fields, &answer_req)) {
            LOG_WRN("Failed to decode relayed setting_expose request: %s", PB_GET_ERROR(&is));
            continue;
        }

        if (answer_req.which_request_type == zmk_setting_expose_Request_list_tag) {
            /* Stream the whole store one entry per cycle. */
            pstream.active = true;
            pstream.req_id = answer_query.req_id;
            pstream.offset = 0;
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &se_relay_stream_work);
            continue;
        }

        /* Everything else is a single Response wrapped in one Notification. */
        answer_resp = (zmk_setting_expose_Response)zmk_setting_expose_Response_init_zero;
        int rc = setting_expose_dispatch(&answer_req, &answer_resp);
        if (rc != 0) {
            answer_resp = (zmk_setting_expose_Response)zmk_setting_expose_Response_init_zero;
            answer_resp.which_response_type = zmk_setting_expose_Response_error_tag;
            snprintf(answer_resp.response_type.error.message,
                     sizeof(answer_resp.response_type.error.message), "Error: %d", rc);
        }

        answer_notif = (zmk_setting_expose_Notification)zmk_setting_expose_Notification_init_zero;
        answer_notif.req_id = answer_query.req_id;
        answer_notif.which_event = zmk_setting_expose_Notification_response_tag;
        answer_notif.event.response = answer_resp;
        se_relay_send_reply(answer_query.req_id, &answer_notif);
    }
}

static K_WORK_DEFINE(se_relay_answer_work, se_relay_answer_work_handler);

static int se_relay_on_query(const zmk_event_t *eh) {
    const struct se_relay_query *query = as_se_relay_query(eh);
    if (query == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (k_msgq_put(&se_relay_query_msgq, query, K_NO_WAIT) != 0) {
        LOG_WRN("setting_expose relay query queue full, dropping query");
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &se_relay_answer_work);
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(se_relay_answer, se_relay_on_query);
ZMK_SUBSCRIPTION(se_relay_answer, se_relay_query);

#else // CONFIG_ZMK_SPLIT_ROLE_CENTRAL

/* ---- Central side: forward replies as notifications --------------------- */

#if IS_ENABLED(CONFIG_ZMK_SETTING_EXPOSE)

#define SETTING_EXPOSE_SUBSYSTEM_IDENTIFIER "zmk__setting_expose"

static int se_relay_subsystem_index(void) {
    size_t count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &count);
    for (size_t i = 0; i < count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, SETTING_EXPOSE_SUBSYSTEM_IDENTIFIER) == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

/*
 * Pending targeted request. Written by the RPC thread in
 * setting_expose_relay_dispatch before the query is broadcast; read on the
 * relay work queue when replies arrive. `target` filters which peripheral
 * replies are forwarded (TARGET_ALL forwards all; a specific index forwards
 * only the matching source). `req_id` (client-chosen, low byte) is echoed by
 * peripherals so stale replies from a previous request are dropped.
 */
static struct {
    uint8_t req_id;
    uint32_t target;
} pending;

/*
 * Active TARGET_ALL delete/clear_all transaction. All fields are touched ONLY
 * on the relay work queue (the reply notify work and the timeout both run
 * there, single-threaded), except the ones the RPC thread sets once before
 * raising the query (happens-before the first reply). See DESIGN.md.
 */
static struct {
    bool active;
    uint8_t req_id;
    uint32_t received;
    uint32_t expected;
    zmk_setting_expose_Request central_req; /* delete/clear_all to run on the central last */
} del_tx;

static zmk_setting_expose_Notification notify_event;

static bool se_relay_encode_notification(pb_ostream_t *stream, const pb_field_t *field,
                                         void *const *arg) {
    const zmk_setting_expose_Notification *event = (const zmk_setting_expose_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, zmk_setting_expose_Notification_fields, event);
}

/* Emit the current notify_event to the connected Studio client. */
static void se_relay_emit(void) {
    int index = se_relay_subsystem_index();
    if (index < 0) {
        LOG_WRN("setting_expose subsystem not registered, dropping notification");
        return;
    }
    raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload =
            {
                .funcs.encode = se_relay_encode_notification,
                .arg = (void *)&notify_event,
            },
    });
}

static struct k_work_delayable se_relay_delete_timeout;

/*
 * Finish an active delete-all transaction: delete the central's OWN store last,
 * then emit the completion notification. Idempotent (clears `active` first) and
 * only ever called on the relay work queue.
 */
static void se_relay_finish_delete(void) {
    if (!del_tx.active) {
        return;
    }
    del_tx.active = false;
    k_work_cancel_delayable(&se_relay_delete_timeout);

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    int rc = setting_expose_dispatch(&del_tx.central_req, &resp);
    if (rc != 0) {
        LOG_WRN("central delete during delete-all failed: %d", rc);
    }

    LOG_DBG("delete-all complete (req_id=%u, %u peripheral replies)", del_tx.req_id,
            del_tx.received);
    notify_event = (zmk_setting_expose_Notification)zmk_setting_expose_Notification_init_zero;
    notify_event.source = 0;
    notify_event.req_id = del_tx.req_id;
    notify_event.which_event = zmk_setting_expose_Notification_complete_tag;
    se_relay_emit();
}

static void se_relay_delete_timeout_work(struct k_work *work) {
    ARG_UNUSED(work);
    if (del_tx.active) {
        LOG_DBG("delete-all timed out waiting for peripherals (got %u/%u)", del_tx.received,
                del_tx.expected);
        se_relay_finish_delete();
    }
}

/* ---- Reply notify path (low-priority work queue, one per cycle) ---- */

K_MSGQ_DEFINE(se_relay_reply_msgq, sizeof(struct se_relay_reply), 8, 4);
static struct se_relay_reply notify_reply;

static void se_relay_notify_work_handler(struct k_work *work);
static K_WORK_DEFINE(se_relay_notify_work, se_relay_notify_work_handler);

static void se_relay_notify_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (k_msgq_get(&se_relay_reply_msgq, &notify_reply, K_NO_WAIT) != 0) {
        return;
    }

    /* Decode the peripheral's Notification, stamp the real source, forward it. */
    notify_event = (zmk_setting_expose_Notification)zmk_setting_expose_Notification_init_zero;
    pb_istream_t is = pb_istream_from_buffer(notify_reply.data, notify_reply.len);
    if (pb_decode(&is, zmk_setting_expose_Notification_fields, &notify_event)) {
        notify_event.source = notify_reply.source;
        notify_event.req_id = notify_reply.req_id;
        se_relay_emit();
    } else {
        LOG_WRN("Failed to decode relayed notification: %s", PB_GET_ERROR(&is));
    }

    /* Count a delete-all reply and finish once every peripheral has answered.
     * A delete/clear_all yields exactly one reply per peripheral (a Response),
     * never a list stream, so counting replies is correct. */
    if (del_tx.active && notify_reply.req_id == del_tx.req_id) {
        del_tx.received++;
        if (del_tx.received >= del_tx.expected) {
            se_relay_finish_delete();
        }
    }

    /* One item per cycle: yield to let the BLE transport drain, then continue. */
    if (k_msgq_num_used_get(&se_relay_reply_msgq) > 0) {
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &se_relay_notify_work);
    }
}

static int se_relay_on_reply(const zmk_event_t *eh) {
    const struct se_relay_reply *reply = as_se_relay_reply(eh);
    if (reply == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    /* Drop stale replies and replies for a different specific target. */
    if (reply->req_id != pending.req_id) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (pending.target != SETTING_EXPOSE_TARGET_ALL && pending.target != reply->source) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (k_msgq_put(&se_relay_reply_msgq, reply, K_NO_WAIT) != 0) {
        LOG_WRN("setting_expose relay reply queue full, dropping reply");
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &se_relay_notify_work);
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(se_relay_notify, se_relay_on_reply);
ZMK_SUBSCRIPTION(se_relay_notify, se_relay_reply);

/* ---- Central entry point (RPC thread) ---- */

static bool request_is_delete(const zmk_setting_expose_Request *req) {
    return req->which_request_type == zmk_setting_expose_Request_delete_tag ||
           req->which_request_type == zmk_setting_expose_Request_clear_all_tag;
}

int setting_expose_relay_dispatch(const zmk_setting_expose_Request *req,
                                  zmk_setting_expose_Response *resp) {
    uint8_t req_id = (uint8_t)req->req_id;

    /* Remember the target so replies can be filtered as they arrive. */
    pending.req_id = req_id;
    pending.target = req->target;

    /* Broadcast the request (the peripheral ignores target). */
    struct se_relay_query query = {.source = ZMK_RELAY_EVENT_SOURCE_SELF, .req_id = req_id};
    pb_ostream_t os = pb_ostream_from_buffer(query.data, sizeof(query.data));
    if (!pb_encode(&os, zmk_setting_expose_Request_fields, req)) {
        LOG_WRN("Failed to encode relay query: %s", PB_GET_ERROR(&os));
        return -EMSGSIZE;
    }
    query.len = (uint16_t)os.bytes_written;

    bool delete_all = (req->target == SETTING_EXPOSE_TARGET_ALL) && request_is_delete(req);

    if (delete_all) {
        /*
         * Ordered delete: tell the peripherals to delete first; defer the
         * central's own delete until every peripheral has replied or the
         * timeout fires (see se_relay_finish_delete). Last-writer-wins if a new
         * delete-all starts before the previous finishes -- the web serializes
         * these behind its in-progress state.
         */
        del_tx.active = true;
        del_tx.req_id = req_id;
        del_tx.received = 0;
        del_tx.expected = ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT;
        del_tx.central_req = *req;

        raise_se_relay_query(query);

        k_work_schedule_for_queue(
            zmk_workqueue_lowprio_work_q(), &se_relay_delete_timeout,
            K_MSEC(del_tx.expected == 0 ? 0 : CONFIG_ZMK_SETTING_EXPOSE_DELETE_ALL_TIMEOUT_MS));
    } else {
        raise_se_relay_query(query);
    }

    resp->which_response_type = zmk_setting_expose_Response_ack_tag;
    return 0;
}

static int se_relay_central_init(void) {
    k_work_init_delayable(&se_relay_delete_timeout, se_relay_delete_timeout_work);
    return 0;
}

SYS_INIT(se_relay_central_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif // CONFIG_ZMK_SETTING_EXPOSE

#endif // CONFIG_ZMK_SPLIT_ROLE_CENTRAL
