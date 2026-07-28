#include "bond_store.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "wlh/log.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BOND_MAGIC 0x424c4857u /* "WLHB" */
#define BOND_VERSION 1u
#define BOND_MAX_SEC 16u
#define BOND_MAX_CCCD 64u
#define BOND_REC_OUR_SEC 1u
#define BOND_REC_PEER_SEC 2u
#define BOND_REC_CCCD 3u
#define BOND_SEC_PAYLOAD 75u
#define BOND_CCCD_PAYLOAD 12u

typedef struct bond_sec_entry {
    struct ble_store_value_sec value;
    uint64_t stamp;
} bond_sec_entry_t;

typedef struct bond_store {
    char path[1024];
    bond_sec_entry_t our_sec[BOND_MAX_SEC];
    size_t our_sec_count;
    bond_sec_entry_t peer_sec[BOND_MAX_SEC];
    size_t peer_sec_count;
    struct ble_store_value_cccd cccd[BOND_MAX_CCCD];
    size_t cccd_count;
    uint64_t next_stamp;
    bool ready;
} bond_store_t;

static bond_store_t bond_store;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size) {
    size_t index;
    int bit;

    crc = ~crc;
    for (index = 0; index < size; ++index) {
        crc ^= data[index];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
}

static void put_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value) {
    put_u16(out, (uint16_t)value);
    put_u16(out + 2, (uint16_t)(value >> 16));
}

static void put_u64(uint8_t *out, uint64_t value) {
    put_u32(out, (uint32_t)value);
    put_u32(out + 4, (uint32_t)(value >> 32));
}

static uint16_t get_u16(const uint8_t *in) {
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in) {
    return get_u16(in) | ((uint32_t)get_u16(in + 2) << 16);
}

static uint64_t get_u64(const uint8_t *in) {
    return get_u32(in) | ((uint64_t)get_u32(in + 4) << 32);
}

static bool addr_is_any(const ble_addr_t *addr) {
    static const ble_addr_t any;
    return memcmp(addr, &any, sizeof(any)) == 0;
}

static bool addr_equal(const ble_addr_t *left, const ble_addr_t *right) {
    return left->type == right->type &&
           memcmp(left->val, right->val, sizeof(left->val)) == 0;
}

static void encode_sec(uint8_t *out, const bond_sec_entry_t *entry) {
    const struct ble_store_value_sec *sec = &entry->value;

    out[0] = sec->peer_addr.type;
    memcpy(out + 1, sec->peer_addr.val, 6);
    out[7] = sec->key_size;
    put_u16(out + 8, sec->ediv);
    put_u64(out + 10, sec->rand_num);
    memcpy(out + 18, sec->ltk, 16);
    memcpy(out + 34, sec->irk, 16);
    memcpy(out + 50, sec->csrk, 16);
    out[66] =
        (uint8_t)((sec->ltk_present ? 0x01u : 0u) |
                  (sec->irk_present ? 0x02u : 0u) |
                  (sec->csrk_present ? 0x04u : 0u) |
                  (sec->authenticated ? 0x08u : 0u) | (sec->sc ? 0x10u : 0u));
    put_u64(out + 67, entry->stamp);
}

static void decode_sec(const uint8_t *in, bond_sec_entry_t *entry) {
    struct ble_store_value_sec *sec = &entry->value;

    memset(entry, 0, sizeof(*entry));
    sec->peer_addr.type = in[0];
    memcpy(sec->peer_addr.val, in + 1, 6);
    sec->key_size = in[7];
    sec->ediv = get_u16(in + 8);
    sec->rand_num = get_u64(in + 10);
    memcpy(sec->ltk, in + 18, 16);
    memcpy(sec->irk, in + 34, 16);
    memcpy(sec->csrk, in + 50, 16);
    sec->ltk_present = (in[66] & 0x01u) != 0u;
    sec->irk_present = (in[66] & 0x02u) != 0u;
    sec->csrk_present = (in[66] & 0x04u) != 0u;
    sec->authenticated = (in[66] & 0x08u) != 0u;
    sec->sc = (in[66] & 0x10u) != 0u;
    entry->stamp = get_u64(in + 67);
}

static void encode_cccd(uint8_t *out, const struct ble_store_value_cccd *cccd) {
    out[0] = cccd->peer_addr.type;
    memcpy(out + 1, cccd->peer_addr.val, 6);
    put_u16(out + 7, cccd->chr_val_handle);
    put_u16(out + 9, cccd->flags);
    out[11] = cccd->value_changed ? 1u : 0u;
}

