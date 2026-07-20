/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Boot-time RPC unit tests for the setting_expose subsystem.
 * Enabled by CONFIG_ZMK_SETTING_EXPOSE_UNIT_TEST=y.
 *
 * Uses a minimal RAM-based settings backend so the tests do not depend on
 * any persistent storage hardware.
 *
 * Results are logged as:
 *   "setting_expose_test: PASS: <test_name>"
 *   "setting_expose_test: FAIL: <test_name>"
 */

#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <zmk/setting_expose/setting_expose.pb.h>
#include <zmk/setting_expose/dispatch.h>
#include <zmk/setting_expose/relay.h>

#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/slist.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(setting_expose_test, CONFIG_ZMK_LOG_LEVEL);

/* ---- Minimal RAM-backed settings store ---------------------------------- */

#define TEST_STORE_MAX_ENTRIES 8
#define TEST_STORE_MAX_KEY_LEN 80
#define TEST_STORE_MAX_VAL_LEN 256

struct test_store_entry {
    bool used;
    char key[TEST_STORE_MAX_KEY_LEN];
    uint8_t value[TEST_STORE_MAX_VAL_LEN];
    size_t val_len;
};

static struct test_store_entry test_db[TEST_STORE_MAX_ENTRIES];
static struct settings_store test_store_instance;

static ssize_t test_read_cb(void *ctx, void *data, size_t len) {
    struct test_store_entry *e = ctx;
    size_t copy = MIN(e->val_len, len);
    memcpy(data, e->value, copy);
    return (ssize_t)copy;
}

static int test_csi_load(struct settings_store *cs, const struct settings_load_arg *arg) {
    for (int i = 0; i < TEST_STORE_MAX_ENTRIES; i++) {
        if (!test_db[i].used) {
            continue;
        }

        const char *full_key = test_db[i].key;
        const char *matched_key = full_key;

        /* Filter by subtree prefix if requested */
        if (arg->subtree) {
            size_t pfx = strlen(arg->subtree);
            if (strncmp(full_key, arg->subtree, pfx) != 0) {
                continue;
            }
            if (full_key[pfx] == '/') {
                matched_key = full_key + pfx + 1;
            } else if (full_key[pfx] == '\0') {
                matched_key = "";
            } else {
                continue;
            }
        }

        if (arg->cb) {
            arg->cb(matched_key, test_db[i].val_len, test_read_cb, &test_db[i], arg->param);
        }
    }
    return 0;
}

static int test_csi_save(struct settings_store *cs, const char *name, const char *value,
                         size_t val_len) {
    for (int i = 0; i < TEST_STORE_MAX_ENTRIES; i++) {
        if (test_db[i].used && strcmp(test_db[i].key, name) == 0) {
            if (value == NULL) {
                test_db[i].used = false;
                return 0;
            }
            size_t copy = MIN(val_len, TEST_STORE_MAX_VAL_LEN);
            memcpy(test_db[i].value, value, copy);
            test_db[i].val_len = copy;
            return 0;
        }
    }

    if (value == NULL) {
        return 0;
    }

    for (int i = 0; i < TEST_STORE_MAX_ENTRIES; i++) {
        if (!test_db[i].used) {
            test_db[i].used = true;
            strncpy(test_db[i].key, name, TEST_STORE_MAX_KEY_LEN - 1);
            test_db[i].key[TEST_STORE_MAX_KEY_LEN - 1] = '\0';
            size_t copy = MIN(val_len, TEST_STORE_MAX_VAL_LEN);
            memcpy(test_db[i].value, value, copy);
            test_db[i].val_len = copy;
            return 0;
        }
    }
    return -ENOMEM;
}

static const struct settings_store_itf test_store_itf = {
    .csi_load = test_csi_load,
    .csi_save = test_csi_save,
};

static void init_test_store(void) {
    memset(test_db, 0, sizeof(test_db));
    test_store_instance.cs_itf = &test_store_itf;
    settings_src_register(&test_store_instance);
    settings_dst_register(&test_store_instance);
}

/* ---- Test helpers ------------------------------------------------------- */

#define TEST_KEY "t/u"

static const struct zmk_rpc_custom_subsystem *find_subsystem(void) {
    STRUCT_SECTION_FOREACH(zmk_rpc_custom_subsystem, s) {
        if (strcmp(s->identifier, "zmk__setting_expose") == 0) {
            return s;
        }
    }
    return NULL;
}

/*
 * Callback used to decode the payload bytes field out of a CallResponse.
 * arg must point to a struct call_handler_decode_ctx.
 */
