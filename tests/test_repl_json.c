/* Validates the REPL JSON Lines output layer against hostile byte strings:
 * every produced line must parse as JSON and round-trip the input bytes. */
#include "repl_json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *parse_line(const cJSON *doc) {
    char *line = wlh_repl_json_to_line(doc);
    cJSON *parsed;
    assert(line != NULL);
    assert(strchr(line, '\n') == NULL);
    parsed = cJSON_Parse(line);
    assert(parsed != NULL);
    cJSON_free(line);
    return parsed;
}

static void expect_string(
    const cJSON *doc, const char *key, const char *expected
) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(doc, key);
    assert(cJSON_IsString(item));
    assert(strcmp(item->valuestring, expected) == 0);
}

static void test_begin_seeds_source_and_event(void) {
    cJSON *doc = wlh_repl_json_begin("unit");
    cJSON *parsed;
    assert(doc != NULL);
    parsed = parse_line(doc);
    expect_string(parsed, "source", "wlh-host-sim");
    expect_string(parsed, "event", "unit");
    cJSON_Delete(parsed);
    cJSON_Delete(doc);
}

static void test_plain_utf8_passthrough(void) {
    static const uint8_t ssid[] = "MyNet \xe6\xb5\x8b\xe8\xaf\x95";
    cJSON *doc = wlh_repl_json_begin("scan_result");
    cJSON *parsed;
    assert(doc != NULL);
    wlh_repl_json_add_bytes(doc, "ssid", ssid, sizeof(ssid) - 1u);
    parsed = parse_line(doc);
    expect_string(parsed, "ssid", (const char *)ssid);
    assert(cJSON_GetObjectItemCaseSensitive(parsed, "ssid_hex") == NULL);
    cJSON_Delete(parsed);
    cJSON_Delete(doc);
}

static void test_quotes_and_backslashes_are_escaped(void) {
    static const uint8_t ssid[] = "evil\"net\\path";
    cJSON *doc = wlh_repl_json_begin("scan_result");
    cJSON *parsed;
    assert(doc != NULL);
    wlh_repl_json_add_bytes(doc, "ssid", ssid, sizeof(ssid) - 1u);
    parsed = parse_line(doc);
    expect_string(parsed, "ssid", "evil\"net\\path");
    cJSON_Delete(parsed);
    cJSON_Delete(doc);
}

static void test_control_bytes_stay_valid_utf8(void) {
    static const uint8_t ssid[] = {'a', 0x01u, '\n', '\t', 'b'};
    cJSON *doc = wlh_repl_json_begin("scan_result");
    cJSON *parsed;
    const cJSON *item;
    assert(doc != NULL);
    wlh_repl_json_add_bytes(doc, "ssid", ssid, sizeof(ssid));
    parsed = parse_line(doc);
    item = cJSON_GetObjectItemCaseSensitive(parsed, "ssid");
    assert(cJSON_IsString(item));
    assert(strlen(item->valuestring) == sizeof(ssid));
    cJSON_Delete(parsed);
    cJSON_Delete(doc);
}

static void test_invalid_utf8_gets_hex_fallback(void) {
    static const uint8_t ssid[] = {'b', 'a', 'd', 0xffu, 0xfeu, '!'};
    cJSON *doc = wlh_repl_json_begin("scan_result");
    cJSON *parsed;
    assert(doc != NULL);
    wlh_repl_json_add_bytes(doc, "ssid", ssid, sizeof(ssid));
    parsed = parse_line(doc);
    expect_string(parsed, "ssid", "bad..!");
    expect_string(parsed, "ssid_hex", "626164fffe21");
    cJSON_Delete(parsed);
    cJSON_Delete(doc);
}

static void test_embedded_nul_gets_hex_fallback(void) {
    static const uint8_t ssid[] = {'a', 0x00u, 'b'};
    cJSON *doc = wlh_repl_json_begin("scan_result");
    cJSON *parsed;
    assert(doc != NULL);
    wlh_repl_json_add_bytes(doc, "ssid", ssid, sizeof(ssid));
    parsed = parse_line(doc);
    expect_string(parsed, "ssid", "a.b");
    expect_string(parsed, "ssid_hex", "610062");
    cJSON_Delete(parsed);
    cJSON_Delete(doc);
}

static void test_overlong_and_truncated_sequences(void) {
    /* Overlong encoding of '/', then a truncated 3-byte lead. */
    static const uint8_t overlong[] = {0xc0u, 0xafu};
    static const uint8_t truncated[] = {'x', 0xe2u, 0x82u};
    cJSON *doc = wlh_repl_json_begin("scan_result");
    cJSON *parsed;
    assert(doc != NULL);
    wlh_repl_json_add_bytes(doc, "a", overlong, sizeof(overlong));
    wlh_repl_json_add_bytes(doc, "b", truncated, sizeof(truncated));
    parsed = parse_line(doc);
    expect_string(parsed, "a_hex", "c0af");
    expect_string(parsed, "b_hex", "78e282");
    cJSON_Delete(parsed);
    cJSON_Delete(doc);
}

static void test_large_payload_hex(void) {
    uint8_t payload[512];
    cJSON *doc = wlh_repl_json_begin("blob");
    cJSON *parsed;
    const cJSON *item;
    for (size_t index = 0u; index < sizeof(payload); ++index)
        payload[index] = (uint8_t)(index * 7u + 0x80u);
    assert(doc != NULL);
    wlh_repl_json_add_hex(doc, "payload", payload, sizeof(payload));
    parsed = parse_line(doc);
    item = cJSON_GetObjectItemCaseSensitive(parsed, "payload");
    assert(cJSON_IsString(item));
    assert(strlen(item->valuestring) == sizeof(payload) * 2u);
    cJSON_Delete(parsed);
    cJSON_Delete(doc);
}

int main(void) {
    test_begin_seeds_source_and_event();
    test_plain_utf8_passthrough();
    test_quotes_and_backslashes_are_escaped();
    test_control_bytes_stay_valid_utf8();
    test_invalid_utf8_gets_hex_fallback();
    test_embedded_nul_gets_hex_fallback();
    test_overlong_and_truncated_sequences();
    test_large_payload_hex();
    printf("repl json tests passed\n");
    return 0;
}
