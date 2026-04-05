// orion_netif.c - lwIP network interface glue

#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "lwip/init.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

#include "virtio_net.h"
#include "lwip_orion.h"
#include "kprintf.h"
#include "string.h"
#include "timer.h"

// global driver state
static struct netif orion_netif;            // the single virtio-net network interface

// TX path
static err_t orion_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;

    static uint8_t tx_flat[1536];                                               // flatten pbuf chain -> one contiguous buffer
    uint16_t total = 0;

    for (struct pbuf *q = p; q != NULL; q = q->next) {                          // walk chain
        if ((uint32_t)total + q->len > sizeof(tx_flat)) {                       // overflow protection
            kprintf("LWIP: TX frame too large (%u B) - dropped\n", (uint32_t)total + q->len);
            return ERR_MEM;
        }
        memcpy(tx_flat + total, q->payload, q->len);                            // copy each chunk -> tx_flat
        total = (uint16_t)(total + q->len);
    }
    return (virtio_net_send(tx_flat, (uint32_t)total) == 0) ? ERR_OK : ERR_IF;  // send packet to NIC
}

// network interface initialise
static err_t orion_netif_init(struct netif *netif) {

    uint8_t mac[6];
    virtio_net_get_mac(mac);                                                            // set mac address

    netif->hwaddr_len = ETHARP_HWADDR_LEN;
    for (int i = 0; i < 6; i++) {
        netif->hwaddr[i] = mac[i];
    }

    netif->mtu        = 1500;                                                           // set standard ethernet MTU 
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;  // set flags (supports broadcast, uses ARP, link is up)

    netif->linkoutput = orion_linkoutput;   // raw frame → virtio
    netif->output     = etharp_output;      // IP → ARP resolution → linkoutput

    #if LWIP_NETIF_HOSTNAME                 // used by DHCP
        netif->hostname   = "orion";
    #endif

    kprintf("LWIP: netif MAC %02x:%02x:%02x:%02x:%02x:%02x  MTU %u\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (uint32_t)netif->mtu);
    return ERR_OK;
}

// RX path
static void orion_rx(const void *data, uint32_t len) {

    if (len < 14 || len > 1514) return;                                     // drop malformed frames silently

    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);           // alloc packet buffer
    if (!p) {
        kprintf("LWIP: RX pbuf_alloc failed (len=%u) - dropped\n", len);
        return;
    }

    const uint8_t *src  = (const uint8_t *)data;
    uint32_t       done = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {                      // copy data -> packet buffer chain
        memcpy(q->payload, src + done, q->len);
        done += q->len;
    }

    if (orion_netif.input(p, &orion_netif) != ERR_OK) {                     // ethernet_input() handles ARP and routes IP packets up stack
        kprintf("LWIP: netif.input error - dropping frame\n");
        pbuf_free(p);
    }
}

// status callback
#if LWIP_NETIF_STATUS_CALLBACK
static void orion_status_cb(struct netif *netif) {
    if (netif_is_up(netif)) {
        kprintf("LWIP: IP  %u.%u.%u.%u\n",
                ip4_addr1_16_val(*netif_ip4_addr(netif)),
                ip4_addr2_16_val(*netif_ip4_addr(netif)),
                ip4_addr3_16_val(*netif_ip4_addr(netif)),
                ip4_addr4_16_val(*netif_ip4_addr(netif)));
        kprintf("LWIP: GW  %u.%u.%u.%u\n",
                ip4_addr1_16_val(*netif_ip4_gw(netif)),
                ip4_addr2_16_val(*netif_ip4_gw(netif)),
                ip4_addr3_16_val(*netif_ip4_gw(netif)),
                ip4_addr4_16_val(*netif_ip4_gw(netif)));
    } else {
        kprintf("LWIP: netif down\n");
    }
}
#endif

#define DHCP_TIMEOUT_TICKS  500   // 5 seconds at 100Hz

static uint32_t dhcp_start_tick = 0;

// drives DHCP, TCP retransmit, ARP expiry, etc. (called on every timer tick)
void lwip_orion_poll(void) {
    sys_check_timeouts();

    // if DHCP hasn't assigned an IP after 5 seconds, give up
    if (dhcp_start_tick && ip4_addr_isany(netif_ip4_addr(&orion_netif))) {
        if (timer_get_ticks() - dhcp_start_tick > DHCP_TIMEOUT_TICKS) {
            dhcp_stop(&orion_netif);
            dhcp_start_tick = 0;
            kprintf("LWIP: DHCP timed out — configure via /net/ipifc/0/ctl\n");
        }
    }
}

// full initialisation
void lwip_orion_init(void) {

    ip4_addr_t ip, mask, gw;
    ip4_addr_set_zero(&ip);
    ip4_addr_set_zero(&mask);
    ip4_addr_set_zero(&gw);

    lwip_init();                    // initialise all lwIP subsystems

    // add network interface
    struct netif *r = netif_add(&orion_netif, &ip, &mask, &gw, NULL, orion_netif_init, ethernet_input);    // input fn: ARP + IP dispatch
    if (!r) {
        kprintf("LWIP: netif_add failed\n");
        return;
    }

    // set callbacks
    #if LWIP_NETIF_STATUS_CALLBACK
        netif_set_status_callback(&orion_netif, orion_status_cb);
    #endif

    netif_set_default(&orion_netif);
    netif_set_up(&orion_netif);             // bring interface up
    netif_set_link_up(&orion_netif);

    virtio_net_set_rx_callback(orion_rx);   // connect RX IRQ path

    dhcp_start(&orion_netif);               // request an IP from the router
    dhcp_start_tick = timer_get_ticks();

    kprintf("LWIP: stack ready - DHCP started\n");
}