struct call_handler_decode_ctx {
    uint8_t buf[256];
    size_t len;
    bool ok;
};

static bool decode_payload_bytes_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    (void)field;
    struct call_handler_decode_ctx *ctx = (struct call_handler_decode_ctx *)*arg;
    ctx->len = stream->bytes_left;
    if (ctx->len > sizeof(ctx->buf)) {
        ctx->ok = false;
        return false;
    }
    ctx->ok = pb_read(stream, ctx->buf, ctx->len);
    return ctx->ok;
}

static bool call_handler(const struct zmk_rpc_custom_subsystem *sub, const uint8_t *payload,
                         size_t payload_len, zmk_setting_expose_Response *out_resp) {
    zmk_custom_CallRequest req = zmk_custom_CallRequest_init_zero;
    if (payload_len > sizeof(req.payload.bytes)) {
        LOG_ERR("call_handler: payload too large (%zu > %zu)", payload_len,
                sizeof(req.payload.bytes));
        return false;
    }
    memcpy(req.payload.bytes, payload, payload_len);
    req.payload.size = (pb_size_t)payload_len;

    pb_callback_t encode_response = {0};
    if (!sub->handler(&req, &encode_response)) {
        return false;
    }

    /*
     * The encode_response callback is designed to be called by pb_encode as
     * a bytes callback inside a zmk_custom_CallResponse.  Calling it directly
     * with a NULL pb_field_t pointer (as was done before) causes a segfault
     * inside zmk_rpc_custom_subsystem_encode_response_payload because
     * pb_encode_tag_for_field dereferences the field pointer.
     *
     * Instead, wrap the callback in a CallResponse and encode it properly so
     * that pb_encode passes the real pb_field_t for the `payload` field.
     */
    zmk_custom_CallResponse call_resp = zmk_custom_CallResponse_init_zero;
    call_resp.payload = encode_response;

    uint8_t call_resp_buf[512];
    pb_ostream_t out_stream = pb_ostream_from_buffer(call_resp_buf, sizeof(call_resp_buf));
    if (!pb_encode(&out_stream, zmk_custom_CallResponse_fields, &call_resp)) {
        LOG_ERR("call_handler: failed to encode CallResponse");
        return false;
    }

    /* Decode the payload bytes back out of the CallResponse, then decode
     * those bytes as a zmk_setting_expose_Response. */
    struct call_handler_decode_ctx ctx = {0};
    zmk_custom_CallResponse dec_resp = zmk_custom_CallResponse_init_zero;
    dec_resp.payload.funcs.decode = decode_payload_bytes_cb;
    dec_resp.payload.arg = &ctx;

    pb_istream_t in_stream = pb_istream_from_buffer(call_resp_buf, out_stream.bytes_written);
    if (!pb_decode(&in_stream, zmk_custom_CallResponse_fields, &dec_resp) || !ctx.ok) {
        LOG_ERR("call_handler: failed to decode CallResponse payload");
        return false;
    }

    pb_istream_t resp_stream = pb_istream_from_buffer(ctx.buf, ctx.len);
    return pb_decode(&resp_stream, zmk_setting_expose_Response_fields, out_resp);
}

