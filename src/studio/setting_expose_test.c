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

    uint8_t resp_buf[512];
    pb_ostream_t out_stream = pb_ostream_from_buffer(resp_buf, sizeof(resp_buf));

    if (!encode_response.funcs.encode(&out_stream, NULL, &encode_response.arg)) {
        return false;
    }

    pb_istream_t in_stream = pb_istream_from_buffer(resp_buf, out_stream.bytes_written);
    return pb_decode(&in_stream, zmk_setting_expose_Response_fields, out_resp);
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
    const uint8_t val[] = {0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(req.request_type.write.value.bytes, val, sizeof(val));
    req.request_type.write.value.size = sizeof(val);

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
    if (resp.response_type.read.value.size != sizeof(expected)) {
        return false;
    }
    return memcmp(resp.response_type.read.value.bytes, expected, sizeof(expected)) == 0;
}

static bool test_list_contains_written_key(const struct zmk_rpc_custom_subsystem *sub) {
    zmk_custom_CallRequest rpc_req = zmk_custom_CallRequest_init_zero;

    zmk_setting_expose_Request req = zmk_setting_expose_Request_init_zero;
    req.which_request_type = zmk_setting_expose_Request_list_tag;

    pb_ostream_t s = pb_ostream_from_buffer(rpc_req.payload.bytes, sizeof(rpc_req.payload.bytes));
    if (!pb_encode(&s, zmk_setting_expose_Request_fields, &req)) {
        return false;
    }
    rpc_req.payload.size = (pb_size_t)s.bytes_written;

    pb_callback_t encode_response = {0};
    if (!sub->handler(&rpc_req, &encode_response)) {
        return false;
    }

    return encode_response.funcs.encode != NULL;
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
    req.request_type.write.value.bytes[0] = 0x01;
    req.request_type.write.value.size = 1;

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
    RUN_TEST(read, test_read(sub));
    RUN_TEST(list, test_list_contains_written_key(sub));
    RUN_TEST(delete, test_delete(sub));
    RUN_TEST(read_after_delete, test_read_after_delete(sub));
    RUN_TEST(empty_key_error, test_empty_key_error(sub));

    LOG_INF("setting_expose_test: done");
    return 0;
}

SYS_INIT(setting_expose_unit_tests, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
