/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Central-side Studio RPC entry point for setting_expose. Decodes the request,
 * routes by Request.target, and either dispatches locally (central) or hands
 * off to the split relay (peripheral / all). The per-operation logic lives in
 * src/setting_expose_dispatch.c so it is shared with the peripheral relay path.
 */

#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <zmk/setting_expose/setting_expose.pb.h>
#include <zmk/setting_expose/dispatch.h>

#if IS_ENABLED(CONFIG_ZMK_SETTING_EXPOSE_SPLIT)
#include <zmk/setting_expose/relay.h>
#endif

#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta setting_expose_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(
        "https://cormoran.github.io/zmk-feature-zephyr-setting-expose/"),
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zmk__setting_expose, &setting_expose_meta,
                         setting_expose_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zmk__setting_expose, zmk_setting_expose_Response);

static bool setting_expose_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                              pb_callback_t *encode_response) {
    zmk_setting_expose_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(zmk__setting_expose, encode_response);

    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, zmk_setting_expose_Request_fields, &req)) {
        LOG_WRN("Failed to decode setting_expose request: %s", PB_GET_ERROR(&req_stream));
        zmk_setting_expose_ErrorResponse err = zmk_setting_expose_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = zmk_setting_expose_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc;
    if (req.target == SETTING_EXPOSE_TARGET_CENTRAL) {
        /* Central-only: handle locally and answer synchronously (unchanged). */
        rc = setting_expose_dispatch(&req, resp);
    } else {
#if IS_ENABLED(CONFIG_ZMK_SETTING_EXPOSE_SPLIT)
        /*
         * Targeted at a peripheral / all halves: relay to the peripheral(s) and
         * acknowledge immediately. Per-half results (and, for a TARGET_ALL
         * delete/clear_all, the completion) arrive as Notifications.
         */
        rc = setting_expose_relay_dispatch(&req, resp);
#else
        /*
         * No split relay in this build: a targeted request degenerates to the
         * local store so a single-board web UI using TARGET_ALL still works.
         */
        rc = setting_expose_dispatch(&req, resp);
#endif
    }

    if (rc != 0) {
        zmk_setting_expose_ErrorResponse err = zmk_setting_expose_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Error: %d", rc);
        resp->which_response_type = zmk_setting_expose_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}