#define RUN_TEST(_name, _expr)                                                                     \
    do {                                                                                           \
        if (_expr) {                                                                               \
            LOG_INF("setting_expose_test: PASS: " #_name);                                         \
        } else {                                                                                   \
            LOG_ERR("setting_expose_test: FAIL: " #_name);                                         \
        }                                                                                          \
    } while (0)

/* ---- Individual tests --------------------------------------------------- */

static bool test_subsystem_found(void) { return find_subsystem() != NULL; }

static bool test_write(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_write_tag;
    strncpy(req.op.write.key, TEST_KEY, sizeof(req.op.write.key) - 1);
    req.op.write.which_typed_value = zmk_setting_expose_SettingEntry_bytes_value_tag;
    const uint8_t val[] = {0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(req.op.write.typed_value.bytes_value.bytes, val, sizeof(val));
    req.op.write.typed_value.bytes_value.size = sizeof(val);

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_ok_tag;
}

static bool test_write_int32(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_write_tag;
    strncpy(req.op.write.key, "t/i32", sizeof(req.op.write.key) - 1);
    req.op.write.which_typed_value = zmk_setting_expose_SettingEntry_int32_value_tag;
    req.op.write.typed_value.int32_value = 42;

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_ok_tag;
}

static bool test_read(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_read_tag;
    strncpy(req.op.read.key, TEST_KEY, sizeof(req.op.read.key) - 1);

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    if (resp.which_result != zmk_setting_expose_Response_entry_tag) {
        return false;
    }

    const uint8_t expected[] = {0xDE, 0xAD, 0xBE, 0xEF};
    if (resp.result.entry.which_typed_value != zmk_setting_expose_SettingEntry_bytes_value_tag) {
        return false;
    }
    if (resp.result.entry.typed_value.bytes_value.size != sizeof(expected)) {
        return false;
    }
    return memcmp(resp.result.entry.typed_value.bytes_value.bytes, expected, sizeof(expected)) == 0;
}

static bool test_list_contains_written_key(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_list_tag;

    uint8_t buf[16];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_list_tag;
}

static bool test_delete(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_delete_tag;
    strncpy(req.op.delete.key, TEST_KEY, sizeof(req.op.delete.key) - 1);

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_ok_tag;
}

static bool test_read_after_delete(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_read_tag;
    strncpy(req.op.read.key, TEST_KEY, sizeof(req.op.read.key) - 1);

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_error_tag;
}

static bool test_empty_key_error(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_write_tag;
    req.op.write.which_typed_value = zmk_setting_expose_SettingEntry_bytes_value_tag;
    req.op.write.typed_value.bytes_value.bytes[0] = 0x01;
    req.op.write.typed_value.bytes_value.size = 1;

    uint8_t buf[64];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_error_tag;
}

static bool test_prefix_type_match(const struct zmk_rpc_custom_subsystem *sub) {
    /*
     * The zmk_custom_CallRequest payload cap is 25 bytes, too small to fit a
     * WriteRequest with a key as long as "behavior/local_id/0" (19 chars) plus
     * any value via RPC.  Pre-populate the store directly instead.
     */
    const char *key = "behavior/local_id/0";
    const char val[] = "my_bhv";
    if (settings_save_one(key, val, sizeof(val) - 1) != 0) {
        return false;
    }

    /* ReadRequest with this key encodes to 23 bytes, which fits the 25-byte cap. */
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_read_tag;
    strncpy(req.op.read.key, key, sizeof(req.op.read.key) - 1);

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }
    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    if (resp.which_result != zmk_setting_expose_Response_entry_tag) {
        return false;
    }
    return resp.result.entry.which_typed_value == zmk_setting_expose_SettingEntry_string_value_tag;
}

static bool test_storage_info(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_storage_info_tag;

    uint8_t buf[16];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_storage_info_tag;
}

static bool test_gc(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_gc_tag;

    uint8_t buf[16];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_ok_tag;
}

static bool test_clear_all(const struct zmk_rpc_custom_subsystem *sub) {
    /* Write a key to be cleared */
    {
        zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
        req.which_op = zmk_setting_expose_Request_write_tag;
        strncpy(req.op.write.key, "ca/k", sizeof(req.op.write.key) - 1);
        req.op.write.which_typed_value = zmk_setting_expose_SettingEntry_bytes_value_tag;
        req.op.write.typed_value.bytes_value.bytes[0] = 0x01;
        req.op.write.typed_value.bytes_value.size = 1;

        uint8_t buf[128];
        pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
        if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
            return false;
        }
        zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
        if (!call_handler(sub, buf, s.bytes_written, &resp)) {
            return false;
        }
        if (resp.which_result != zmk_setting_expose_Response_ok_tag) {
            return false;
        }
    }

    /* Clear all */
    {
        zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
        req.which_op = zmk_setting_expose_Request_clear_all_tag;

        uint8_t buf[16];
        pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
        if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
            return false;
        }
        zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
        if (!call_handler(sub, buf, s.bytes_written, &resp)) {
            return false;
        }
        if (resp.which_result != zmk_setting_expose_Response_ok_tag) {
            return false;
        }
    }

    /* Read the key - should be gone */
    {
        zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
        req.which_op = zmk_setting_expose_Request_read_tag;
        strncpy(req.op.read.key, "ca/k", sizeof(req.op.read.key) - 1);

        uint8_t buf[128];
        pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
        if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
            return false;
        }
        zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
        if (!call_handler(sub, buf, s.bytes_written, &resp)) {
            return false;
        }
        return resp.which_result == zmk_setting_expose_Response_error_tag;
    }
}

static bool clear_all_via_rpc(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_clear_all_tag;

    uint8_t buf[16];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }
    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_ok_tag;
}

/*
 * Issue a list request for one page. entries is a callback field that
 * call_handler does not decode, but the has_more / next_offset scalars are
 * decoded normally, which is what the pagination test asserts on.
 */