static void decode_cccd(const uint8_t *in, struct ble_store_value_cccd *cccd) {
    memset(cccd, 0, sizeof(*cccd));
    cccd->peer_addr.type = in[0];
    memcpy(cccd->peer_addr.val, in + 1, 6);
    cccd->chr_val_handle = get_u16(in + 7);
    cccd->flags = get_u16(in + 9);
    cccd->value_changed = in[11] != 0u;
}

static int bond_store_persist(void) {
    uint8_t buffer
        [16 + (2u * BOND_MAX_SEC) * (3u + BOND_SEC_PAYLOAD) +
         BOND_MAX_CCCD * (3u + BOND_CCCD_PAYLOAD) + 4];
    char temp_path[sizeof(bond_store.path) + 8];
    size_t offset = 16;
    size_t index;
    uint32_t count;
    int fd;
    ssize_t written;

    count = (uint32_t)(bond_store.our_sec_count + bond_store.peer_sec_count +
                       bond_store.cccd_count);
    put_u32(buffer, BOND_MAGIC);
    put_u32(buffer + 4, BOND_VERSION);
    put_u32(buffer + 8, count);
    put_u32(buffer + 12, 0u);

    for (index = 0; index < bond_store.our_sec_count; ++index) {
        buffer[offset] = BOND_REC_OUR_SEC;
        put_u16(buffer + offset + 1, BOND_SEC_PAYLOAD);
        encode_sec(buffer + offset + 3, &bond_store.our_sec[index]);
        offset += 3u + BOND_SEC_PAYLOAD;
    }
    for (index = 0; index < bond_store.peer_sec_count; ++index) {
        buffer[offset] = BOND_REC_PEER_SEC;
        put_u16(buffer + offset + 1, BOND_SEC_PAYLOAD);
        encode_sec(buffer + offset + 3, &bond_store.peer_sec[index]);
        offset += 3u + BOND_SEC_PAYLOAD;
    }
    for (index = 0; index < bond_store.cccd_count; ++index) {
        buffer[offset] = BOND_REC_CCCD;
        put_u16(buffer + offset + 1, BOND_CCCD_PAYLOAD);
        encode_cccd(buffer + offset + 3, &bond_store.cccd[index]);
        offset += 3u + BOND_CCCD_PAYLOAD;
    }
    put_u32(buffer + offset, crc32_update(0u, buffer, offset));
    offset += 4;

    snprintf(temp_path, sizeof(temp_path), "%s.tmp", bond_store.path);
    fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        WLH_LOGW(
            "host-ble",
            "bond store open %s failed: %s",
            temp_path,
            strerror(errno)
        );
        return -1;
    }
    written = write(fd, buffer, offset);
    if (written != (ssize_t)offset || fsync(fd) != 0 || close(fd) != 0) {
        WLH_LOGW("host-ble", "bond store write failed: %s", strerror(errno));
        close(fd);
        unlink(temp_path);
        return -1;
    }
    if (rename(temp_path, bond_store.path) != 0) {
        WLH_LOGW("host-ble", "bond store rename failed: %s", strerror(errno));
        unlink(temp_path);
        return -1;
    }
    return 0;
}

static void bond_store_quarantine(const char *reason) {
    char backup[sizeof(bond_store.path) + 32];

    snprintf(
        backup,
        sizeof(backup),
        "%s.corrupt.%lld",
        bond_store.path,
        (long long)time(NULL)
    );
    if (rename(bond_store.path, backup) == 0)
        WLH_LOGW(
            "host-ble",
            "bond store %s (%s); moved to %s, starting empty",
            reason,
            bond_store.path,
            backup
        );
    else
        WLH_LOGW(
            "host-ble",
            "bond store %s and backup failed (%s); starting empty",
            reason,
            strerror(errno)
        );
}

