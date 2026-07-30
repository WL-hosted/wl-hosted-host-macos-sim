#include "iperf_controller.h"

#include "iperf2.h"
#include "network.h"
#include "repl_json.h"
#include "wlh/log.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPERF_PORT 5001u
#define IPERF_TCP_PACKET_SIZE 1400u
#define IPERF_MAX_DURATION 300u
#define IPERF_MAX_MBPS 100u
#define IPERF_REPORT_MS 3000u
#define IPERF_UDP_ACK_ATTEMPTS 10u
#define IPERF_UDP_ACK_WAIT_MS 250u

struct wlh_iperf_controller {
    app_t *app;
    pthread_mutex_t mutex;
    pthread_t worker;
    bool joinable;
    bool running;
    atomic_bool cancel;
    int socket_fd;
    char cancel_detail[96];
    wlh_iperf_request_t request;
    char peer[16];
};

static uint64_t now_us(void) {
    return wlh_app_monotonic_ms() * 1000u;
}
static const char *protocol_name(wlh_iperf_protocol_t protocol) {
    return protocol == WLH_IPERF_TCP ? "tcp" : "udp";
}
static const char *role_name(wlh_iperf_role_t role) {
    return role == WLH_IPERF_CLIENT ? "client" : "server";
}
static const char *direction(const wlh_iperf_request_t *request) {
    return request->role == WLH_IPERF_CLIENT ? "tx" : "rx";
}
static void emit_started(struct wlh_iperf_controller *controller) {
    cJSON *doc = wlh_repl_json_begin("iperf_started");
    if (doc != NULL) {
        cJSON_AddStringToObject(
            doc, "protocol", protocol_name(controller->request.protocol)
        );
        cJSON_AddStringToObject(
            doc, "role", role_name(controller->request.role)
        );
        cJSON_AddStringToObject(
            doc, "direction", direction(&controller->request)
        );
        cJSON_AddStringToObject(
            doc, "peer", controller->peer[0] ? controller->peer : "any"
        );
        cJSON_AddNumberToObject(doc, "port", IPERF_PORT);
        cJSON_AddNumberToObject(
            doc, "duration", controller->request.duration_sec
        );
        if (controller->request.protocol == WLH_IPERF_UDP)
            cJSON_AddNumberToObject(
                doc,
                "target_bits_per_second",
                (double)controller->request.target_mbps * 1000000.0
            );
    }
    wlh_repl_json_emit(doc);
}
static void emit_interval(
    struct wlh_iperf_controller *controller,
    uint64_t bytes,
    uint64_t interval_bytes,
    uint64_t elapsed_ms,
    uint64_t interval_ms,
    const wlh_iperf2_udp_stats_t *udp
) {
    cJSON *doc = wlh_repl_json_begin("iperf_interval");
    if (doc != NULL) {
        cJSON_AddNumberToObject(doc, "interval", (double)elapsed_ms / 1000.0);
        cJSON_AddNumberToObject(doc, "bytes", (double)bytes);
        cJSON_AddNumberToObject(
            doc,
            "bits_per_second",
            interval_ms ? (double)interval_bytes * 8000.0 / (double)interval_ms
                        : 0.0
        );
        cJSON_AddNumberToObject(
            doc,
            "cumulative_bits_per_second",
            elapsed_ms ? (double)bytes * 8000.0 / (double)elapsed_ms : 0.0
        );
        if (udp != NULL) {
            cJSON_AddNumberToObject(doc, "packets", (double)udp->packets);
            cJSON_AddNumberToObject(doc, "lost_packets", (double)udp->lost);
            cJSON_AddNumberToObject(
                doc, "out_of_order", (double)udp->out_of_order
            );
            cJSON_AddNumberToObject(doc, "jitter_ms", udp->jitter_ms);
        }
    }
    wlh_repl_json_emit(doc);
}
static void emit_result(
    struct wlh_iperf_controller *controller,
    uint64_t bytes,
    uint64_t elapsed_ms,
    bool success,
    const char *detail,
    const wlh_iperf2_udp_stats_t *udp
) {
    cJSON *doc = wlh_repl_json_begin("iperf_result");
    if (doc != NULL) {
        cJSON_AddStringToObject(
            doc, "protocol", protocol_name(controller->request.protocol)
        );
        cJSON_AddStringToObject(
            doc, "role", role_name(controller->request.role)
        );
        cJSON_AddStringToObject(
            doc, "direction", direction(&controller->request)
        );
        cJSON_AddStringToObject(
            doc, "peer", controller->peer[0] ? controller->peer : "any"
        );
        cJSON_AddNumberToObject(doc, "port", IPERF_PORT);
        cJSON_AddNumberToObject(doc, "duration_ms", (double)elapsed_ms);
        cJSON_AddNumberToObject(doc, "bytes", (double)bytes);
        cJSON_AddNumberToObject(
            doc,
            "bits_per_second",
            elapsed_ms ? (double)bytes * 8000.0 / (double)elapsed_ms : 0.0
        );
        cJSON_AddBoolToObject(doc, "success", success);
        cJSON_AddStringToObject(doc, "detail", detail);
        if (udp != NULL) {
            uint64_t expected = udp->packets + udp->lost;
            cJSON_AddNumberToObject(doc, "packets", (double)udp->packets);
            cJSON_AddNumberToObject(doc, "lost_packets", (double)udp->lost);
            cJSON_AddNumberToObject(
                doc,
                "lost_percent",
                expected ? (double)udp->lost * 100.0 / (double)expected : 0.0
            );
            cJSON_AddNumberToObject(
                doc, "out_of_order", (double)udp->out_of_order
            );
            cJSON_AddNumberToObject(doc, "jitter_ms", udp->jitter_ms);
            cJSON_AddNumberToObject(
                doc,
                "target_bits_per_second",
                (double)controller->request.target_mbps * 1000000.0
            );
        }
    }
    wlh_repl_json_emit(doc);
}
static void set_socket(struct wlh_iperf_controller *controller, int fd) {
    pthread_mutex_lock(&controller->mutex);
    controller->socket_fd = fd;
    pthread_mutex_unlock(&controller->mutex);
}
static bool cancelled(struct wlh_iperf_controller *controller) {
    return atomic_load(&controller->cancel);
}
static int wait_ready(int fd, bool write_ready, uint32_t timeout_ms) {
    fd_set set;
    struct timeval timeout = {
        (long)(timeout_ms / 1000u), (long)((timeout_ms % 1000u) * 1000u)
    };
    FD_ZERO(&set);
    FD_SET(fd, &set);
    return lwip_select(
        fd + 1,
        write_ready ? NULL : &set,
        write_ready ? &set : NULL,
        NULL,
        &timeout
    );
}
static int set_socket_timeout(int fd, int option, uint32_t timeout_ms) {
    struct timeval timeout = {
        (long)(timeout_ms / 1000u), (long)((timeout_ms % 1000u) * 1000u)
    };
    return lwip_setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
}
static bool send_backpressured(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS ||
           errno == ENOMEM || errno == EHOSTUNREACH || errno < 0;
}
/* The iPerf2 client retransmits its terminal UDP datagram while it waits for
 * the server's report.  Mirror Server::write_UDP_AckFIN(): retransmit the
 * report when another datagram arrives, then treat a quiet interval as an
 * acknowledgement.  A server-side deadline can expire after the client's
 * FIN was lost, so that path sends all bounded attempts instead of requiring
 * an incoming retransmission to trigger them. */