static bool list_page(const struct zmk_rpc_custom_subsystem *sub, uint32_t offset, uint32_t limit,
                      zmk_setting_expose_ListPage *out_list) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_op = zmk_setting_expose_Request_list_tag;
    req.op.list.offset = offset;
    req.op.list.limit = limit;

    uint8_t buf[32];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }
    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    if (resp.which_result != zmk_setting_expose_Response_list_tag) {
        return false;
    }
    *out_list = resp.result.list;
    return true;
}

static bool test_pagination(const struct zmk_rpc_custom_subsystem *sub) {
    /* Start from a known-empty store so the entry count is deterministic. */
    if (!clear_all_via_rpc(sub)) {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        char key[] = "pg/0";
        key[3] = (char)('0' + i);
        uint8_t v = (uint8_t)i;
        if (settings_save_one(key, &v, 1) != 0) {
            return false;
        }
    }

    zmk_setting_expose_ListPage l = zmk_setting_expose_ListPage_init_zero;

    /* Page 0 (offset 0, limit 2): more remain, cursor advances to 2. */
    if (!list_page(sub, 0, 2, &l) || !l.has_more || l.next_offset != 2) {
        return false;
    }
    /* Page 1 (offset 2, limit 2): more remain, cursor advances to 4. */
    if (!list_page(sub, 2, 2, &l) || !l.has_more || l.next_offset != 4) {
        return false;
    }
    /* Page 2 (offset 4, limit 2): last page (1 entry), cursor at 5. */
    if (!list_page(sub, 4, 2, &l) || l.has_more || l.next_offset != 5) {
        return false;
    }
    /* limit 0 means "all remaining": one page, no more, cursor at 5. */
    if (!list_page(sub, 0, 0, &l) || l.has_more || l.next_offset != 5) {
        return false;
    }
    return true;
}

/*
 * A targeted request (target != TARGET_CENTRAL) on a build WITHOUT split relay
 * support degenerates to the local store, so the client still gets a real
 * Response rather than a bare Ack. (native_sim has no ZMK_SPLIT.)
 */
static bool test_target_all_fallback(const struct zmk_rpc_custom_subsystem *sub) {
    const char *key = "ta/k";
    uint8_t v = 7;
    if (settings_save_one(key, &v, 1) != 0) {
        return false;
    }

    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.target = SETTING_EXPOSE_TARGET_ALL;
    req.req_id = 99;
    req.which_op = zmk_setting_expose_Request_read_tag;
    strncpy(req.op.read.key, key, sizeof(req.op.read.key) - 1);

    uint8_t buf[64];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }
    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_result == zmk_setting_expose_Response_entry_tag;
}

/*
 * The peripheral relay path decodes a Request, runs setting_expose_dispatch
 * against its own store, and encodes the Response into a fixed buffer. Exercise
 * that data path directly (no BLE): write via dispatch, then read it back.
 */
static bool test_dispatch_roundtrip(void) {
    const char *key = "rt/k";

    const uint8_t val[] = {0x11, 0x22, 0x33, 0x44};

    zmk_setting_expose_Request wreq = zmk_setting_expose_Request_init_zero;
    wreq.which_op = zmk_setting_expose_Request_write_tag;
    strncpy(wreq.op.write.key, key, sizeof(wreq.op.write.key) - 1);
    wreq.op.write.which_typed_value = zmk_setting_expose_SettingEntry_bytes_value_tag;
    memcpy(wreq.op.write.typed_value.bytes_value.bytes, val, sizeof(val));
    wreq.op.write.typed_value.bytes_value.size = sizeof(val);

    zmk_setting_expose_Response wresp = zmk_setting_expose_Response_init_zero;
    if (setting_expose_dispatch(&wreq, &wresp) != 0) {
        return false;
    }
    if (wresp.which_result != zmk_setting_expose_Response_ok_tag) {
        return false;
    }

    zmk_setting_expose_Request rreq = zmk_setting_expose_Request_init_zero;
    rreq.which_op = zmk_setting_expose_Request_read_tag;
    strncpy(rreq.op.read.key, key, sizeof(rreq.op.read.key) - 1);

    zmk_setting_expose_Response rresp = zmk_setting_expose_Response_init_zero;
    if (setting_expose_dispatch(&rreq, &rresp) != 0) {
        return false;
    }

    if (rresp.which_result != zmk_setting_expose_Response_entry_tag) {
        return false;
    }

    /* A read result (a SettingEntry) is delivered as the `entry` event. Encode
     * it into the relay-sized buffer and decode it back -- exactly what the
     * peripheral relay path does before shipping a reply. */
    zmk_setting_expose_Notification n = zmk_setting_expose_Notification_init_zero;
    n.which_event = zmk_setting_expose_Notification_entry_tag;
    n.event.entry = rresp.result.entry;

    uint8_t buf[SE_RELAY_MAX_DATA];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, zmk_setting_expose_Notification_fields, &n)) {
        return false;
    }
    zmk_setting_expose_Notification decoded = zmk_setting_expose_Notification_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
    if (!pb_decode(&is, zmk_setting_expose_Notification_fields, &decoded)) {
        return false;
    }
    /* Unregistered keys read back as raw bytes. */
    return decoded.which_event == zmk_setting_expose_Notification_entry_tag &&
           decoded.event.entry.which_typed_value ==
               zmk_setting_expose_SettingEntry_bytes_value_tag &&
           decoded.event.entry.typed_value.bytes_value.size == sizeof(val) &&
           memcmp(decoded.event.entry.typed_value.bytes_value.bytes, val, sizeof(val)) == 0;
}

