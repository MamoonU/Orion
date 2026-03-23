// lwipopts.h - lwIP 2.1.3 configuration for Orion OS
// Bare-metal, single-core, no POSIX.

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// OS abstraction
#define NO_SYS                      1   // raw API, no threading
#define SYS_LIGHTWEIGHT_PROT        0   // single-core: no critical-section guards needed

// memory
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4               // x86 required
#define MEM_SIZE                    (256 * 1024)    // lwIP internel heap = 256KB

// packet buffer pool
#define PBUF_POOL_SIZE              32      // enabled: number of packets pool can hold
#define PBUF_POOL_BUFSIZE           1536    // enabled: bytes per entry (max ethernet frame)

// protocol support
#define LWIP_ARP                    1       // enabled: MAC <-> IP resolution
#define LWIP_ETHERNET               1
#define LWIP_IPV4                   1       // enabled: routing
#define LWIP_IPV6                   0       // disabled: not needed until /net filesystem
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1       // enabled: DNS
#define LWIP_TCP                    1       // enabled: HTTP, sockets
#define LWIP_DHCP                   1       // enabled: automatic IP
#define LWIP_DNS                    1       // enabled: name resolution
#define LWIP_IGMP                   0       // disabled:
#define LWIP_AUTOIP                 0       // disabled:

// TCP
#define TCP_MSS                     1460            // max segment size: 1460 = 1500 (MTU) - 20 (IP) - 20 (TCP)
#define TCP_WND                     (4 * TCP_MSS)   // recieve window: ~6KB
#define TCP_SND_BUF                 (4 * TCP_MSS)   // send buffer: ~6KB
#define TCP_SND_QUEUELEN            8               // max queued segments: 8
#define TCP_QUEUE_OOSEQ             1               // out of sequence

// ARP
#define ARP_TABLE_SIZE              10              // store IP -> MAC mappings (size = 10 entries)
#define ARP_QUEUEING                1               // if no MAC, queue packets until ARP resolves

// DHCP
#define DHCP_DOES_ARP_CHECK         0               // disable to skip ARP check on leased address

// DNS
#define DNS_MAX_SERVERS             2               // 2 DNS servers
#define DNS_TABLE_SIZE              8               // 8 entries per server

// network interface
#define LWIP_NETIF_STATUS_CALLBACK  1               // status changes (up/down)
#define LWIP_NETIF_LINK_CALLBACK    1               // link changes
#define LWIP_NETIF_HOSTNAME         1               // hostname support
#define LWIP_LOOPBACK_MAX_PBUFS     0

// checksums
#define CHECKSUM_GEN_IP             1               // IP generation
#define CHECKSUM_GEN_UDP            1               // UDP generation
#define CHECKSUM_GEN_TCP            1               // TCP generation
#define CHECKSUM_CHECK_IP           1               // IP
#define CHECKSUM_CHECK_UDP          1               // UDP
#define CHECKSUM_CHECK_TCP          1               // TCP

// stats / debug
#define LWIP_STATS                  0               // saves ~4KB of BSS
#define LWIP_STATS_DISPLAY          0
#define LWIP_DEBUG                  0

// errno
#define LWIP_PROVIDE_ERRNO          1               // lwIP defines its own errno

// socket layer
#define LWIP_POSIX_SOCKETS_IO_NAMES 0               // disabled

#endif