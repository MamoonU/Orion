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

// append src -> dst
static char *kstrcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++));
    return dst;
}

// ascii -> integer
static int katoi(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n;
}

// int -> str
static void uint_to_str(uint32_t v, char *buf, uint32_t bufsz) {
    if (!bufsz) return;
    if (!v) { if (bufsz > 1) { buf[0]='0'; buf[1]='\0'; } return; }
    char tmp[12]; int i = 11; tmp[11] = '\0';
    while (v && i > 0) { tmp[--i] = (char)('0' + v % 10); v /= 10; }
    strncpy(buf, &tmp[i], bufsz - 1);
    buf[bufsz - 1] = '\0';
}

// trim trailing newline/CR/space
static void trim_ws(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' ')) s[--n]='\0';
}

// parse "ip!port" or "*!port" → ip4_addr_t + u16 port
static int parse_addr_port(const char *s, ip4_addr_t *ip, uint16_t *port) {
    const char *bang = s;
    while (*bang && *bang != '!') bang++;
    if (!*bang) return -1;

    char ip_s[32];
    uint32_t ilen = (uint32_t)(bang - s);
    if (ilen >= sizeof(ip_s)) return -1;
    memcpy(ip_s, s, ilen); ip_s[ilen] = '\0';

    if (strcmp(ip_s, "*") == 0 || strcmp(ip_s, "0.0.0.0") == 0)
        ip4_addr_set_any(ip);
    else if (!ip4addr_aton(ip_s, ip))
        return -1;

    *port = (uint16_t)katoi(bang + 1);
    return 0;
}

// write "a.b.c.d!port\n" into buf
static uint32_t format_ap(const ip4_addr_t *ip, uint16_t port, char *buf, uint32_t bufsz) {
    char ip_s[20];
    ip4addr_ntoa_r(ip, ip_s, sizeof(ip_s));
    strncpy(buf, ip_s, bufsz - 1); buf[bufsz - 1] = '\0';
    uint32_t l = (uint32_t)strlen(buf);
    if (l + 1 < bufsz) { buf[l++] = '!'; buf[l] = '\0'; }
    char port_s[8]; uint_to_str(port, port_s, sizeof(port_s));
    if (l + strlen(port_s) + 2 < bufsz) {
        kstrcat(buf, port_s);
        kstrcat(buf, "\n");
    }
    return (uint32_t)strlen(buf);
}

// wake blocked process
static void wake_pid(uint16_t *slot) {
    uint16_t pid = *slot;
    if (pid == NETFS_NO_WAITER) return;
    *slot = NETFS_NO_WAITER;
    pcb_t *p = proc_get((pid_t)pid);
    if (p) proc_wake(p);
}

// sleep current process
static void block_self(uint16_t *slot) {
    pcb_t *self = sched_current();
    if (!self) return;
    *slot = (uint16_t)self->pid;
    self->state = PROC_BLOCKED;
    sched_remove(self);
    sched_yield();
}

// connect() completed
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {

    netfs_tcp_t *c = (netfs_tcp_t *)arg;            // lwIP passed args

    if (err == ERR_OK) {                            // success case
        c->state      = CONN_ESTABLISHED;
        c->local_port = pcb->local_port;
        c->local_ip   = pcb->local_ip;
    } else {                                        // failure case
        c->state    = CONN_FREE;
        c->conn_err = (int8_t)err;
    }

    wake_pid(&c->blocked_connector);                // wake waiting process
    return ERR_OK;
}

// data arrived
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {

    netfs_tcp_t *c = (netfs_tcp_t *)arg;
    (void)err;

    if (!p) {                                                               // remote closed connection
        c->state = CONN_CLOSING;
        wake_pid(&c->blocked_reader);
        return ERR_OK;
    }

    for (struct pbuf *q = p; q; q = q->next) {                              // for each chunk
        const uint8_t *src = (const uint8_t *)q->payload;                   // copy data -> buffer
        for (u16_t i = 0; i < q->len; i++) {
            if (c->rx_count < NETFS_RX_BUF) {                               // if space available
                uint32_t tail = (c->rx_head + c->rx_count) % NETFS_RX_BUF;  // compute tail
                c->rx_buf[tail] = src[i];                                   // store byte
                c->rx_count++;
            }                                                               // silently drop bytes if buffer full
        }
    }
    tcp_recved(pcb, p->tot_len);                                            // tell lwIP -> consumed data
    pbuf_free(p);                                                           // free lwIP buffer
    wake_pid(&c->blocked_reader);                                           // wake readers
    return ERR_OK;
}

// fatal error
static void tcp_err_cb(void *arg, err_t err) {

    netfs_tcp_t *c = (netfs_tcp_t *)arg;
    (void)err;

    c->state = CONN_FREE;                   // mark connection dead
    c->pcb   = 0;

    wake_pid(&c->blocked_connector);        // wake everyone
    wake_pid(&c->blocked_reader);
    wake_pid(&c->blocked_acceptor);
}

// new incoming connection
static err_t tcp_accept_cb(void *arg, struct tcp_pcb *new_pcb, err_t err) {

    netfs_tcp_t *c = (netfs_tcp_t *)arg;
    if (err != ERR_OK || !new_pcb) return ERR_VAL;

    if (c->accept_count >= NETFS_ACCEPT_Q) {            // queue full check
        tcp_abort(new_pcb); return ERR_ABRT;
    }

    uint8_t tail = (uint8_t)((c->accept_head + c->accept_count) & (NETFS_ACCEPT_Q - 1));

    c->accept_q[tail] = new_pcb;                        // insert into accept queue
    c->accept_count++;

    wake_pid(&c->blocked_acceptor);                     // wake acceptor
    return ERR_OK;
}

// data arrived (UDP)
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {

    netfs_udp_t *c = (netfs_udp_t *)arg;
    (void)pcb;
    if (!p) return;

    c->remote_ip   = *addr;                                                 // store sender info
    c->remote_port = port;

    for (struct pbuf *q = p; q; q = q->next) {                              // for each chunk
        const uint8_t *src = (const uint8_t *)q->payload;                   // copy data -> buffer
        for (u16_t i = 0; i < q->len; i++) {
            if (c->rx_count < NETFS_RX_BUF) {                               // if space available
                uint32_t tail = (c->rx_head + c->rx_count) % NETFS_RX_BUF;  // compute tail
                c->rx_buf[tail] = src[i];                                   // store byte
                c->rx_count++;
            }                                                               // silently drop bytes if buffer full
        }
    }
    pbuf_free(p);                                                           // free packet
    wake_pid(&c->blocked_reader);                                           // wake reader
}






