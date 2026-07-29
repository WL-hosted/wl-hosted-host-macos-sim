#include "repl_json.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool utf8_valid(const uint8_t *bytes, size_t size) {
    size_t index = 0u;
    while (index < size) {
        uint8_t lead = bytes[index];
        size_t continuation;
        uint32_t code_point;
        uint32_t minimum;
        if (lead == 0x00u)
            return false; /* embedded NUL would truncate the C string */
        if (lead < 0x80u) {
            index++;
            continue;
        }
        if ((lead & 0xe0u) == 0xc0u) {
            continuation = 1u;
            code_point = lead & 0x1fu;
            minimum = 0x80u;
        } else if ((lead & 0xf0u) == 0xe0u) {
            continuation = 2u;
            code_point = lead & 0x0fu;
            minimum = 0x800u;
        } else if ((lead & 0xf8u) == 0xf0u) {
            continuation = 3u;
            code_point = lead & 0x07u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (size - index <= continuation)
            return false;
        for (size_t offset = 1u; offset <= continuation; ++offset) {
            uint8_t byte = bytes[index + offset];
            if ((byte & 0xc0u) != 0x80u)
                return false;
            code_point = (code_point << 6u) | (byte & 0x3fu);
        }
        if (code_point < minimum || code_point > 0x10ffffu ||
            (code_point >= 0xd800u && code_point <= 0xdfffu))
            return false;
        index += continuation + 1u;
    }
    return true;
}

cJSON *wlh_repl_json_begin(const char *event) {
    cJSON *doc = cJSON_CreateObject();
    if (doc == NULL)
        return NULL;
    if (cJSON_AddStringToObject(doc, "source", "wlh-host-sim") == NULL ||
        cJSON_AddStringToObject(doc, "event", event) == NULL) {
        cJSON_Delete(doc);
        return NULL;
    }
    return doc;
}

void wlh_repl_json_add_hex(
    cJSON *doc, const char *key, const uint8_t *bytes, size_t size
) {
    static const char digits[] = "0123456789abcdef";
    char stack_buffer[129];
    char *hex = stack_buffer;
    cJSON *value;
    if (doc == NULL)
        return;
    if (size * 2u + 1u > sizeof(stack_buffer)) {
        hex = cJSON_malloc(size * 2u + 1u);
        if (hex == NULL)
            return;
    }
    for (size_t index = 0u; index < size; ++index) {
        hex[index * 2u] = digits[bytes[index] >> 4u];
        hex[index * 2u + 1u] = digits[bytes[index] & 0x0fu];
    }
    hex[size * 2u] = '\0';
    value = cJSON_CreateString(hex);
    if (hex != stack_buffer)
        cJSON_free(hex);
    if (value != NULL && !cJSON_AddItemToObject(doc, key, value))
        cJSON_Delete(value);
}

void wlh_repl_json_add_bytes(
    cJSON *doc, const char *key, const uint8_t *bytes, size_t size
) {
    char *text;
    cJSON *value;
    if (doc == NULL)
        return;
    text = cJSON_malloc(size + 1u);
    if (text == NULL)
        return;
    if (utf8_valid(bytes, size)) {
        memcpy(text, bytes, size);
        text[size] = '\0';
        value = cJSON_CreateString(text);
        cJSON_free(text);
        if (value != NULL && !cJSON_AddItemToObject(doc, key, value))
            cJSON_Delete(value);
        return;
    }
    for (size_t index = 0u; index < size; ++index) {
        uint8_t byte = bytes[index];
        text[index] = (byte >= 0x20u && byte <= 0x7eu) ? (char)byte : '.';
    }
    text[size] = '\0';
    value = cJSON_CreateString(text);
    cJSON_free(text);
    if (value != NULL && !cJSON_AddItemToObject(doc, key, value))
        cJSON_Delete(value);
    {
        char hex_key[64];
        (void)snprintf(hex_key, sizeof(hex_key), "%s_hex", key);
        wlh_repl_json_add_hex(doc, hex_key, bytes, size);
    }
}

char *wlh_repl_json_to_line(const cJSON *doc) {
    if (doc == NULL)
        return NULL;
    return cJSON_PrintUnformatted(doc);
}

void wlh_repl_json_emit(cJSON *doc) {
    char *line = wlh_repl_json_to_line(doc);
    pthread_mutex_lock(&print_mutex);
    if (line != NULL)
        fputs(line, stdout);
    else
        fputs(
            "{\"source\":\"wlh-host-sim\",\"event\":\"error\","
            "\"detail\":\"json allocation failed\"}",
            stdout
        );
    fputc('\n', stdout);
    fflush(stdout);
    pthread_mutex_unlock(&print_mutex);
    cJSON_free(line);
    cJSON_Delete(doc);
}
