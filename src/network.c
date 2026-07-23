#include "network.h"

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/raw.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include "lwip_port/sys_arch.h"
#include "netif/ethernet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PING_IDENTIFIER 0x574cu
#define PING_INTERVAL_MS 1000u
#define DHCP_POLL_MS 100u
#define PING_PAYLOAD_SIZE 32u

typedef struct ping_request {
    struct sim_network *network;
    uint32_t request_id;
    uint32_t count;
    uint32_t timeout_ms;
    char hostname[254];
} ping_request_t;

struct sim_network {
    struct netif netif;
    sim_network_send_fn send;
    sim_ping_result_fn report;
    void *context;
    bool active;
    uint32_t request_id;
    uint32_t count;
    uint32_t transmitted;
    uint32_t received;
    uint32_t received_mask;
    uint32_t deadline_ms;
    char hostname[254];
    char address[47];
    ip_addr_t target;
    struct raw_pcb *ping_pcb;
};

typedef struct link_request {
    sim_network_t *network;
    uint8_t mac[6];
    bool up;
} link_request_t;

static void ping_wait_for_dhcp(void *argument);
static void ping_tick(void *argument);
static void ping_deadline(void *argument);

static bool deadline_expired(const sim_network_t *network) {
    return (int32_t)(sys_now() - network->deadline_ms) >= 0;
}

static void finish_ping(sim_network_t *network, const char *detail) {
    sim_ping_result_t result;
    if (!network->active)
        return;
    sys_untimeout(ping_wait_for_dhcp, network);
    sys_untimeout(ping_tick, network);
    sys_untimeout(ping_deadline, network);
    if (network->ping_pcb != NULL) {
        raw_remove(network->ping_pcb);
        network->ping_pcb = NULL;
    }
    memset(&result, 0, sizeof(result));
    result.request_id = network->request_id;
    result.transmitted = network->transmitted;
    result.received = network->received;
    result.success = network->received != 0u;
    (void)snprintf(
        result.hostname, sizeof(result.hostname), "%s", network->hostname
    );
    (void)snprintf(
        result.address, sizeof(result.address), "%s", network->address
    );
    (void)snprintf(result.detail, sizeof(result.detail), "%s", detail);
    network->active = false;
    network->report(network->context, &result);
}

static err_t link_output(struct netif *netif, struct pbuf *pbuf) {
    sim_network_t *network = netif->state;
    uint8_t frame[1518];
    if (pbuf->tot_len > sizeof(frame) ||
        pbuf_copy_partial(pbuf, frame, pbuf->tot_len, 0u) != pbuf->tot_len)
        return ERR_BUF;
    return network->send(network->context, frame, pbuf->tot_len) == 0 ? ERR_OK
                                                                      : ERR_IF;
}

static err_t initialize_netif(struct netif *netif) {
    netif->name[0] = 'w';
    netif->name[1] = 'h';
    netif->output = etharp_output;
    netif->linkoutput = link_output;
    netif->mtu = 1500u;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    return ERR_OK;
}

static void configure_netif(void *argument) {
    sim_network_t *network = argument;
    ip4_addr_t any;
    ip4_addr_set_zero(&any);
    if (netif_add(
            &network->netif,
            &any,
            &any,
            &any,
            network,
            initialize_netif,
            tcpip_input
        ) == NULL)
        LWIP_PLATFORM_ASSERT("netif_add");
    netif_set_default(&network->netif);
    netif_set_down(&network->netif);
    netif_set_link_down(&network->netif);
}

static void tcpip_ready(void *argument) {
    sys_sem_t *ready = argument;
    sys_sem_signal(ready);
}

sim_network_t *sim_network_create(
    const wlh_osal_ops_t *osal,
    sim_network_send_fn send,
    sim_ping_result_fn report,
    void *context
) {
    sim_network_t *network;
    sys_sem_t ready;
    if (!wlh_osal_ops_valid(osal) || send == NULL || report == NULL)
        return NULL;
    network = calloc(1u, sizeof(*network));
    if (network == NULL)
        return NULL;
    network->send = send;
    network->report = report;
    network->context = context;
    wlh_lwip_sys_arch_configure(osal);
    if (sys_sem_new(&ready, 0u) != ERR_OK) {
        free(network);
        return NULL;
    }
    tcpip_init(tcpip_ready, &ready);
    (void)sys_arch_sem_wait(&ready, 0u);
    sys_sem_free(&ready);
    if (tcpip_callback_wait(configure_netif, network) != ERR_OK) {
        free(network);
        return NULL;
    }
    return network;
}

static void destroy_netif(void *argument) {
    sim_network_t *network = argument;
    if (network->active)
        finish_ping(network, "network stopped");
    dhcp_stop(&network->netif);
    netif_remove(&network->netif);
}

void sim_network_destroy(sim_network_t *network) {
    if (network == NULL)
        return;
    (void)tcpip_callback_wait(destroy_netif, network);
    free(network);
}

