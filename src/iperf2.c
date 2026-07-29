#include "iperf2.h"

#include <arpa/inet.h>
#include <string.h>

bool wlh_iperf2_udp_decode(
    const uint8_t *data, uint32_t size, wlh_iperf2_udp_header_t *header
) {
    uint32_t value;
    if (data == NULL || header == NULL || size < WLH_IPERF2_UDP_HEADER_SIZE)
        return false;
    memcpy(&value, data, sizeof(value));
    header->sequence = (int32_t)ntohl(value);
    memcpy(&value, data + 4u, sizeof(value));
    header->seconds = ntohl(value);
    memcpy(&value, data + 8u, sizeof(value));
    header->microseconds = ntohl(value);
    return header->microseconds < 1000000u;
}

void wlh_iperf2_udp_encode(
    uint8_t data[WLH_IPERF2_UDP_HEADER_SIZE],
    const wlh_iperf2_udp_header_t *header
) {
    uint32_t value;
    value = htonl((uint32_t)header->sequence);
    memcpy(data, &value, sizeof(value));
    value = htonl(header->seconds);
    memcpy(data + 4u, &value, sizeof(value));
    value = htonl(header->microseconds);
    memcpy(data + 8u, &value, sizeof(value));
}

void wlh_iperf2_udp_stats_init(wlh_iperf2_udp_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->next_sequence = 0;
}

void wlh_iperf2_udp_stats_add(
    wlh_iperf2_udp_stats_t *stats,
    const wlh_iperf2_udp_header_t *header,
    uint32_t bytes,
    uint64_t arrival_us
) {
    double sent_ms = (double)header->seconds * 1000.0 +
                     (double)header->microseconds / 1000.0;
    double transit = (double)arrival_us / 1000.0 - sent_ms;
    if (header->sequence < 0)
        return;
    stats->packets++;
    stats->bytes += bytes;
    if (header->sequence == stats->next_sequence) {
        stats->next_sequence++;
    } else if (header->sequence > stats->next_sequence) {
        stats->lost += (uint64_t)(header->sequence - stats->next_sequence);
        stats->next_sequence = header->sequence + 1;
    } else {
        stats->out_of_order++;
    }
    if (stats->have_transit) {
        double delta = transit - stats->last_transit_ms;
        if (delta < 0.0)
            delta = -delta;
        stats->jitter_ms += (delta - stats->jitter_ms) / 16.0;
    } else {
        stats->have_transit = true;
    }
    stats->last_transit_ms = transit;
}

bool wlh_iperf2_udp_encode_server_report(
    uint8_t *data,
    uint32_t size,
    uint64_t bytes,
    uint64_t elapsed_ms,
    const wlh_iperf2_udp_stats_t *stats
) {
    uint32_t values[28] = {0};
    uint64_t elapsed_us;
    uint64_t jitter_us;
    size_t index;
    if (data == NULL || stats == NULL ||
        size < WLH_IPERF2_UDP_SERVER_REPORT_SIZE)
        return false;
    elapsed_us = elapsed_ms * 1000u;
    jitter_us = (uint64_t)(stats->jitter_ms * 1000.0);
    /* iPerf2 2.2.1's write_UDP_AckFIN() allocates a zeroed UDP_datagram,
     * then places server_hdr immediately after its four 32-bit words. */
    memset(data, 0, WLH_IPERF2_UDP_SERVER_UDP_HEADER_SIZE);
    values[0] = htonl(0x88000000u); /* HEADER_VERSION1 | HEADER_SEQNO64B */
    values[1] = htonl((uint32_t)(bytes >> 32u));
    values[2] = htonl((uint32_t)bytes);
    values[3] = htonl((uint32_t)(elapsed_us / 1000000u));
    values[4] = htonl((uint32_t)(elapsed_us % 1000000u));
    values[5] = htonl((uint32_t)stats->lost);
    values[6] = htonl((uint32_t)stats->out_of_order);
    values[7] = htonl((uint32_t)stats->packets);
    values[8] = htonl((uint32_t)(jitter_us / 1000000u));
    values[9] = htonl((uint32_t)(jitter_us % 1000000u));
    /* server_hdr_extension (indices 10..24) is zero because Host Sim does
     * not implement iPerf2 enhanced/trip-time transit statistics.  The
     * 64-bit counter extension follows it at indices 25..27. */
    values[25] = htonl((uint32_t)(stats->lost >> 32u));
    values[26] = htonl((uint32_t)(stats->out_of_order >> 32u));
    values[27] = htonl((uint32_t)(stats->packets >> 32u));
    for (index = 0; index < sizeof(values) / sizeof(values[0]); ++index)
        memcpy(
            data + WLH_IPERF2_UDP_SERVER_UDP_HEADER_SIZE +
                index * sizeof(uint32_t),
            &values[index],
            sizeof(uint32_t)
        );
    return true;
}
