// netfs.c - /net filesystem: Plan 9-style network file interface over lwIP
//
// directory layout:
//   /net/tcp/clone          read → returns free slot number, allocates TCP PCB
//   /net/tcp/N/ctl          write: "connect ip!port" | "announce *!port" | "accept" | "close"
//   /net/tcp/N/data         read/write: TCP stream
//   /net/tcp/N/status       read: connection state string
//   /net/tcp/N/local        read: "ip!port"
//   /net/tcp/N/remote       read: "ip!port"
//   /net/udp/clone          read → returns free slot number, allocates UDP PCB
//   /net/udp/N/ctl          write: "connect ip!port" | "announce *!port" | "close"
//   /net/udp/N/data         read/write: UDP datagrams
//   /net/udp/N/status       read: state string
//   /net/udp/N/local        read: "ip!port"
//   /net/udp/N/remote       read: "ip!port"
//   /net/ipifc/0/status     read: "ip=... mask=... gw=... mac=..."
//   /net/ipifc/0/ctl        write: "add ip mask [gw]" for static config
//   /net/arp                read: ARP table placeholder

#include "netfs.h"
#include "vfs.h"
#include "ramfs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "proc.h"
#include "sched.h"

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include "virtio_net.h"

// constants
#define NETFS_TCP_MAX    8                      // max TCP connections
#define NETFS_UDP_MAX    8                      // max UDP connections
#define NETFS_RX_BUF     2048                   // recieve buffer size (per-conn)
#define NETFS_NO_WAITER  0xFFFFu                // sentinel
#define NETFS_ACCEPT_Q   4                      // size of listen()-style sockets queue

// connection state structure
typedef enum {
    CONN_FREE = 0,                              // slot unused
    CONN_ALLOCATED,                             // slot created (not connected)
    CONN_CONNECTING,                            // TCP handshake in progress
    CONN_ESTABLISHED,                           // connected
    CONN_LISTEN,                                // server socket
    CONN_CLOSING,                               // closedown in progress
} conn_state_t;

// TCP connection structure
typedef struct {
    conn_state_t    state;                      // kernel view
    struct tcp_pcb *pcb;                        // protocol control block (lwIP internal)

    uint8_t         rx_buf[NETFS_RX_BUF];       // RX buffer data
    uint32_t        rx_head;                    // RX buffer read position
    uint32_t        rx_count;                   // RX buffer bytes available

    uint16_t        blocked_reader;             // proc waiting in read()
    uint16_t        blocked_connector;          // proc waiting for connect() to finish
    uint16_t        blocked_acceptor;           // proc waiting in accept()

    // connection endpoints store
    ip4_addr_t      local_ip;
    ip4_addr_t      remote_ip;
    uint16_t        local_port;
    uint16_t        remote_port;

    struct tcp_pcb *accept_q[NETFS_ACCEPT_Q];   // incoming connections queue
    uint8_t         accept_head;                // queue read position
    uint8_t         accept_count;               // queue bytes available

    int8_t          conn_err;                   // error tracking
} netfs_tcp_t;

// UDP connection structure
typedef struct {
    conn_state_t    state;                      // kernel view
    struct tcp_pcb *pcb;                        // protocol control block (lwIP internal)

    uint8_t         rx_buf[NETFS_RX_BUF];       // RX buffer data
    uint32_t        rx_head;                    // RX buffer read position
    uint32_t        rx_count;                   // RX buffer bytes available

    uint16_t        blocked_reader;             // proc waiting in read()

    // connection endpoints store
    ip4_addr_t      local_ip;
    ip4_addr_t      remote_ip;
    uint16_t        local_port;
    uint16_t        remote_port;
} netfs_udp_t;

// global connection tables
static netfs_tcp_t tcp_conns[NETFS_TCP_MAX];
static netfs_udp_t udp_conns[NETFS_UDP_MAX];

// protocol identifiers
#define PROTO_TCP  0
#define PROTO_UDP  1

// file types
#define FTYPE_CLONE        0
#define FTYPE_CTL          1
#define FTYPE_DATA         2
#define FTYPE_STATUS       3
#define FTYPE_LOCAL        4
#define FTYPE_REMOTE       5
#define FTYPE_IPIFC_STATUS 10
#define FTYPE_IPIFC_CTL    11
#define FTYPE_ARP          12

// file structure 
typedef struct {
    uint8_t  proto;                             // UDP/TCP ?
    uint8_t  ftype;                             // file type
    uint32_t slot;                              // index in global connection table
} netfs_file_t;