static int bond_store_load(void) {
    uint8_t buffer
        [16 + (2u * BOND_MAX_SEC) * (3u + BOND_SEC_PAYLOAD) +
         BOND_MAX_CCCD * (3u + BOND_CCCD_PAYLOAD) + 4];
    FILE *file;
    size_t size;
    size_t offset = 16;
    uint32_t version;
    uint32_t count;
    uint32_t index;

    file = fopen(bond_store.path, "rb");
    if (file == NULL)
        return 0;
    size = fread(buffer, 1, sizeof(buffer), file);
    if (!feof(file)) {
        fclose(file);
        bond_store_quarantine("oversized");
        return 0;
    }
    fclose(file);

    if (size < 20 || get_u32(buffer) != BOND_MAGIC) {
        bond_store_quarantine("corrupt header");
        return 0;
    }
    version = get_u32(buffer + 4);
    if (version != BOND_VERSION) {
        char backup[sizeof(bond_store.path) + 32];

        snprintf(backup, sizeof(backup), "%s.unsupported", bond_store.path);
        if (rename(bond_store.path, backup) != 0) {
            WLH_LOGW(
                "host-ble",
                "bond store version %u unsupported; refusing to overwrite %s",
                version,
                bond_store.path
            );
            return -1;
        }
        WLH_LOGW(
            "host-ble",
            "bond store version %u unsupported; preserved as %s, starting "
            "empty",
            version,
            backup
        );
        return 0;
    }
    if (crc32_update(0u, buffer, size - 4) != get_u32(buffer + size - 4)) {
        bond_store_quarantine("checksum mismatch");
        return 0;
    }

    count = get_u32(buffer + 8);
    for (index = 0; index < count; ++index) {
        uint8_t type;
        uint16_t length;

        if (offset + 3 > size - 4)
            break;
        type = buffer[offset];
        length = get_u16(buffer + offset + 1);
        if (offset + 3 + length > size - 4)
            break;
        if (type == BOND_REC_OUR_SEC && length == BOND_SEC_PAYLOAD &&
            bond_store.our_sec_count < BOND_MAX_SEC)
            decode_sec(
                buffer + offset + 3,
                &bond_store.our_sec[bond_store.our_sec_count++]
            );
        else if (type == BOND_REC_PEER_SEC && length == BOND_SEC_PAYLOAD &&
                 bond_store.peer_sec_count < BOND_MAX_SEC)
            decode_sec(
                buffer + offset + 3,
                &bond_store.peer_sec[bond_store.peer_sec_count++]
            );
        else if (type == BOND_REC_CCCD && length == BOND_CCCD_PAYLOAD &&
                 bond_store.cccd_count < BOND_MAX_CCCD)
            decode_cccd(
                buffer + offset + 3, &bond_store.cccd[bond_store.cccd_count++]
            );
        offset += 3u + length;
    }
    for (index = 0; index < bond_store.our_sec_count; ++index)
        if (bond_store.our_sec[index].stamp >= bond_store.next_stamp)
            bond_store.next_stamp = bond_store.our_sec[index].stamp + 1u;
    for (index = 0; index < bond_store.peer_sec_count; ++index)
        if (bond_store.peer_sec[index].stamp >= bond_store.next_stamp)
            bond_store.next_stamp = bond_store.peer_sec[index].stamp + 1u;
    WLH_LOGI(
        "host-ble",
        "bond store loaded: %zu our, %zu peer, %zu cccd",
        bond_store.our_sec_count,
        bond_store.peer_sec_count,
        bond_store.cccd_count
    );
    return 0;
}

static bond_sec_entry_t *sec_array(int obj_type, size_t **count_out) {
    if (obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC) {
        *count_out = &bond_store.our_sec_count;
        return bond_store.our_sec;
    }
    *count_out = &bond_store.peer_sec_count;
    return bond_store.peer_sec;
}

static int sec_find(
    const bond_sec_entry_t *entries,
    size_t count,
    const struct ble_store_key_sec *key
) {
    size_t index;
    uint8_t skip = key->idx;

    for (index = 0; index < count; ++index) {
        if (!addr_is_any(&key->peer_addr) &&
            !addr_equal(&entries[index].value.peer_addr, &key->peer_addr))
            continue;
        if (skip > 0u) {
            --skip;
            continue;
        }
        return (int)index;
    }
    return -1;
}

static void purge_peer(const ble_addr_t *peer_addr) {
    size_t index = 0;

    while (index < bond_store.our_sec_count) {
        if (addr_equal(&bond_store.our_sec[index].value.peer_addr, peer_addr))
            bond_store.our_sec[index] =
                bond_store.our_sec[--bond_store.our_sec_count];
        else
            ++index;
    }
    index = 0;
    while (index < bond_store.peer_sec_count) {
        if (addr_equal(&bond_store.peer_sec[index].value.peer_addr, peer_addr))
            bond_store.peer_sec[index] =
                bond_store.peer_sec[--bond_store.peer_sec_count];
        else
            ++index;
    }
    index = 0;
    while (index < bond_store.cccd_count) {
        if (addr_equal(&bond_store.cccd[index].peer_addr, peer_addr))
            bond_store.cccd[index] = bond_store.cccd[--bond_store.cccd_count];
        else
            ++index;
    }
}