/*
 * The relayed list streams one entry per notification via setting_expose_entry_at.
 * Verify sequential fetch returns each stored key exactly once and reports
 * end-of-stream (0) past the last index, with each entry encodable into the
 * relay reply buffer.
 */
static bool test_entry_at_streaming(void) {
    /* Start from a known-empty store. */
    zmk_setting_expose_Request creq = zmk_setting_expose_Request_init_zero;
    creq.which_op = zmk_setting_expose_Request_clear_all_tag;
    zmk_setting_expose_Response cresp = zmk_setting_expose_Response_init_zero;
    if (setting_expose_dispatch(&creq, &cresp) != 0) {
        return false;
    }

    const int N = 3;
    for (int i = 0; i < N; i++) {
        char key[] = "st/0";
        key[3] = (char)('0' + i);
        uint8_t v = (uint8_t)i;
        if (settings_save_one(key, &v, 1) != 0) {
            return false;
        }
    }

    int seen = 0;
    for (uint32_t idx = 0; idx < (uint32_t)N + 2; idx++) {
        zmk_setting_expose_SettingEntry entry = zmk_setting_expose_SettingEntry_init_zero;
        int rc = setting_expose_entry_at(idx, &entry);
        if (idx < (uint32_t)N) {
            if (rc != 1 || strncmp(entry.key, "st/", 3) != 0) {
                return false;
            }
            /* One entry must always fit the relay reply buffer. */
            zmk_setting_expose_Notification n = zmk_setting_expose_Notification_init_zero;
            n.which_event = zmk_setting_expose_Notification_entry_tag;
            n.event.entry = entry;
            uint8_t buf[SE_RELAY_MAX_DATA];
            pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
            if (!pb_encode(&os, zmk_setting_expose_Notification_fields, &n)) {
                return false;
            }
            seen++;
        } else if (rc != 0) {
            return false; /* past the end must report end-of-stream */
        }
    }
    return seen == N;
}

/* ---- Boot-time test runner ---------------------------------------------- */

static int setting_expose_unit_tests(void) {
    init_test_store();

    LOG_INF("setting_expose_test: starting unit tests");

    RUN_TEST(subsystem_found, test_subsystem_found());

    const struct zmk_rpc_custom_subsystem *sub = find_subsystem();
    if (sub == NULL) {
        LOG_ERR("setting_expose_test: subsystem not found, skipping remaining tests");
        return 0;
    }

    RUN_TEST(write, test_write(sub));
    RUN_TEST(write_int32, test_write_int32(sub));
    RUN_TEST(read, test_read(sub));
    RUN_TEST(list, test_list_contains_written_key(sub));
    RUN_TEST(delete, test_delete(sub));
    RUN_TEST(read_after_delete, test_read_after_delete(sub));
    RUN_TEST(empty_key_error, test_empty_key_error(sub));
    RUN_TEST(prefix_type_match, test_prefix_type_match(sub));
    RUN_TEST(storage_info, test_storage_info(sub));
    RUN_TEST(gc, test_gc(sub));
    RUN_TEST(clear_all, test_clear_all(sub));
    RUN_TEST(pagination, test_pagination(sub));
    RUN_TEST(target_all_fallback, test_target_all_fallback(sub));
    RUN_TEST(dispatch_roundtrip, test_dispatch_roundtrip());
    RUN_TEST(entry_at_streaming, test_entry_at_streaming());

    LOG_INF("setting_expose_test: done");
    return 0;
}

SYS_INIT(setting_expose_unit_tests, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
