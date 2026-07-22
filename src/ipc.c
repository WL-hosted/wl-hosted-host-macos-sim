#include "sim.h"
#include "wlh/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const uint8_t hello_magic[8] = {'W', 'L', 'H', 'S', 'I', 'M', 0, 0};

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u);
}
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}
static void write16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8u);
}
static void write32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8u);
    p[2] = (uint8_t)(v >> 16u);
    p[3] = (uint8_t)(v >> 24u);
}

static int write_all(int fd, const uint8_t *data, size_t size) {
    while (size != 0u) {
        ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        data += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, uint8_t *data, size_t size) {
    while (size != 0u) {
        ssize_t received = read(fd, data, size);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
            return -1;
        data += (size_t)received;
        size -= (size_t)received;
    }
    return 0;
}

static const char *role_name(enum sim_role role) {
    switch (role) {
    case SIM_ROLE_HOST:
        return "host";
    case SIM_ROLE_COPROC:
        return "coproc";
    case SIM_ROLE_MANAGER:
        return "manager";
    default:
        return "unknown";
    }
}

static int connect_unix(const char *path) {
    int fd;
    struct sockaddr_un address;
    if (strlen(path) >= sizeof(address.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        WLH_LOGW("host-sim", "unix connect to %s failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int sim_ipc_open(sim_ipc_t *ipc, const char *endpoint) {
    uint8_t local[SIM_IPC_HELLO_SIZE] = {0};
    uint8_t peer[SIM_IPC_HELLO_SIZE];
    char *end;
    long inherited;

    if (ipc == NULL || endpoint == NULL)
        return -1;
    memset(ipc, 0, sizeof(*ipc));
    ipc->fd = -1;

    if (strncmp(endpoint, "connect:", 8u) == 0) {
        ipc->fd = connect_unix(endpoint + 8u);
    } else if (strncmp(endpoint, "fd:", 3u) == 0) {
        errno = 0;
        inherited = strtol(endpoint + 3u, &end, 10);
        if (errno != 0 || *end != '\0' || inherited < 0 ||
            inherited > INT32_MAX)
            return -1;
        ipc->fd = (int)inherited;
    }
    if (ipc->fd < 0) {
        WLH_LOGW("host-sim", "IPC endpoint %s open failed", endpoint);
        return -1;
    }

    memcpy(local, hello_magic, sizeof(hello_magic));
    write16(local + 8u, 1u);
    local[10] = SIM_ROLE_HOST;
    local[11] = SIM_IPC_SIDEBAND_FLAG;
    write32(local + 12u, SIM_IPC_MAX_RECORD_SIZE);

    // clang-format off
    if (write_all(ipc->fd, local, sizeof(local)) != 0 ||
        read_all(ipc->fd, peer, sizeof(peer)) != 0 ||
        memcmp(peer, hello_magic, sizeof(hello_magic)) != 0 ||
        read16(peer + 8u) != 1u ||
        (peer[10] != SIM_ROLE_MANAGER && peer[10] != SIM_ROLE_COPROC) ||
        (peer[11] & (uint8_t)~SIM_IPC_SIDEBAND_FLAG) != 0u ||
        read32(peer + 12u) < 4u) {
        // clang-format on
        WLH_LOGW("host-sim", "IPC hello negotiation failed");
        sim_ipc_close(ipc);
        return -1;
    }

    ipc->peer_role = (enum sim_role)peer[10];
    ipc->max_record_size = read32(peer + 12u) < SIM_IPC_MAX_RECORD_SIZE
                               ? read32(peer + 12u)
                               : SIM_IPC_MAX_RECORD_SIZE;
    ipc->sideband = ipc->peer_role == SIM_ROLE_MANAGER &&
                    (peer[11] & SIM_IPC_SIDEBAND_FLAG) != 0u;
    WLH_LOGI(
        "host-sim",
        "IPC hello ok peer=%s max_record=%u sideband=%d",
        role_name(ipc->peer_role),
        ipc->max_record_size,
        ipc->sideband
    );

    if (pthread_mutex_init(&ipc->write_mutex, NULL) != 0) {
        sim_ipc_close(ipc);
        return -1;
    }
    return 0;
}

void sim_ipc_close(sim_ipc_t *ipc) {
    if (ipc == NULL)
        return;
    if (ipc->fd >= 0) {
        WLH_LOGI("host-sim", "IPC closing fd=%d", ipc->fd);
        shutdown(ipc->fd, SHUT_RDWR);
        close(ipc->fd);
        ipc->fd = -1;
    }
}

int sim_ipc_write(
    sim_ipc_t *ipc, uint8_t kind, const uint8_t *payload, size_t payload_size
) {
    uint8_t *record;
    size_t body_size = 4u + payload_size;
    int result;

    if (ipc == NULL || ipc->fd < 0 || kind < SIM_RECORD_WIRE_FRAME ||
        kind > SIM_RECORD_WIFI_COMMAND ||
        (payload_size != 0u && payload == NULL) ||
        body_size > ipc->max_record_size)
        return -1;

    record = malloc(4u + body_size);
    if (record == NULL)
        return -1;
    write32(record, (uint32_t)body_size);
    record[4] = kind;
    record[5] = 0u;
    record[6] = 0u;
    record[7] = 0u;
    if (payload_size != 0u)
        memcpy(record + 8u, payload, payload_size);

    pthread_mutex_lock(&ipc->write_mutex);
    result = write_all(ipc->fd, record, 4u + body_size);
    pthread_mutex_unlock(&ipc->write_mutex);
    free(record);
    return result;
}

int sim_ipc_read(
    sim_ipc_t *ipc, uint8_t *kind, uint8_t **payload, size_t *payload_size
) {
    uint8_t length_bytes[4];
    uint8_t fixed[4];
    uint32_t body_size;

    if (ipc == NULL || kind == NULL || payload == NULL ||
        payload_size == NULL ||
        read_all(ipc->fd, length_bytes, sizeof(length_bytes)) != 0)
        return -1;

    body_size = read32(length_bytes);
    if (body_size < 4u || body_size > ipc->max_record_size ||
        read_all(ipc->fd, fixed, sizeof(fixed)) != 0 ||
        fixed[0] < SIM_RECORD_WIRE_FRAME ||
        fixed[0] > SIM_RECORD_WIFI_COMMAND || fixed[1] != 0u ||
        fixed[2] != 0u || fixed[3] != 0u)
        return -1;

    *payload_size = body_size - 4u;
    *payload = *payload_size == 0u ? NULL : malloc(*payload_size);
    if (*payload_size != 0u &&
        (*payload == NULL || read_all(ipc->fd, *payload, *payload_size) != 0)) {
        free(*payload);
        *payload = NULL;
        return -1;
    }

    *kind = fixed[0];
    return 0;
}