/* Evicts the least recently used bond whose peer has no live connection.
 * Returns 0 when an entry was removed. */
static int evict_lru_bond(void) {
    const bond_sec_entry_t *oldest = NULL;
    size_t index;
    ble_addr_t victim;

    for (index = 0; index < bond_store.our_sec_count; ++index) {
        const bond_sec_entry_t *entry = &bond_store.our_sec[index];

        if (ble_gap_conn_find_by_addr(&entry->value.peer_addr, NULL) == 0)
            continue;
        if (oldest == NULL || entry->stamp < oldest->stamp)
            oldest = entry;
    }
    for (index = 0; index < bond_store.peer_sec_count; ++index) {
        const bond_sec_entry_t *entry = &bond_store.peer_sec[index];

        if (ble_gap_conn_find_by_addr(&entry->value.peer_addr, NULL) == 0)
            continue;
        if (oldest == NULL || entry->stamp < oldest->stamp)
            oldest = entry;
    }
    if (oldest == NULL)
        return -1;
    victim = oldest->value.peer_addr;
    WLH_LOGW(
        "host-ble",
        "bond store full; evicting LRU bond %02x:%02x:%02x:%02x:%02x:%02x",
        victim.val[5],
        victim.val[4],
        victim.val[3],
        victim.val[2],
        victim.val[1],
        victim.val[0]
    );
    purge_peer(&victim);
    return 0;
}

static int store_read(
    int obj_type, const union ble_store_key *key, union ble_store_value *dst
) {
    if (obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC ||
        obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        size_t *count;
        bond_sec_entry_t *entries = sec_array(obj_type, &count);
        int index = sec_find(entries, *count, &key->sec);

        if (index < 0)
            return BLE_HS_ENOENT;
        entries[index].stamp = bond_store.next_stamp++;
        dst->sec = entries[index].value;
        return 0;
    }
    if (obj_type == BLE_STORE_OBJ_TYPE_CCCD) {
        size_t index;
        uint8_t skip = key->cccd.idx;

        for (index = 0; index < bond_store.cccd_count; ++index) {
            const struct ble_store_value_cccd *cccd = &bond_store.cccd[index];

            if (!addr_is_any(&key->cccd.peer_addr) &&
                !addr_equal(&cccd->peer_addr, &key->cccd.peer_addr))
                continue;
            if (key->cccd.chr_val_handle != 0u &&
                cccd->chr_val_handle != key->cccd.chr_val_handle)
                continue;
            if (skip > 0u) {
                --skip;
                continue;
            }
            dst->cccd = *cccd;
            return 0;
        }
        return BLE_HS_ENOENT;
    }
    return BLE_HS_ENOTSUP;
}

static int store_write(int obj_type, const union ble_store_value *val) {
    if (obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC ||
        obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        size_t *count;
        bond_sec_entry_t *entries = sec_array(obj_type, &count);
        struct ble_store_key_sec key;
        int index;

        memset(&key, 0, sizeof(key));
        key.peer_addr = val->sec.peer_addr;
        index = sec_find(entries, *count, &key);
        if (index < 0) {
            if (*count >= BOND_MAX_SEC && evict_lru_bond() != 0)
                return BLE_HS_ESTORE_CAP;
            if (*count >= BOND_MAX_SEC)
                return BLE_HS_ESTORE_CAP;
            index = (int)(*count)++;
        }
        entries[index].value = val->sec;
        entries[index].stamp = bond_store.next_stamp++;
    } else if (obj_type == BLE_STORE_OBJ_TYPE_CCCD) {
        size_t index;

        for (index = 0; index < bond_store.cccd_count; ++index)
            if (addr_equal(
                    &bond_store.cccd[index].peer_addr, &val->cccd.peer_addr
                ) &&
                bond_store.cccd[index].chr_val_handle ==
                    val->cccd.chr_val_handle)
                break;
        if (index == bond_store.cccd_count) {
            if (bond_store.cccd_count >= BOND_MAX_CCCD && evict_lru_bond() != 0)
                return BLE_HS_ESTORE_CAP;
            if (bond_store.cccd_count >= BOND_MAX_CCCD)
                return BLE_HS_ESTORE_CAP;
            ++bond_store.cccd_count;
        }
        bond_store.cccd[index] = val->cccd;
    } else {
        return BLE_HS_ENOTSUP;
    }
    return bond_store_persist() == 0 ? 0 : BLE_HS_ESTORE_FAIL;
}

