#ifndef WLH_HOST_SIM_REPL_JSON_H
#define WLH_HOST_SIM_REPL_JSON_H

#include "cJSON/cJSON.h"

#include <stddef.h>
#include <stdint.h>

/* Creates a JSON Lines document seeded with source=wlh-host-sim and the
 * event name. Returns NULL on allocation failure; every helper below and
 * wlh_repl_json_emit() accept a NULL document. */
cJSON *wlh_repl_json_begin(const char *event);

/* Adds arbitrary bytes under `key`. Valid UTF-8 without embedded NUL is
 * stored verbatim; anything else is stored as a printable-sanitized string
 * plus a `<key>_hex` field carrying the raw bytes. */
void wlh_repl_json_add_bytes(
    cJSON *doc, const char *key, const uint8_t *bytes, size_t size
);

void wlh_repl_json_add_hex(
    cJSON *doc, const char *key, const uint8_t *bytes, size_t size
);

/* Single-line serialized form; caller releases with cJSON_free(). Returns
 * NULL on allocation failure. */
char *wlh_repl_json_to_line(const cJSON *doc);

/* Writes the document as one line to stdout (mutex-serialized, flushed) and
 * deletes it. Serialization failure degrades to a fixed error line so the
 * stream never carries a partial document. */
void wlh_repl_json_emit(cJSON *doc);

#endif