static void apply_link(void *argument) {
    link_request_t *request = argument;
    sim_network_t *network = request->network;
    if (request->up) {
        memcpy(
            network->netif.hwaddr, request->mac, sizeof(network->netif.hwaddr)
        );
        network->netif.hwaddr_len = 6u;
        netif_set_link_up(&network->netif);
        netif_set_up(&network->netif);
        if (dhcp_start(&network->netif) != ERR_OK)
            LWIP_PLATFORM_ASSERT("dhcp_start");
    } else {
        if (network->active)
            finish_ping(network, "Wi-Fi link disconnected");
        dhcp_stop(&network->netif);
        netif_set_down(&network->netif);
        netif_set_link_down(&network->netif);
        {
            ip4_addr_t any;
            ip4_addr_set_zero(&any);
            netif_set_addr(&network->netif, &any, &any, &any);
        }
    }
}

int sim_network_link_up(sim_network_t *network, const uint8_t mac[6]) {
    link_request_t request;
    if (network == NULL || mac == NULL)
        return -1;
    request.network = network;
    request.up = true;
    memcpy(request.mac, mac, sizeof(request.mac));
    return tcpip_callback_wait(apply_link, &request) == ERR_OK ? 0 : -1;
}

void sim_network_link_down(sim_network_t *network) {
    link_request_t request;
    if (network == NULL)
        return;
    memset(&request, 0, sizeof(request));
    request.network = network;
    (void)tcpip_callback_wait(apply_link, &request);
}

int sim_network_input(
    sim_network_t *network, const uint8_t *frame, size_t size
) {
    struct pbuf *pbuf;
    err_t result;
    if (network == NULL || frame == NULL || size < 14u || size > 1518u)
        return -1;
    pbuf = pbuf_alloc(PBUF_RAW, (u16_t)size, PBUF_POOL);
    if (pbuf == NULL || pbuf_take(pbuf, frame, size) != ERR_OK) {
        if (pbuf != NULL)
            pbuf_free(pbuf);
        return -1;
    }
    result = network->netif.input(pbuf, &network->netif);
    if (result != ERR_OK)
        pbuf_free(pbuf);
    return result == ERR_OK ? 0 : -1;
}

static void prepare_echo(
    struct icmp_echo_hdr *echo, uint16_t size, uint16_t sequence
) {
    size_t index;
    ICMPH_TYPE_SET(echo, ICMP_ECHO);
    ICMPH_CODE_SET(echo, 0u);
    echo->chksum = 0u;
    echo->id = PING_IDENTIFIER;
    echo->seqno = lwip_htons(sequence);
    for (index = sizeof(*echo); index < size; ++index)
        ((uint8_t *)echo)[index] = (uint8_t)index;
    echo->chksum = inet_chksum(echo, size);
}

static void send_echo(sim_network_t *network) {
    const uint16_t size = sizeof(struct icmp_echo_hdr) + PING_PAYLOAD_SIZE;
    struct pbuf *pbuf = pbuf_alloc(PBUF_IP, size, PBUF_RAM);
    uint16_t sequence = (uint16_t)(network->transmitted + 1u);
    if (pbuf == NULL || pbuf->len != pbuf->tot_len) {
        if (pbuf != NULL)
            pbuf_free(pbuf);
        finish_ping(network, "ICMP packet allocation failed");
        return;
    }
    prepare_echo(pbuf->payload, size, sequence);
    if (raw_sendto(network->ping_pcb, pbuf, &network->target) != ERR_OK) {
        pbuf_free(pbuf);
        finish_ping(network, "ICMP send failed");
        return;
    }
    pbuf_free(pbuf);
    network->transmitted++;
}

static u8_t receive_echo(
    void *argument,
    struct raw_pcb *pcb,
    struct pbuf *pbuf,
    const ip_addr_t *address
) {
    sim_network_t *network = argument;
    struct ip_hdr ip_header;
    struct icmp_echo_hdr echo;
    uint16_t header_size;
    uint16_t sequence;
    (void)pcb;
    (void)address;
    if (!network->active ||
        pbuf_copy_partial(pbuf, &ip_header, sizeof(ip_header), 0u) !=
            sizeof(ip_header))
        return 0u;
    header_size = (uint16_t)(IPH_HL(&ip_header) * 4u);
    if (header_size < IP_HLEN ||
        pbuf_copy_partial(pbuf, &echo, sizeof(echo), header_size) !=
            sizeof(echo) ||
        ICMPH_TYPE(&echo) != ICMP_ER || echo.id != PING_IDENTIFIER)
        return 0u;
    sequence = lwip_ntohs(echo.seqno);
    if (sequence == 0u || sequence > network->count)
        return 0u;
    if ((network->received_mask & (1u << (sequence - 1u))) == 0u) {
        network->received_mask |= 1u << (sequence - 1u);
        network->received++;
    }
    pbuf_free(pbuf);
    if (network->received == network->count)
        finish_ping(network, "all ICMP echo replies received");
    return 1u;
}