static int store_delete(int obj_type, const union ble_store_key *key) {
    if (obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC ||
        obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        size_t *count;
        bond_sec_entry_t *entries = sec_array(obj_type, &count);
        int index = sec_find(entries, *count, &key->sec);

        if (index < 0)
            return BLE_HS_ENOENT;
        entries[index] = entries[--(*count)];
    } else if (obj_type == BLE_STORE_OBJ_TYPE_CCCD) {
        size_t index;
        bool found = false;

        for (index = 0; index < bond_store.cccd_count; ++index) {
            const struct ble_store_value_cccd *cccd = &bond_store.cccd[index];

            if (!addr_is_any(&key->cccd.peer_addr) &&
                !addr_equal(&cccd->peer_addr, &key->cccd.peer_addr))
                continue;
            if (key->cccd.chr_val_handle != 0u &&
                cccd->chr_val_handle != key->cccd.chr_val_handle)
                continue;
            bond_store.cccd[index] = bond_store.cccd[--bond_store.cccd_count];
            found = true;
            break;
        }
        if (!found)
            return BLE_HS_ENOENT;
    } else {
        return BLE_HS_ENOTSUP;
    }
    return bond_store_persist() == 0 ? 0 : BLE_HS_ESTORE_FAIL;
}

static int resolve_default_path(char *out, size_t out_size) {
    const struct passwd *pw = getpwuid(getuid());
    int written;

    if (pw == NULL || pw->pw_dir == NULL || pw->pw_dir[0] == '\0') {
        WLH_LOGE("host-ble", "cannot resolve home directory for bond store");
        return -1;
    }
    written = snprintf(
        out,
        out_size,
        "%s/Library/Application Support/WL-hosted/bonds.bin",
        pw->pw_dir
    );
    if (written < 0 || (size_t)written >= out_size)
        return -1;
    return 0;
}

static int ensure_parent_directory(const char *path) {
    char parent[sizeof(bond_store.path)];
    char *cursor;

    snprintf(parent, sizeof(parent), "%s", path);
    cursor = strrchr(parent, '/');
    if (cursor == NULL)
        return 0;
    *cursor = '\0';
    if (parent[0] == '\0')
        return 0;

    for (cursor = parent + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(parent, 0700) != 0 && errno != EEXIST)
            return -1;
        *cursor = '/';
    }
    if (mkdir(parent, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int wlh_ble_bond_store_init(const char *path, bool clear_bonds) {
    memset(&bond_store, 0, sizeof(bond_store));
    bond_store.next_stamp = 1u;

    if (path != NULL) {
        if (strlen(path) >= sizeof(bond_store.path)) {
            WLH_LOGE("host-ble", "bond store path too long");
            return -1;
        }
        snprintf(bond_store.path, sizeof(bond_store.path), "%s", path);
    } else if (resolve_default_path(bond_store.path, sizeof(bond_store.path)) !=
               0) {
        return -1;
    }
    if (ensure_parent_directory(bond_store.path) != 0) {
        WLH_LOGE(
            "host-ble",
            "cannot create bond store directory for %s: %s",
            bond_store.path,
            strerror(errno)
        );
        return -1;
    }

    if (clear_bonds) {
        char temp_path[sizeof(bond_store.path) + 8];

        snprintf(temp_path, sizeof(temp_path), "%s.tmp", bond_store.path);
        if (unlink(bond_store.path) != 0 && errno != ENOENT) {
            WLH_LOGE(
                "host-ble",
                "cannot clear bond store %s: %s",
                bond_store.path,
                strerror(errno)
            );
            return -1;
        }
        (void)unlink(temp_path);
        WLH_LOGI("host-ble", "bond store cleared: %s", bond_store.path);
    } else if (bond_store_load() != 0) {
        return -1;
    }

    ble_hs_cfg.store_read_cb = store_read;
    ble_hs_cfg.store_write_cb = store_write;
    ble_hs_cfg.store_delete_cb = store_delete;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    bond_store.ready = true;
    return 0;
}

void wlh_ble_bond_store_deinit(void) {
    if (!bond_store.ready)
        return;
    ble_hs_cfg.store_read_cb = NULL;
    ble_hs_cfg.store_write_cb = NULL;
    ble_hs_cfg.store_delete_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;
    memset(&bond_store, 0, sizeof(bond_store));
}
