#include "iperf2.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t bytes[WLH_IPERF2_UDP_HEADER_SIZE];
    wlh_iperf2_udp_header_t header = {3, 17u, 250000u};
    wlh_iperf2_udp_header_t decoded;
    wlh_iperf2_udp_stats_t stats;
    wlh_iperf2_udp_encode(bytes, &header);
    assert(wlh_iperf2_udp_decode(bytes, sizeof(bytes), &decoded));
    assert(
        decoded.sequence == 3 && decoded.seconds == 17u &&
        decoded.microseconds == 250000u
    );
    assert(!wlh_iperf2_udp_decode(bytes, sizeof(bytes) - 1u, &decoded));
    header.microseconds = 1000000u;
    wlh_iperf2_udp_encode(bytes, &header);
    assert(!wlh_iperf2_udp_decode(bytes, sizeof(bytes), &decoded));
    {
        uint8_t client_header[40] = {0};
        uint32_t value = htonl(0x48010000u);
        uint32_t duration_ms = 0u;
        memcpy(client_header + 16u, &value, sizeof(value));
        value = htonl((uint32_t)-1000);
        memcpy(client_header + 36u, &value, sizeof(value));
        assert(wlh_iperf2_udp_decode_client_duration_ms(
            client_header, sizeof(client_header), &duration_ms
        ));
        assert(duration_ms == 10000u);
        assert(!wlh_iperf2_udp_decode_client_duration_ms(
            client_header, sizeof(client_header) - 1u, &duration_ms
        ));
        value = htonl(1000u);
        memcpy(client_header + 36u, &value, sizeof(value));
        assert(!wlh_iperf2_udp_decode_client_duration_ms(
            client_header, sizeof(client_header), &duration_ms
        ));
    }
    wlh_iperf2_udp_stats_init(&stats);
    header = (wlh_iperf2_udp_header_t){0, 1u, 0u};
    wlh_iperf2_udp_stats_add(&stats, &header, 100u, 1010000u);
    header.sequence = 2;
    wlh_iperf2_udp_stats_add(&stats, &header, 100u, 1020000u);
    header.sequence = 1;
    wlh_iperf2_udp_stats_add(&stats, &header, 100u, 1030000u);
    assert(
        stats.packets == 3u && stats.bytes == 300u && stats.lost == 1u &&
        stats.out_of_order == 1u && stats.jitter_ms >= 0.0
    );
    {
        uint8_t report[WLH_IPERF2_DEFAULT_UDP_PACKET_SIZE];
        uint32_t value;
        const uint32_t expected[] = {
            0x88000000u,
            1u,
            100u,
            2u,
            500000u,
            1u,
            1u,
            2u,
            0u,
            (uint32_t)((uint64_t)(stats.jitter_ms * 1000.0) % 1000000u),
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            0u,
            1u,
            2u,
            4u
        };
        size_t index;
        stats.packets = 0x300000001u;
        stats.lost = 0x100000001u;
        stats.out_of_order = 0x200000001u;
        memset(report, 0xa5, sizeof(report));
        assert(wlh_iperf2_udp_encode_server_report(
            report, sizeof(report), 0x100000064u, 2500u, &stats
        ));
        for (index = 0u; index < WLH_IPERF2_UDP_SERVER_UDP_HEADER_SIZE; ++index)
            assert(report[index] == 0u);
        for (index = 0u; index < sizeof(expected) / sizeof(expected[0]);
             ++index) {
            memcpy(
                &value,
                report + WLH_IPERF2_UDP_SERVER_UDP_HEADER_SIZE +
                    index * sizeof(value),
                sizeof(value)
            );
            assert(ntohl(value) == expected[index]);
        }
        /* The server sends a report with the original UDP datagram length.
         * The report itself changes only its 128-byte protocol prefix; keeping
         * the remainder intact matches iPerf2's mBufLen write behavior. */
        assert(report[WLH_IPERF2_UDP_SERVER_REPORT_SIZE] == 0xa5u);
        assert(!wlh_iperf2_udp_encode_server_report(
            report, WLH_IPERF2_UDP_SERVER_REPORT_SIZE - 1u, 0u, 0u, &stats
        ));
    }
    puts("iperf2 tests passed");
    return 0;
}
