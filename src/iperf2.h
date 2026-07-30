#ifndef WLH_HOST_SIM_IPERF2_H
#define WLH_HOST_SIM_IPERF2_H

#include <stdbool.h>
#include <stdint.h>

/* iPerf2 UDP packet header: signed sequence then send time, all network byte
 * order.  A negative sequence denotes the final datagram. */
#define WLH_IPERF2_UDP_HEADER_SIZE 12u
/* iPerf2 2.2.x server reports use the 64-bit-sequence UDP header, even
 * though ordinary data packets remain compatible with the 12-byte v1 header
 * parsed above. */
#define WLH_IPERF2_UDP_SERVER_UDP_HEADER_SIZE 16u
#define WLH_IPERF2_UDP_SERVER_HEADER_SIZE 112u
#define WLH_IPERF2_UDP_SERVER_REPORT_SIZE                                      \
    (WLH_IPERF2_UDP_SERVER_UDP_HEADER_SIZE + WLH_IPERF2_UDP_SERVER_HEADER_SIZE)
/* iPerf2's default UDP datagram length.  Together with IPv4 and UDP headers,
 * this produces a 1512-byte Ethernet payload, below the standard 1518-byte
 * Ethernet frame limit. */
#define WLH_IPERF2_DEFAULT_UDP_PACKET_SIZE 1470u
typedef struct wlh_iperf2_udp_header {
    int32_t sequence;
    uint32_t seconds;
    uint32_t microseconds;
} wlh_iperf2_udp_header_t;

typedef struct wlh_iperf2_udp_stats {
    uint64_t packets;
    uint64_t lost;
    uint64_t out_of_order;
    uint64_t bytes;
    int32_t next_sequence;
    double jitter_ms;
    double last_transit_ms;
    bool have_transit;
} wlh_iperf2_udp_stats_t;

bool wlh_iperf2_udp_decode(
    const uint8_t *data, uint32_t size, wlh_iperf2_udp_header_t *header
);
/* Extracts a time-mode duration from the standard client_udp_testhdr carried
 * in iPerf2 UDP payloads. */
bool wlh_iperf2_udp_decode_client_duration_ms(
    const uint8_t *data, uint32_t size, uint32_t *duration_ms
);
void wlh_iperf2_udp_encode(
    uint8_t data[WLH_IPERF2_UDP_HEADER_SIZE],
    const wlh_iperf2_udp_header_t *header
);
void wlh_iperf2_udp_stats_init(wlh_iperf2_udp_stats_t *stats);
void wlh_iperf2_udp_stats_add(
    wlh_iperf2_udp_stats_t *stats,
    const wlh_iperf2_udp_header_t *header,
    uint32_t bytes,
    uint64_t arrival_us
);
/* Writes the iPerf2 2.2.x UDP server report (16-byte UDP_datagram followed
 * by the 112-byte server_hdr). */
bool wlh_iperf2_udp_encode_server_report(
    uint8_t *data,
    uint32_t size,
    uint64_t bytes,
    uint64_t elapsed_ms,
    const wlh_iperf2_udp_stats_t *stats
);

#endif
