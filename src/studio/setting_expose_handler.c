#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <zmk/setting_expose/setting_expose.pb.h>
#include <zmk/setting_expose.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/iterable_sections.h>
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

/* ---- Helpers ------------------------------------------------------------ */

/*
 * Sentinel entry placed in the iterable section to guarantee that the linker
 * section symbols (_zmk_setting_expose_entry_list_start / _end) are always
 * defined, even when no user entries are registered.  The key is NULL and is
 * skipped during iteration.
 */
STRUCT_SECTION_ITERABLE(zmk_setting_expose_entry, _zmk_setting_expose_sentinel) = {
    .key = NULL,
    .type = 0,
};

static zmk_setting_expose_SettingType find_type_for_key(const char *key) {
    STRUCT_SECTION_FOREACH(zmk_setting_expose_entry, entry) {
        if (entry->key == NULL) {
            continue; /* skip sentinel */
        }
        if (strcmp(entry->key, key) == 0) {
            return (zmk_setting_expose_SettingType)entry->type;
        }
    }
    return zmk_setting_expose_SettingType_BYTES;
}

/* ---- List handler ------------------------------------------------------- */

struct list_encode_ctx {
    pb_ostream_t *stream;
    const pb_field_t *field;
    int error;
};

static int settings_list_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                            void *param) {
    struct list_encode_ctx *ctx = (struct list_encode_ctx *)param;

    if (ctx->error) {
        return -ctx->error;
    }

    zmk_setting_expose_SettingEntry entry = zmk_setting_expose_SettingEntry_init_zero;

    strncpy(entry.key, key, sizeof(entry.key) - 1);

    ssize_t read_len = read_cb(cb_arg, entry.value.bytes, sizeof(entry.value.bytes));
    if (read_len < 0) {
        LOG_WRN("Failed to read value for key %s: %d", key, (int)read_len);
        entry.value.size = 0;
    } else {
        entry.value.size = (pb_size_t)read_len;
    }

    entry.type = find_type_for_key(key);

    if (!pb_encode_tag_for_field(ctx->stream, ctx->field) ||
        !pb_encode_submessage(ctx->stream, zmk_setting_expose_SettingEntry_fields, &entry)) {
        LOG_WRN("Failed to encode setting entry for key %s", key);
        ctx->error = EIO;
        return -EIO;
    }

    return 0;
}

static bool encode_list_entries(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct list_encode_ctx ctx = {
        .stream = stream,
        .field = field,
        .error = 0,
    };
    settings_load_subtree_direct(NULL, settings_list_cb, &ctx);
    return ctx.error == 0;
}

static int handle_list_request(const zmk_setting_expose_ListRequest *req,
                               zmk_setting_expose_Response *resp) {
    (void)req;
    resp->which_response_type = zmk_setting_expose_Response_list_tag;
    resp->response_type.list.entries.funcs.encode = encode_list_entries;
    resp->response_type.list.entries.arg = NULL;
    return 0;
}

/* ---- Read handler ------------------------------------------------------- */

struct read_cb_ctx {
    const char *key;
    zmk_setting_expose_ReadResponse *out;
    bool found;
};

static int settings_read_direct_cb(const char *key, size_t len, settings_read_cb read_cb,
                                   void *cb_arg, void *param) {
    struct read_cb_ctx *ctx = (struct read_cb_ctx *)param;

    /* settings_load_subtree_direct strips the subtree prefix from key.
     * When loading with the full key as subtree, key should be empty string. */
    (void)key;

    ssize_t read_len = read_cb(cb_arg, ctx->out->value.bytes, sizeof(ctx->out->value.bytes));
    if (read_len < 0) {
        LOG_WRN("Failed to read value for key %s: %d", ctx->key, (int)read_len);
        ctx->out->value.size = 0;
    } else {
        ctx->out->value.size = (pb_size_t)read_len;
    }
    ctx->found = true;
    return 0;
}

static int handle_read_request(const zmk_setting_expose_ReadRequest *req,
                               zmk_setting_expose_Response *resp) {
    if (strlen(req->key) == 0) {
        LOG_WRN("Read request with empty key");
        return -EINVAL;
    }

    zmk_setting_expose_ReadResponse *out = &resp->response_type.read;
    strncpy(out->key, req->key, sizeof(out->key) - 1);
    out->type = find_type_for_key(req->key);

    struct read_cb_ctx ctx = {
        .key = req->key,
        .out = out,
        .found = false,
    };

    settings_load_subtree_direct(req->key, settings_read_direct_cb, &ctx);

    if (!ctx.found) {
        LOG_WRN("Setting not found: %s", req->key);
        return -ENOENT;
    }

    resp->which_response_type = zmk_setting_expose_Response_read_tag;
    return 0;
}

/* ---- Write handler ------------------------------------------------------ */

static int handle_write_request(const zmk_setting_expose_WriteRequest *req,
                                zmk_setting_expose_Response *resp) {
    if (strlen(req->key) == 0) {
        LOG_WRN("Write request with empty key");
        return -EINVAL;
    }

    int rc = settings_save_one(req->key, req->value.bytes, req->value.size);
    if (rc != 0) {
        LOG_WRN("Failed to save setting %s: %d", req->key, rc);
        return rc;
    }

    LOG_DBG("Saved setting: %s (%d bytes)", req->key, (int)req->value.size);
    resp->which_response_type = zmk_setting_expose_Response_write_tag;
    return 0;
}

/* ---- Delete handler ----------------------------------------------------- */

static int handle_delete_request(const zmk_setting_expose_DeleteRequest *req,
                                 zmk_setting_expose_Response *resp) {
    if (strlen(req->key) == 0) {
        LOG_WRN("Delete request with empty key");
        return -EINVAL;
    }

    int rc = settings_delete(req->key);
    if (rc != 0) {
        LOG_WRN("Failed to delete setting %s: %d", req->key, rc);
        return rc;
    }

    LOG_DBG("Deleted setting: %s", req->key);
    resp->which_response_type = zmk_setting_expose_Response_delete_tag;
    return 0;
}

/* ---- Top-level dispatcher ----------------------------------------------- */

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

    int rc = 0;
    switch (req.which_request_type) {
    case zmk_setting_expose_Request_list_tag:
        rc = handle_list_request(&req.request_type.list, resp);
        break;
    case zmk_setting_expose_Request_read_tag:
        rc = handle_read_request(&req.request_type.read, resp);
        break;
    case zmk_setting_expose_Request_write_tag:
        rc = handle_write_request(&req.request_type.write, resp);
        break;
    case zmk_setting_expose_Request_delete_tag:
        rc = handle_delete_request(&req.request_type.delete, resp);
        break;
    default:
        LOG_WRN("Unsupported setting_expose request type: %d", req.which_request_type);
        rc = -ENOTSUP;
    }

    if (rc != 0) {
        zmk_setting_expose_ErrorResponse err = zmk_setting_expose_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Error: %d", rc);
        resp->which_response_type = zmk_setting_expose_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}
