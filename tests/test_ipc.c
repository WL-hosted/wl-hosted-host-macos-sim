#include "sim.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int read_all(int fd, uint8_t *data, size_t size) {
    while (size != 0u) {
        ssize_t got = read(fd, data, size);
        if (got <= 0)
            return -1;
        data += (size_t)got;
        size -= (size_t)got;
    }
    return 0;
}
static int write_all(int fd, const uint8_t *data, size_t size) {
    while (size != 0u) {
        ssize_t put = write(fd, data, size);
        if (put <= 0)
            return -1;
        data += (size_t)put;
        size -= (size_t)put;
    }
    return 0;
}

static void *peer_main(void *context) {
    int fd = *(int *)context;
    uint8_t hello[16];
    uint8_t prefix[4];
    uint8_t body[64];
    static const uint8_t response[16] = {
        'W', 'L', 'H', 'S', 'I', 'M', 0, 0, 1, 0, 3, 1, 27, 0, 1, 0
    };

    assert(read_all(fd, hello, sizeof(hello)) == 0);
    assert(hello[10] == SIM_ROLE_HOST);
    assert(write_all(fd, response, 3u) == 0);
    assert(write_all(fd, response + 3u, sizeof(response) - 3u) == 0);

    assert(read_all(fd, prefix, sizeof(prefix)) == 0);
    assert(prefix[0] == 7u);
    assert(read_all(fd, body, 7u) == 0);
    assert(
        body[0] == SIM_RECORD_WIRE_FRAME && memcmp(body + 4u, "abc", 3u) == 0
    );

    prefix[0] = 7u;
    prefix[1] = prefix[2] = prefix[3] = 0u;
    body[0] = SIM_RECORD_WIRE_FRAME;
    body[1] = body[2] = body[3] = 0u;
    memcpy(body + 4u, "xyz", 3u);

    assert(write_all(fd, prefix, 2u) == 0);
    assert(write_all(fd, prefix + 2u, 2u) == 0);
    assert(write_all(fd, body, 1u) == 0);
    assert(write_all(fd, body + 1u, 6u) == 0);
    close(fd);
    return NULL;
}

int main(void) {
    int pair[2];
    pthread_t peer;
    sim_ipc_t ipc;
    uint8_t kind;
    uint8_t *payload;
    size_t payload_size;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(pthread_create(&peer, NULL, peer_main, &pair[1]) == 0);
    {
        char endpoint[32];
        int null_fd = open("/dev/null", O_RDWR);
        assert(null_fd >= 0);
        snprintf(endpoint, sizeof(endpoint), "fd:%d", null_fd);
        assert(sim_ipc_open(&ipc, endpoint) != 0);
    }
    {
        char endpoint[32];
        snprintf(endpoint, sizeof(endpoint), "fd:%d", pair[0]);
        assert(sim_ipc_open(&ipc, endpoint) == 0);
    }

    assert(ipc.peer_role == SIM_ROLE_MANAGER && ipc.sideband);
    assert(
        sim_ipc_write(
            &ipc, SIM_RECORD_WIRE_FRAME, (const uint8_t *)"abc", 3u
        ) == 0
    );
    assert(sim_ipc_read(&ipc, &kind, &payload, &payload_size) == 0);
    assert(
        kind == SIM_RECORD_WIRE_FRAME && payload_size == 3u &&
        memcmp(payload, "xyz", 3u) == 0
    );

    free(payload);
    sim_ipc_close(&ipc);
    pthread_join(peer, NULL);
    puts("host sim IPC tests passed");
    return 0;
}
