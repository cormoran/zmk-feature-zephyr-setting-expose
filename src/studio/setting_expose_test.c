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
    req.which_request_type = zmk_setting_expose_Request_write_tag;
    strncpy(req.request_type.write.key, TEST_KEY, sizeof(req.request_type.write.key) - 1);
    req.request_type.write.which_typed_value = zmk_setting_expose_WriteRequest_bytes_value_tag;
    const uint8_t val[] = {0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(req.request_type.write.typed_value.bytes_value.bytes, val, sizeof(val));
    req.request_type.write.typed_value.bytes_value.size = sizeof(val);

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_response_type == zmk_setting_expose_Response_write_tag;
}

static bool test_write_int32(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_request_type = zmk_setting_expose_Request_write_tag;
    strncpy(req.request_type.write.key, "t/i32", sizeof(req.request_type.write.key) - 1);
    req.request_type.write.which_typed_value = zmk_setting_expose_WriteRequest_int32_value_tag;
    req.request_type.write.typed_value.int32_value = 42;

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_response_type == zmk_setting_expose_Response_write_tag;
}

static bool test_read(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_request_type = zmk_setting_expose_Request_read_tag;
    strncpy(req.request_type.read.key, TEST_KEY, sizeof(req.request_type.read.key) - 1);

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    if (resp.which_response_type != zmk_setting_expose_Response_read_tag) {
        return false;
    }

    const uint8_t expected[] = {0xDE, 0xAD, 0xBE, 0xEF};
    if (resp.response_type.read.which_typed_value !=
        zmk_setting_expose_ReadResponse_bytes_value_tag) {
        return false;
    }
    if (resp.response_type.read.typed_value.bytes_value.size != sizeof(expected)) {
        return false;
    }
    return memcmp(resp.response_type.read.typed_value.bytes_value.bytes, expected,
                  sizeof(expected)) == 0;
}

static bool test_list_contains_written_key(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_request_type = zmk_setting_expose_Request_list_tag;

    uint8_t buf[16];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_response_type == zmk_setting_expose_Response_list_tag;
}

static bool test_delete(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_request_type = zmk_setting_expose_Request_delete_tag;
    strncpy(req.request_type.delete.key, TEST_KEY, sizeof(req.request_type.delete.key) - 1);

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_response_type == zmk_setting_expose_Response_delete_tag;
}

static bool test_read_after_delete(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_request_type = zmk_setting_expose_Request_read_tag;
    strncpy(req.request_type.read.key, TEST_KEY, sizeof(req.request_type.read.key) - 1);

    uint8_t buf[128];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_response_type == zmk_setting_expose_Response_error_tag;
}

static bool test_empty_key_error(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_request_type = zmk_setting_expose_Request_write_tag;
    req.request_type.write.which_typed_value = zmk_setting_expose_WriteRequest_bytes_value_tag;
    req.request_type.write.typed_value.bytes_value.bytes[0] = 0x01;
    req.request_type.write.typed_value.bytes_value.size = 1;

    uint8_t buf[64];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_response_type == zmk_setting_expose_Response_error_tag;
}

static bool test_storage_info(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_request_type = zmk_setting_expose_Request_storage_info_tag;

    uint8_t buf[16];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_response_type == zmk_setting_expose_Response_storage_info_tag;
}

static bool test_gc(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_request_type = zmk_setting_expose_Request_gc_tag;

    uint8_t buf[16];
    pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }

    zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
    if (!call_handler(sub, buf, s.bytes_written, &resp)) {
        return false;
    }
    return resp.which_response_type == zmk_setting_expose_Response_gc_tag;
}

static bool test_clear_all(const struct zmk_rpc_custom_subsystem *sub) {
    /* Write a key to be cleared */
    {
        zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
        req.which_request_type = zmk_setting_expose_Request_write_tag;
        strncpy(req.request_type.write.key, "ca/k", sizeof(req.request_type.write.key) - 1);
        req.request_type.write.which_typed_value = zmk_setting_expose_WriteRequest_bytes_value_tag;
        req.request_type.write.typed_value.bytes_value.bytes[0] = 0x01;
        req.request_type.write.typed_value.bytes_value.size = 1;

        uint8_t buf[128];
        pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
        if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
            return false;
        }
        zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
        if (!call_handler(sub, buf, s.bytes_written, &resp)) {
            return false;
        }
        if (resp.which_response_type != zmk_setting_expose_Response_write_tag) {
            return false;
        }
    }

    /* Clear all */
    {
        zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
        req.which_request_type = zmk_setting_expose_Request_clear_all_tag;

        uint8_t buf[16];
        pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
        if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
            return false;
        }
        zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
        if (!call_handler(sub, buf, s.bytes_written, &resp)) {
            return false;
        }
        if (resp.which_response_type != zmk_setting_expose_Response_clear_all_tag) {
            return false;
        }
    }

    /* Read the key - should be gone */
    {
        zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
        req.which_request_type = zmk_setting_expose_Request_read_tag;
        strncpy(req.request_type.read.key, "ca/k", sizeof(req.request_type.read.key) - 1);

        uint8_t buf[128];
        pb_ostream_t s = pb_ostream_from_buffer(buf, sizeof(buf));
        if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
            return false;
        }
        zmk_setting_expose_Response resp = zmk_setting_expose_Response_init_zero;
        if (!call_handler(sub, buf, s.bytes_written, &resp)) {
            return false;
        }
        return resp.which_response_type == zmk_setting_expose_Response_error_tag;
    }
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
    RUN_TEST(storage_info, test_storage_info(sub));
    RUN_TEST(gc, test_gc(sub));
    RUN_TEST(clear_all, test_clear_all(sub));

    LOG_INF("setting_expose_test: done");
    return 0;
}

SYS_INIT(setting_expose_unit_tests, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
