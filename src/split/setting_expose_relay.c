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
 * The peripheral answers a relayed Request with the shared
 * setting_expose_dispatch against its OWN store and relays the encoded Response
 * back. The central forwards each reply to the connected Studio client as a
 * Notification, filtered by the pending request's target. A TARGET_ALL
 * delete/clear_all is special: the central deletes the peripherals first
 * (broadcast) and its OWN store last, only after every peripheral has replied
 * or a timeout elapses, then emits a Notification with complete=true.
 */

#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>
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

/*
 * Dedicated work queue for the heavy half of the relay. The carriers are
 * re-raised locally by ZMK_RELAY_EVENT_HANDLE on the SYSTEM work queue, whose
 * ~2 KB spare stack is too small for the peripheral's decode+dispatch+encode
 * and -- worse -- the central's raise_zmk_studio_custom_notification (which
 * builds a full notification + response and double pb_encodes synchronously).
 * kscan-diagnostics observed a sysworkq stack overflow doing this inline, so we
 * hand the work to our own thread. All state below that is not touched by the
 * RPC thread is confined to this single-threaded queue, so it needs no locking.
 */
static K_THREAD_STACK_DEFINE(se_relay_stack, CONFIG_ZMK_SETTING_EXPOSE_RELAY_STACK_SIZE);
static struct k_work_q se_relay_workq;