static void ping_tick(void *argument) {
    sim_network_t *network = argument;
    if (!network->active)
        return;
    if (deadline_expired(network)) {
        finish_ping(
            network,
            network->received != 0u ? "ICMP probing completed"
                                    : "ICMP echo timed out"
        );
        return;
    }
    if (network->transmitted < network->count) {
        send_echo(network);
        if (!network->active)
            return;
        sys_timeout(PING_INTERVAL_MS, ping_tick, network);
        return;
    }
    finish_ping(
        network,
        network->received != 0u ? "ICMP probing completed"
                                : "ICMP echo timed out"
    );
}

static void ping_deadline(void *argument) {
    sim_network_t *network = argument;
    if (network->active)
        finish_ping(network, "ping operation timed out");
}

static void begin_ping(sim_network_t *network) {
    network->ping_pcb = raw_new(IP_PROTO_ICMP);
    if (network->ping_pcb == NULL) {
        finish_ping(network, "unable to allocate ICMP control block");
        return;
    }
    raw_recv(network->ping_pcb, receive_echo, network);
    raw_bind(network->ping_pcb, IP_ADDR_ANY);
    send_echo(network);
    if (network->active)
        sys_timeout(PING_INTERVAL_MS, ping_tick, network);
}

static void dns_resolved(
    const char *hostname, const ip_addr_t *address, void *argument
) {
    sim_network_t *network = argument;
    (void)hostname;
    if (!network->active)
        return;
    if (address == NULL || !IP_IS_V4(address)) {
        finish_ping(network, "DNS resolution failed");
        return;
    }
    network->target = *address;
    (void)ipaddr_ntoa_r(
        address, network->address, (int)sizeof(network->address)
    );
    begin_ping(network);
}

static void resolve_target(sim_network_t *network) {
    ip_addr_t address;
    err_t result =
        dns_gethostbyname(network->hostname, &address, dns_resolved, network);
    if (result == ERR_OK)
        dns_resolved(network->hostname, &address, network);
    else if (result != ERR_INPROGRESS)
        finish_ping(network, "DNS request rejected");
}

static void ping_wait_for_dhcp(void *argument) {
    sim_network_t *network = argument;
    if (!network->active)
        return;
    if (deadline_expired(network)) {
        finish_ping(network, "DHCP address acquisition timed out");
        return;
    }
    if (!ip4_addr_isany_val(*netif_ip4_addr(&network->netif))) {
        resolve_target(network);
        return;
    }
    sys_timeout(DHCP_POLL_MS, ping_wait_for_dhcp, network);
}

static void start_ping(void *argument) {
    ping_request_t *request = argument;
    sim_network_t *network = request->network;
    if (network->active) {
        sim_ping_result_t result;
        memset(&result, 0, sizeof(result));
        result.request_id = request->request_id;
        (void)snprintf(
            result.hostname, sizeof(result.hostname), "%s", request->hostname
        );
        (void)snprintf(
            result.detail, sizeof(result.detail), "%s", "ping already active"
        );
        network->report(network->context, &result);
        free(request);
        return;
    }
    network->active = true;
    network->request_id = request->request_id;
    network->count = request->count;
    network->transmitted = 0u;
    network->received = 0u;
    network->received_mask = 0u;
    network->deadline_ms = sys_now() + request->timeout_ms;
    sys_timeout(request->timeout_ms, ping_deadline, network);
    network->address[0] = '\0';
    (void)snprintf(
        network->hostname, sizeof(network->hostname), "%s", request->hostname
    );
    free(request);
    if (!netif_is_up(&network->netif) || !netif_is_link_up(&network->netif)) {
        finish_ping(network, "Wi-Fi link is down");
        return;
    }
    ping_wait_for_dhcp(network);
}

int sim_network_ping(
    sim_network_t *network,
    uint32_t request_id,
    const char *hostname,
    uint32_t count,
    uint32_t timeout_ms
) {
    ping_request_t *request;
    size_t hostname_size;
    if (network == NULL || request_id == 0u || hostname == NULL ||
        count == 0u || count > 10u || timeout_ms == 0u || timeout_ms > 60000u)
        return -1;
    hostname_size = strlen(hostname);
    if (hostname_size == 0u || hostname_size > 253u)
        return -1;
    request = calloc(1u, sizeof(*request));
    if (request == NULL)
        return -1;
    request->network = network;
    request->request_id = request_id;
    request->count = count;
    request->timeout_ms = timeout_ms;
    memcpy(request->hostname, hostname, hostname_size + 1u);
    if (tcpip_callback(start_ping, request) != ERR_OK) {
        free(request);
        return -1;
    }
    return 0;
}
