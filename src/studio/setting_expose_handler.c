#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <zmk/setting_expose/setting_expose.pb.h>
#include <zmk/setting_expose.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/iterable_sections.h>
#include <string.h>

#ifdef CONFIG_SETTINGS_NVS
#include <zephyr/fs/nvs.h>
#endif

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

static enum zmk_setting_type find_type_for_key(const char *key) {
    STRUCT_SECTION_FOREACH(zmk_setting_expose_entry, entry) {
        if (entry->key == NULL) {
            continue; /* skip sentinel */
        }
        if (strcmp(entry->key, key) == 0) {
            return entry->type;
        }
    }
    return ZMK_SETTING_TYPE_BYTES;
}

/*
 * Fill the typed_value oneof in a SettingEntry from raw bytes + type hint.
 */
static void fill_setting_entry_typed_value(zmk_setting_expose_SettingEntry *entry,
                                           const uint8_t *raw, size_t len,
                                           enum zmk_setting_type type) {
    switch (type) {
    case ZMK_SETTING_TYPE_INT32:
        if (len >= 4) {
            int32_t v;
            memcpy(&v, raw, 4);
            entry->which_typed_value = zmk_setting_expose_SettingEntry_int32_value_tag;
            entry->typed_value.int32_value = v;
            return;
        }
        break;
    case ZMK_SETTING_TYPE_BOOL:
        if (len >= 1) {
            entry->which_typed_value = zmk_setting_expose_SettingEntry_bool_value_tag;
            entry->typed_value.bool_value = raw[0] != 0;
            return;
        }
        break;
    case ZMK_SETTING_TYPE_STRING: {
        size_t copy = MIN(len, sizeof(entry->typed_value.string_value) - 1);
        entry->which_typed_value = zmk_setting_expose_SettingEntry_string_value_tag;
        memcpy(entry->typed_value.string_value, raw, copy);
        entry->typed_value.string_value[copy] = '\0';
        return;
    }
    default:
        break;
    }
    /* Default: raw bytes */
    size_t copy = MIN(len, sizeof(entry->typed_value.bytes_value.bytes));
    entry->which_typed_value = zmk_setting_expose_SettingEntry_bytes_value_tag;
    memcpy(entry->typed_value.bytes_value.bytes, raw, copy);
    entry->typed_value.bytes_value.size = (pb_size_t)copy;
}

/*
 * Fill the typed_value oneof in a ReadResponse from raw bytes + type hint.
 * Same logic as fill_setting_entry_typed_value but for ReadResponse struct.
 */
static void fill_read_response_typed_value(zmk_setting_expose_ReadResponse *out, const uint8_t *raw,
                                           size_t len, enum zmk_setting_type type) {
    switch (type) {
    case ZMK_SETTING_TYPE_INT32:
        if (len >= 4) {
            int32_t v;
            memcpy(&v, raw, 4);
            out->which_typed_value = zmk_setting_expose_ReadResponse_int32_value_tag;
            out->typed_value.int32_value = v;
            return;
        }
        break;
    case ZMK_SETTING_TYPE_BOOL:
        if (len >= 1) {
            out->which_typed_value = zmk_setting_expose_ReadResponse_bool_value_tag;
            out->typed_value.bool_value = raw[0] != 0;
            return;
        }
        break;
    case ZMK_SETTING_TYPE_STRING: {
        size_t copy = MIN(len, sizeof(out->typed_value.string_value) - 1);
        out->which_typed_value = zmk_setting_expose_ReadResponse_string_value_tag;
        memcpy(out->typed_value.string_value, raw, copy);
        out->typed_value.string_value[copy] = '\0';
        return;
    }
    default:
        break;
    }
    size_t copy = MIN(len, sizeof(out->typed_value.bytes_value.bytes));
    out->which_typed_value = zmk_setting_expose_ReadResponse_bytes_value_tag;
    memcpy(out->typed_value.bytes_value.bytes, raw, copy);
    out->typed_value.bytes_value.size = (pb_size_t)copy;
}

/*
 * Convert a WriteRequest typed_value oneof to raw bytes for settings_save_one.
 * Returns the number of bytes written to buf, or -1 on error.
 */
static int write_request_to_raw(const zmk_setting_expose_WriteRequest *req, uint8_t *buf,
                                size_t buf_size) {
    switch (req->which_typed_value) {
    case zmk_setting_expose_WriteRequest_int32_value_tag:
        if (buf_size < 4) {
            return -ENOBUFS;
        }
        memcpy(buf, &req->typed_value.int32_value, 4);
        return 4;
    case zmk_setting_expose_WriteRequest_bool_value_tag:
        if (buf_size < 1) {
            return -ENOBUFS;
        }
        buf[0] = req->typed_value.bool_value ? 1 : 0;
        return 1;
    case zmk_setting_expose_WriteRequest_string_value_tag: {
        size_t len = strnlen(req->typed_value.string_value, sizeof(req->typed_value.string_value));
        size_t copy = MIN(len, buf_size);
        memcpy(buf, req->typed_value.string_value, copy);
        return (int)copy;
    }
    default: /* bytes */
        if (buf_size < req->typed_value.bytes_value.size) {
            return -ENOBUFS;
        }
        memcpy(buf, req->typed_value.bytes_value.bytes, req->typed_value.bytes_value.size);
        return (int)req->typed_value.bytes_value.size;
    }
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

    uint8_t raw[256];
    ssize_t read_len = read_cb(cb_arg, raw, sizeof(raw));
    if (read_len < 0) {
        LOG_WRN("Failed to read value for key %s: %d", key, (int)read_len);
        read_len = 0;
    }

    enum zmk_setting_type type = find_type_for_key(key);
    fill_setting_entry_typed_value(&entry, raw, (size_t)read_len, type);

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
    enum zmk_setting_type type;
    bool found;
};