static int se_relay_workq_init(void) {
    struct k_work_queue_config cfg = {.name = "se_relay"};
    k_work_queue_start(&se_relay_workq, se_relay_stack, K_THREAD_STACK_SIZEOF(se_relay_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, &cfg);
    return 0;
}

SYS_INIT(se_relay_workq_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ---- Peripheral side: answer relayed requests --------------------------- */

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

K_MSGQ_DEFINE(se_relay_query_msgq, sizeof(struct se_relay_query), 4, 4);

static zmk_setting_expose_Request answer_req;
static zmk_setting_expose_Response answer_resp;
static struct se_relay_reply answer_reply;
static struct se_relay_query answer_query;

static void se_relay_answer_work(struct k_work *work) {
    while (k_msgq_get(&se_relay_query_msgq, &answer_query, K_NO_WAIT) == 0) {
        answer_req = (zmk_setting_expose_Request)zmk_setting_expose_Request_init_zero;
        pb_istream_t is = pb_istream_from_buffer(answer_query.data, answer_query.len);
        if (!pb_decode(&is, zmk_setting_expose_Request_fields, &answer_req)) {
            LOG_WRN("Failed to decode relayed setting_expose request: %s", PB_GET_ERROR(&is));
            continue;
        }

        answer_resp = (zmk_setting_expose_Response)zmk_setting_expose_Response_init_zero;
        int rc = setting_expose_dispatch(&answer_req, &answer_resp, SE_RELAY_LIST_BUDGET);
        if (rc != 0) {
            /* Report the failure to the client as an ErrorResponse. */
            answer_resp = (zmk_setting_expose_Response)zmk_setting_expose_Response_init_zero;
            answer_resp.which_response_type = zmk_setting_expose_Response_error_tag;
            snprintf(answer_resp.response_type.error.message,
                     sizeof(answer_resp.response_type.error.message), "Error: %d", rc);
        }

        answer_reply = (struct se_relay_reply){
            .source = ZMK_RELAY_EVENT_SOURCE_SELF,
            .req_id = answer_query.req_id,
        };
        pb_ostream_t os = pb_ostream_from_buffer(answer_reply.data, sizeof(answer_reply.data));
        if (!pb_encode(&os, zmk_setting_expose_Response_fields, &answer_resp)) {
            LOG_ERR("Failed to encode relayed setting_expose reply: %s", PB_GET_ERROR(&os));
            continue;
        }
        answer_reply.len = (uint16_t)os.bytes_written;

        raise_se_relay_reply(answer_reply);
    }
}

static K_WORK_DEFINE(se_relay_answer_work_item, se_relay_answer_work);

static int se_relay_on_query(const zmk_event_t *eh) {
    const struct se_relay_query *query = as_se_relay_query(eh);
    if (query == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (k_msgq_put(&se_relay_query_msgq, query, K_NO_WAIT) != 0) {
        LOG_WRN("setting_expose relay query queue full, dropping query");
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_work_submit_to_queue(&se_relay_workq, &se_relay_answer_work_item);
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
 * there, single-threaded), except `active`/`req_id`/`expected`/`op` which the
 * RPC thread sets once before raising the query (happens-before the first
 * reply). See DESIGN.md for the ordering guarantee.
 */
static struct {
    bool active;
    uint8_t req_id;
    uint32_t received;
    uint32_t expected;
    zmk_setting_expose_Request central_req; /* delete/clear_all to run on the central last */
} del_tx;

static bool se_relay_encode_notification(pb_ostream_t *stream, const pb_field_t *field,
                                         void *const *arg) {
    const zmk_setting_expose_Notification *event = (const zmk_setting_expose_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, zmk_setting_expose_Notification_fields, event);
}

/* Raise one Notification. Runs on the relay work queue (see stack note). */
static zmk_setting_expose_Notification notify_event;

static void se_relay_emit(uint8_t source, uint8_t req_id, const uint8_t *payload,
                          size_t payload_len, bool complete) {
    int index = se_relay_subsystem_index();
    if (index < 0) {
        LOG_WRN("setting_expose subsystem not registered, dropping notification");
        return;
    }

    notify_event = (zmk_setting_expose_Notification)zmk_setting_expose_Notification_init_zero;
    notify_event.source = source;
    notify_event.req_id = req_id;
    notify_event.complete = complete;
    notify_event.payload.size = (pb_size_t)MIN(payload_len, sizeof(notify_event.payload.bytes));
    if (payload != NULL && notify_event.payload.size > 0) {
        memcpy(notify_event.payload.bytes, payload, notify_event.payload.size);
    }

    /* encode_payload runs inline within raise_zmk_studio_custom_notification,
     * so pointing at the (static) notify_event is safe. */
    raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload =
            {
                .funcs.encode = se_relay_encode_notification,
                .arg = (void *)&notify_event,
            },
    });
}

/*
 * Finish an active delete-all transaction: delete the central's OWN store last,
 * then emit the completion notification. Idempotent (clears `active` first) and
 * only ever called on the relay work queue.
 */
static struct k_work_delayable se_relay_delete_timeout;
static zmk_setting_expose_Response se_central_delete_resp;

static void se_relay_finish_delete(void) {
    if (!del_tx.active) {
        return;
    }
    del_tx.active = false;
    k_work_cancel_delayable(&se_relay_delete_timeout);

    se_central_delete_resp = (zmk_setting_expose_Response)zmk_setting_expose_Response_init_zero;
    int rc = setting_expose_dispatch(&del_tx.central_req, &se_central_delete_resp, 0);
    if (rc != 0) {
        se_central_delete_resp = (zmk_setting_expose_Response)zmk_setting_expose_Response_init_zero;
        se_central_delete_resp.which_response_type = zmk_setting_expose_Response_error_tag;
        snprintf(se_central_delete_resp.response_type.error.message,
                 sizeof(se_central_delete_resp.response_type.error.message), "Error: %d", rc);
    }

    static uint8_t buf[SE_RELAY_REPLY_DATA_MAX];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    size_t len = 0;
    if (pb_encode(&os, zmk_setting_expose_Response_fields, &se_central_delete_resp)) {
        len = os.bytes_written;
    } else {
        LOG_ERR("Failed to encode central delete response: %s", PB_GET_ERROR(&os));
    }

    LOG_DBG("delete-all complete (req_id=%u, %u peripheral replies)", del_tx.req_id,
            del_tx.received);
    se_relay_emit(0, del_tx.req_id, buf, len, true);
}

static void se_relay_delete_timeout_work(struct k_work *work) {
    ARG_UNUSED(work);
    if (del_tx.active) {
        LOG_DBG("delete-all timed out waiting for peripherals (got %u/%u)", del_tx.received,
                del_tx.expected);
        se_relay_finish_delete();
    }
}

/* ---- Reply notify path (relay work queue) ---- */

K_MSGQ_DEFINE(se_relay_reply_msgq, sizeof(struct se_relay_reply), 8, 4);
static struct se_relay_reply notify_reply;

static void se_relay_notify_work(struct k_work *work) {
    ARG_UNUSED(work);
    while (k_msgq_get(&se_relay_reply_msgq, &notify_reply, K_NO_WAIT) == 0) {
        /* Forward the peripheral's answer to the client. */
        se_relay_emit(notify_reply.source, notify_reply.req_id, notify_reply.data, notify_reply.len,
                      false);

        /* Count it toward an active delete-all and finish once all are in. */
        if (del_tx.active && notify_reply.req_id == del_tx.req_id) {
            del_tx.received++;
            if (del_tx.received >= del_tx.expected) {
                se_relay_finish_delete();
            }
        }
    }
}

static K_WORK_DEFINE(se_relay_notify_work_item, se_relay_notify_work);

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
    k_work_submit_to_queue(&se_relay_workq, &se_relay_notify_work_item);
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(se_relay_notify, se_relay_on_reply);
ZMK_SUBSCRIPTION(se_relay_notify, se_relay_reply);

/* ---- Central-own contribution for TARGET_ALL (non-delete ops) ---- */

/*
 * Enqueue the central's OWN result as a source-0 reply so it flows through the
 * exact same notify path as the peripheral replies. Runs on the RPC thread.
 */
static zmk_setting_expose_Response se_central_resp;

static void se_relay_emit_central_own(const zmk_setting_expose_Request *req, uint8_t req_id) {
    se_central_resp = (zmk_setting_expose_Response)zmk_setting_expose_Response_init_zero;
    int rc = setting_expose_dispatch(req, &se_central_resp, SE_RELAY_LIST_BUDGET);
    if (rc != 0) {
        se_central_resp = (zmk_setting_expose_Response)zmk_setting_expose_Response_init_zero;
        se_central_resp.which_response_type = zmk_setting_expose_Response_error_tag;
        snprintf(se_central_resp.response_type.error.message,
                 sizeof(se_central_resp.response_type.error.message), "Error: %d", rc);
    }

    struct se_relay_reply own = {.source = 0, .req_id = req_id};
    pb_ostream_t os = pb_ostream_from_buffer(own.data, sizeof(own.data));
    if (!pb_encode(&os, zmk_setting_expose_Response_fields, &se_central_resp)) {
        LOG_ERR("Failed to encode central-own response: %s", PB_GET_ERROR(&os));
        return;
    }
    own.len = (uint16_t)os.bytes_written;

    if (k_msgq_put(&se_relay_reply_msgq, &own, K_NO_WAIT) != 0) {
        LOG_WRN("setting_expose relay reply queue full, dropping central-own result");
        return;
    }
    k_work_submit_to_queue(&se_relay_workq, &se_relay_notify_work_item);
}

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

    /* Broadcast the request (minus nothing -- the peripheral ignores target). */
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
            &se_relay_workq, &se_relay_delete_timeout,
            K_MSEC(del_tx.expected == 0 ? 0 : CONFIG_ZMK_SETTING_EXPOSE_DELETE_ALL_TIMEOUT_MS));
    } else {
        raise_se_relay_query(query);

        /* TARGET_ALL non-delete ops also report the central's own store. */
        if (req->target == SETTING_EXPOSE_TARGET_ALL) {
            se_relay_emit_central_own(req, req_id);
        }
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