static bool send_udp_server_report(
    struct wlh_iperf_controller *controller,
    int fd,
    const struct sockaddr_in *peer,
    socklen_t peer_size,
    uint64_t bytes,
    uint64_t elapsed_ms,
    const wlh_iperf2_udp_stats_t *stats,
    bool retry_without_terminal
) {
    uint8_t report[WLH_IPERF2_DEFAULT_UDP_PACKET_SIZE] = {0};
    uint8_t received[WLH_IPERF2_DEFAULT_UDP_PACKET_SIZE];
    uint32_t attempt;
    if (!wlh_iperf2_udp_encode_server_report(
            report, sizeof(report), bytes, elapsed_ms, stats
        ))
        return false;
    for (attempt = 0u;
         attempt < IPERF_UDP_ACK_ATTEMPTS && !cancelled(controller);
         ++attempt) {
        int ready;
        int sent = lwip_sendto(
            fd,
            report,
            sizeof(report),
            0,
            (const struct sockaddr *)peer,
            peer_size
        );
        if (sent != (int)sizeof(report))
            return false;
        ready = wait_ready(fd, false, IPERF_UDP_ACK_WAIT_MS);
        if (ready < 0)
            return false;
        if (ready == 0) {
            if (!retry_without_terminal)
                return true;
            continue;
        }
        (void)lwip_recvfrom(fd, received, sizeof(received), 0, NULL, NULL);
    }
    return !cancelled(controller);
}
static void *worker_main(void *argument) {
    struct wlh_iperf_controller *controller = argument;
    uint8_t payload[WLH_IPERF2_DEFAULT_UDP_PACKET_SIZE] = {0};
    struct sockaddr_in peer;
    uint64_t started = wlh_app_monotonic_ms(), last_report = started,
             last_bytes = 0u, bytes = 0u, udp_first_packet_ms = 0u;
    uint32_t udp_client_duration_ms = 0u;
    wlh_iperf2_udp_stats_t udp;
    struct sockaddr_in udp_peer;
    socklen_t udp_peer_size = 0u;
    int fd = -1, data_fd = -1;
    uint32_t send_backpressure_count = 0u;
    const size_t packet_size = controller->request.protocol == WLH_IPERF_UDP
                                   ? WLH_IPERF2_DEFAULT_UDP_PACKET_SIZE
                                   : IPERF_TCP_PACKET_SIZE;
    bool success = false;
    const char *detail = "completed";
    memset(&peer, 0, sizeof(peer));
    wlh_iperf2_udp_stats_init(&udp);
    emit_started(controller);
    fd = lwip_socket(
        AF_INET,
        controller->request.protocol == WLH_IPERF_TCP ? SOCK_STREAM
                                                      : SOCK_DGRAM,
        0
    );
    if (fd < 0) {
        detail = "socket allocation failed";
        goto done;
    }
    set_socket(controller, fd);
    peer.sin_family = AF_INET;
    peer.sin_port = lwip_htons(IPERF_PORT);
    if (controller->request.role == WLH_IPERF_CLIENT) {
        if (!ip4addr_aton(controller->peer, (ip4_addr_t *)&peer.sin_addr)) {
            detail = "invalid IPv4 peer";
            goto done;
        }
        if (lwip_connect(fd, (struct sockaddr *)&peer, sizeof(peer)) != 0) {
            detail = "connection failed";
            goto done;
        }
        data_fd = fd;
        /* Keep the socket blocking, but bound every send so cancellation and
         * the test deadline progress under TCP or UDP output backpressure. */
        if (set_socket_timeout(data_fd, SO_SNDTIMEO, 250u) != 0) {
            detail = "failed to set send timeout";
            goto done;
        }
    } else {
        peer.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
        if (lwip_bind(fd, (struct sockaddr *)&peer, sizeof(peer)) != 0) {
            detail = "bind failed";
            goto done;
        }
        if (controller->request.protocol == WLH_IPERF_TCP) {
            if (lwip_listen(fd, 1) != 0) {
                detail = "listen failed";
                goto done;
            }
            if (wait_ready(
                    fd, false, controller->request.duration_sec * 1000u
                ) <= 0) {
                detail = "server wait timed out";
                goto done;
            }
            data_fd = lwip_accept(fd, NULL, NULL);
            if (data_fd < 0) {
                detail = "accept failed";
                goto done;
            }
            set_socket(controller, data_fd);
            /* Server duration has two bounded phases: wait up to the
             * requested duration for a client, then grant an accepted client
             * its own full receive interval.  Otherwise launch/scheduling
             * delay before the Mac client starts truncates its TCP stream. */
            started = wlh_app_monotonic_ms();
            last_report = started;
            last_bytes = 0u;
        } else
            data_fd = fd;
        if (set_socket_timeout(data_fd, SO_RCVTIMEO, 250u) != 0) {
            detail = "failed to set receive timeout";
            goto done;
        }
    }
    while (!cancelled(controller)) {
        int count;
        uint64_t current = wlh_app_monotonic_ms();
        uint64_t timeout_ms =
            (uint64_t)controller->request.duration_sec * 1000u;
        if (controller->request.protocol == WLH_IPERF_UDP &&
            controller->request.role == WLH_IPERF_SERVER) {
            if (udp_first_packet_ms != 0u && udp_client_duration_ms != 0u &&
                current - udp_first_packet_ms >= udp_client_duration_ms) {
                if (!send_udp_server_report(
                        controller,
                        data_fd,
                        &udp_peer,
                        udp_peer_size,
                        udp.bytes,
                        current - udp_first_packet_ms,
                        &udp,
                        false
                    )) {
                    detail = "failed to send UDP server report";
                    break;
                }
                success = true;
                break;
            }
            /* The listening interval is anchored at server start, not at the
             * first packet. Under loss/backpressure a packet can arrive much
             * later than the client began its test; extending the deadline
             * from that packet makes the iPerf2 server report miss the
             * client's finite end-packet retry window. */
            if (current - started >= timeout_ms) {
                if (udp_peer_size == 0u || udp_first_packet_ms == 0u) {
                    detail = "server wait timed out";
                    break;
                }
                if (!send_udp_server_report(
                        controller,
                        data_fd,
                        &udp_peer,
                        udp_peer_size,
                        udp.bytes,
                        current - udp_first_packet_ms,
                        &udp,
                        true
                    )) {
                    detail = "failed to send UDP server report";
                    break;
                }
                success = true;
                break;
            }
        } else if (current - started >= timeout_ms) {
            break;
        }
        if (controller->request.role == WLH_IPERF_CLIENT) {
            if (controller->request.protocol == WLH_IPERF_UDP) {
                uint64_t us = now_us();
                wlh_iperf2_udp_header_t header = {
                    (int32_t)udp.packets,
                    (uint32_t)(us / 1000000u),
                    (uint32_t)(us % 1000000u)
                };
                wlh_iperf2_udp_encode(payload, &header);
            }
            count = lwip_send(data_fd, payload, packet_size, 0);
            if (count < 0 && send_backpressured()) {
                ++send_backpressure_count;
                if (send_backpressure_count <= 3u ||
                    send_backpressure_count % 1000u == 0u) {
                    int socket_error = 0;
                    socklen_t socket_error_size = sizeof(socket_error);
                    (void)lwip_getsockopt(
                        data_fd,
                        SOL_SOCKET,
                        SO_ERROR,
                        &socket_error,
                        &socket_error_size
                    );
                    WLH_LOGW(
                        "iperf",
                        "send backpressured #%lu errno=%d so_error=%d",
                        (unsigned long)send_backpressure_count,
                        errno,
                        socket_error
                    );
                }
                sys_msleep(1u);
                continue;
            }
            if (count <= 0) {
                WLH_LOGW(
                    "iperf", "send failed count=%d errno=%d", count, errno
                );
                detail = "send failed";
                break;
            }
            bytes += (uint32_t)count;
            if (controller->request.protocol == WLH_IPERF_UDP) {
                ++udp.packets;
                uint64_t target_us =
                    (uint64_t)packet_size * 8u * 1000000u /
                    ((uint64_t)controller->request.target_mbps * 1000000u);
                if (target_us > 0u)
                    sys_msleep((u32_t)((target_us + 999u) / 1000u));
            }
        } else {
            struct sockaddr_in sender;
            socklen_t sender_size = sizeof(sender);
            if (controller->request.protocol == WLH_IPERF_UDP)
                count = lwip_recvfrom(
                    data_fd,
                    payload,
                    sizeof(payload),
                    0,
                    (struct sockaddr *)&sender,
                    &sender_size
                );
            else
                count = lwip_recv(data_fd, payload, sizeof(payload), 0);
            if (count == 0 && controller->request.protocol == WLH_IPERF_TCP) {
                success = true;
                break;
            }
            if (count < 0)
                continue;
            bytes += (uint32_t)count;
            if (controller->request.protocol == WLH_IPERF_UDP) {
                wlh_iperf2_udp_header_t header;
                if (!wlh_iperf2_udp_decode(payload, (uint32_t)count, &header)) {
                    detail = "invalid UDP iPerf2 packet";
                    break;
                }
                if (header.sequence < 0) {
                    uint64_t elapsed_ms =
                        udp_first_packet_ms == 0u
                            ? 0u
                            : wlh_app_monotonic_ms() - udp_first_packet_ms;
                    if (!send_udp_server_report(
                            controller,
                            data_fd,
                            &sender,
                            sender_size,
                            udp.bytes,
                            elapsed_ms,
                            &udp,
                            true
                        )) {
                        detail = "failed to send UDP server report";
                        break;
                    }
                    success = true;
                    break;
                }
                if (udp_first_packet_ms == 0u) {
                    udp_first_packet_ms = wlh_app_monotonic_ms();
                    (void)wlh_iperf2_udp_decode_client_duration_ms(
                        payload, (uint32_t)count, &udp_client_duration_ms
                    );
                }
                udp_peer = sender;
                udp_peer_size = sender_size;
                wlh_iperf2_udp_stats_add(
                    &udp, &header, (uint32_t)count, now_us()
                );
            }
        }
        if (wlh_app_monotonic_ms() - last_report >= IPERF_REPORT_MS) {
            uint64_t current = wlh_app_monotonic_ms();
            emit_interval(
                controller,
                bytes,
                bytes - last_bytes,
                current - started,
                current - last_report,
                controller->request.protocol == WLH_IPERF_UDP ? &udp : NULL
            );
            last_bytes = bytes;
            last_report = current;
        }
    }
    if (!cancelled(controller) &&
        controller->request.protocol == WLH_IPERF_UDP &&
        controller->request.role == WLH_IPERF_CLIENT && data_fd >= 0) {
        uint64_t us = now_us();
        wlh_iperf2_udp_header_t final = {
            -(int32_t)(udp.packets + 1u),
            (uint32_t)(us / 1000000u),
            (uint32_t)(us % 1000000u)
        };
        wlh_iperf2_udp_encode(payload, &final);
        {
            uint8_t report[WLH_IPERF2_DEFAULT_UDP_PACKET_SIZE];
            bool report_received = false;
            uint32_t attempt;
            for (attempt = 0u;
                 attempt < IPERF_UDP_ACK_ATTEMPTS && !cancelled(controller);
                 ++attempt) {
                int count = lwip_send(data_fd, payload, packet_size, 0);
                if (count != (int)packet_size) {
                    if (!send_backpressured())
                        break;
                    sys_msleep(1u);
                    continue;
                }
                int ready = wait_ready(data_fd, false, IPERF_UDP_ACK_WAIT_MS);
                if (ready < 0)
                    break;
                if (ready == 0)
                    continue;
                if (lwip_recv(data_fd, report, sizeof(report), 0) > 0) {
                    report_received = true;
                    break;
                }
            }
            if (!report_received && strcmp(detail, "completed") == 0)
                detail = "failed to receive UDP server report";
        }
    }
    if (cancelled(controller))
        detail = controller->cancel_detail;
    else if (strcmp(detail, "completed") == 0)
        success = bytes != 0u;
done:
    if (data_fd >= 0 && data_fd != fd)
        lwip_close(data_fd);
    if (fd >= 0)
        lwip_close(fd);
    set_socket(controller, -1);
    emit_result(
        controller,
        bytes,
        wlh_app_monotonic_ms() - started,
        success,
        detail,
        controller->request.protocol == WLH_IPERF_UDP ? &udp : NULL
    );
    pthread_mutex_lock(&controller->mutex);
    controller->running = false;
    pthread_mutex_unlock(&controller->mutex);
    return NULL;
}
wlh_iperf_controller_t *wlh_iperf_controller_create(app_t *app) {
    wlh_iperf_controller_t *controller = calloc(1u, sizeof(*controller));
    if (controller != NULL) {
        controller->app = app;
        controller->socket_fd = -1;
        (void)pthread_mutex_init(&controller->mutex, NULL);
    }
    return controller;
}
void wlh_iperf_cancel(wlh_iperf_controller_t *controller, const char *detail) {
    int fd;
    if (controller == NULL)
        return;
    atomic_store(&controller->cancel, true);
    pthread_mutex_lock(&controller->mutex);
    (void)snprintf(
        controller->cancel_detail,
        sizeof(controller->cancel_detail),
        "%s",
        detail
    );
    fd = controller->socket_fd;
    pthread_mutex_unlock(&controller->mutex);
    if (fd >= 0)
        (void)lwip_shutdown(fd, SHUT_RDWR);
}
void wlh_iperf_controller_destroy(wlh_iperf_controller_t *controller) {
    if (controller == NULL)
        return;
    wlh_iperf_cancel(controller, "REPL exited");
    if (controller->joinable)
        pthread_join(controller->worker, NULL);
    pthread_mutex_destroy(&controller->mutex);
    free(controller);
}
int wlh_iperf_start(
    wlh_iperf_controller_t *controller, const wlh_iperf_request_t *request
) {
    char local_address[16];
    if (controller == NULL || request == NULL || request->duration_sec == 0u ||
        request->duration_sec > IPERF_MAX_DURATION ||
        (request->protocol == WLH_IPERF_UDP &&
         (request->target_mbps == 0u ||
          request->target_mbps > IPERF_MAX_MBPS)) ||
        !sim_network_ipv4(controller->app->network, local_address))
        return -1;
    pthread_mutex_lock(&controller->mutex);
    if (controller->running) {
        pthread_mutex_unlock(&controller->mutex);
        return -2;
    }
    if (controller->joinable) {
        pthread_mutex_unlock(&controller->mutex);
        pthread_join(controller->worker, NULL);
        pthread_mutex_lock(&controller->mutex);
        controller->joinable = false;
    }
    controller->request = *request;
    controller->peer[0] = '\0';
    if (request->peer != NULL)
        (void)snprintf(
            controller->peer, sizeof(controller->peer), "%s", request->peer
        );
    controller->running = true;
    atomic_store(&controller->cancel, false);
    controller->cancel_detail[0] = '\0';
    if (pthread_create(&controller->worker, NULL, worker_main, controller) !=
        0) {
        controller->running = false;
        pthread_mutex_unlock(&controller->mutex);
        return -1;
    }
    controller->joinable = true;
    pthread_mutex_unlock(&controller->mutex);
    return 0;
}