static int settings_read_direct_cb(const char *key, size_t len, settings_read_cb read_cb,
                                   void *cb_arg, void *param) {
    struct read_cb_ctx *ctx = (struct read_cb_ctx *)param;

    /* settings_load_subtree_direct strips the subtree prefix from key.
     * When loading with the full key as subtree, key should be empty string. */
    (void)key;

    uint8_t raw[256];
    ssize_t read_len = read_cb(cb_arg, raw, sizeof(raw));
    if (read_len < 0) {
        LOG_WRN("Failed to read value for key %s: %d", ctx->key, (int)read_len);
        read_len = 0;
    }

    fill_read_response_typed_value(ctx->out, raw, (size_t)read_len, ctx->type);
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

    struct read_cb_ctx ctx = {
        .key = req->key,
        .out = out,
        .type = find_type_for_key(req->key),
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

    uint8_t raw[256];
    int raw_len = write_request_to_raw(req, raw, sizeof(raw));
    if (raw_len < 0) {
        LOG_WRN("Failed to convert write request for key %s: %d", req->key, raw_len);
        return raw_len;
    }

    int rc = settings_save_one(req->key, raw, (size_t)raw_len);
    if (rc != 0) {
        LOG_WRN("Failed to save setting %s: %d", req->key, rc);
        return rc;
    }

    LOG_DBG("Saved setting: %s (%d bytes)", req->key, raw_len);
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

/* ---- Storage info handler ----------------------------------------------- */

static int handle_storage_info_request(const zmk_setting_expose_StorageInfoRequest *req,
                                       zmk_setting_expose_Response *resp) {
    (void)req;

    zmk_setting_expose_StorageInfoResponse *out = &resp->response_type.storage_info;

#ifdef CONFIG_SETTINGS_NVS
    void *storage = NULL;
    if (settings_storage_get(&storage) == 0 && storage != NULL) {
        struct nvs_fs *fs = (struct nvs_fs *)storage;
        uint32_t total = (uint32_t)fs->sector_size * (uint32_t)fs->sector_count;
        ssize_t free_sz = nvs_calc_free_space(fs);
        out->total_bytes = total;
        if (free_sz >= 0) {
            out->free_bytes = (uint32_t)free_sz;
            out->used_bytes = total > (uint32_t)free_sz ? total - (uint32_t)free_sz : 0;
        }
        /* NVS does not directly expose garbage size; leave as 0 */
    }
#endif

    resp->which_response_type = zmk_setting_expose_Response_storage_info_tag;
    return 0;
}

/* ---- GC handler --------------------------------------------------------- */

static int handle_gc_request(const zmk_setting_expose_GcRequest *req,
                             zmk_setting_expose_Response *resp) {
    (void)req;

#ifdef CONFIG_SETTINGS_NVS
    void *storage = NULL;
    if (settings_storage_get(&storage) == 0 && storage != NULL) {
        struct nvs_fs *fs = (struct nvs_fs *)storage;
        int rc = nvs_sector_use_next(fs);
        if (rc != 0) {
            LOG_DBG("nvs_sector_use_next returned %d (may be normal if sector not full)", rc);
        }
    }
#endif

    resp->which_response_type = zmk_setting_expose_Response_gc_tag;
    return 0;
}

/* ---- Clear-all handler -------------------------------------------------- */

#define CLEAR_ALL_BATCH 16
/*
 * Maximum number of individual keys to delete in one clear_all call.
 * This prevents an infinite loop if deletion does not reduce the count
 * (e.g. a backend that immediately re-populates deleted entries).
 */
#define CLEAR_ALL_MAX_ITERATIONS 500

static char _clear_all_keys[CLEAR_ALL_BATCH][80];
static int _clear_all_count;

static int collect_key_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                          void *param) {
    (void)len;
    (void)read_cb;
    (void)cb_arg;
    (void)param;
    if (_clear_all_count < CLEAR_ALL_BATCH) {
        strncpy(_clear_all_keys[_clear_all_count], key, sizeof(_clear_all_keys[0]) - 1);
        _clear_all_keys[_clear_all_count][sizeof(_clear_all_keys[0]) - 1] = '\0';
        _clear_all_count++;
    }
    return 0;
}

static int handle_clear_all_request(const zmk_setting_expose_ClearAllRequest *req,
                                    zmk_setting_expose_Response *resp) {
    (void)req;

    /* Iterate in batches to avoid unbounded stack usage. */
    int safety = CLEAR_ALL_MAX_ITERATIONS;
    do {
        _clear_all_count = 0;
        settings_load_subtree_direct(NULL, collect_key_cb, NULL);
        for (int i = 0; i < _clear_all_count && safety > 0; i++, safety--) {
            settings_delete(_clear_all_keys[i]);
        }
    } while (_clear_all_count > 0 && safety > 0);

    LOG_DBG("clear_all: finished");
    resp->which_response_type = zmk_setting_expose_Response_clear_all_tag;
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
    case zmk_setting_expose_Request_storage_info_tag:
        rc = handle_storage_info_request(&req.request_type.storage_info, resp);
        break;
    case zmk_setting_expose_Request_gc_tag:
        rc = handle_gc_request(&req.request_type.gc, resp);
        break;
    case zmk_setting_expose_Request_clear_all_tag:
        rc = handle_clear_all_request(&req.request_type.clear_all, resp);
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
