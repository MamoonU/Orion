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

// allocate new TCP connection 
static int tcp_clone_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    (void)v;
    (void)off;

    uint32_t slot = ~0u;
    for (uint32_t i = 0; i < NETFS_TCP_MAX; i++) {
        if (tcp_conns[i].state == CONN_FREE) {                          // find free slot
            slot = i;
            break;
        }
    }
    if (slot == ~0u) return -1;

    netfs_tcp_t *c = &tcp_conns[slot];
    c->pcb = tcp_new();                                                 // create lwIP PCB
    if (!c->pcb) return -1;

    c->rx_head = 0;                                                     // initialise state
    c->rx_count = 0;
    c->blocked_reader    = NETFS_NO_WAITER;
    c->blocked_connector = NETFS_NO_WAITER;
    c->blocked_acceptor  = NETFS_NO_WAITER;
    c->accept_head = 0;
    c->accept_count = 0;
    c->conn_err = 0;
    ip4_addr_set_any(&c->local_ip);
    ip4_addr_set_any(&c->remote_ip);
    c->local_port = 0; c->remote_port = 0;

    tcp_arg (c->pcb, c);                                                // register callbacks
    tcp_recv(c->pcb, tcp_recv_cb);
    tcp_err (c->pcb, tcp_err_cb);

    c->state = CONN_ALLOCATED;

    char tmp[16];
    uint_to_str(slot, tmp, sizeof(tmp));                                // return slot # as string
    uint32_t n = (uint32_t)strlen(tmp);
    if (n > len) n = len;
    memcpy(buf, tmp, n);
    return (int)n;
}

// control plane: TCP operations occur -> writing commands
static int tcp_ctl_write(vnode_t *v, const void *buf, uint32_t len, uint32_t off) {

    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_tcp_t  *c = &tcp_conns[f->slot];
    if (c->state == CONN_FREE) return -1;

    char cmd[128];
    uint32_t n = (len < sizeof(cmd)-1) ? len : sizeof(cmd)-1;
    memcpy(cmd, buf, n); cmd[n] = '\0';
    trim_ws(cmd);

    if (strncmp(cmd, "connect ", 8) == 0) {                                         // 1. connect ip!port

        if (c->state != CONN_ALLOCATED) return -1;
        ip4_addr_t rip; uint16_t rport;
        if (parse_addr_port(cmd + 8, &rip, &rport) < 0) return -1;                  // parse address port

        c->remote_ip   = rip;                                                       // save target
        c->remote_port = rport;
        c->state       = CONN_CONNECTING;

        err_t e = tcp_connect(c->pcb, &rip, rport, tcp_connected_cb);               // call lwIP
        if (e != ERR_OK) { c->state = CONN_ALLOCATED; return -1; }

        block_self(&c->blocked_connector);                                          // block process
        return (c->state == CONN_ESTABLISHED) ? (int)len : -1;

    } else if (strncmp(cmd, "announce ", 9) == 0) {                                 // 2. announce *!port

        if (c->state != CONN_ALLOCATED) return -1;
        ip4_addr_t lip; uint16_t lport;
        if (parse_addr_port(cmd + 9, &lip, &lport) < 0) return -1;                  // parse address port

        c->local_ip   = lip;                                                        // save target
        c->local_port = lport;

        tcp_bind(c->pcb, &lip, lport);                                              // bind -> IP/port
        struct tcp_pcb *lpcb = tcp_listen(c->pcb);                                  // convert to listening pcb
        if (!lpcb) { c->state = CONN_FREE; c->pcb = 0; return -1; }
        c->pcb = lpcb;
        tcp_arg   (c->pcb, c);
        tcp_accept(c->pcb, tcp_accept_cb);                                          // register accept callback
        c->state = CONN_LISTEN;                                                     // set
        return (int)len;

    } else if (strcmp(cmd, "accept") == 0) {                                        // 3. accept

        if (c->state != CONN_LISTEN) return -1;

        while (c->accept_count == 0 && c->state == CONN_LISTEN) {                   // wait: tcp_accept_cb enqueues new PCB
            block_self(&c->blocked_acceptor);
        }
        if (c->accept_count == 0) return -1;

        uint32_t nslot = ~0u;                                                       // find free slot for the new connection
        for (uint32_t i = 0; i < NETFS_TCP_MAX; i++) {
            if (tcp_conns[i].state == CONN_FREE) { nslot = i; break; }
        }
        if (nslot == ~0u) return -1;

        netfs_tcp_t *nc = &tcp_conns[nslot];
        nc->pcb = c->accept_q[c->accept_head];                                      // transfer PCB
        c->accept_head  = (uint8_t)((c->accept_head + 1) & (NETFS_ACCEPT_Q - 1));
        c->accept_count--;

        nc->rx_head = 0;
        nc->rx_count = 0;
        nc->blocked_reader    = NETFS_NO_WAITER;
        nc->blocked_connector = NETFS_NO_WAITER;
        nc->blocked_acceptor  = NETFS_NO_WAITER;
        nc->accept_head = 0;
        nc->accept_count = 0;
        nc->conn_err = 0;
        nc->local_port  = nc->pcb->local_port;
        nc->remote_port = nc->pcb->remote_port;
        nc->local_ip    = nc->pcb->local_ip;
        nc->remote_ip   = nc->pcb->remote_ip;
        nc->state       = CONN_ESTABLISHED;

        tcp_arg (nc->pcb, nc);
        tcp_recv(nc->pcb, tcp_recv_cb);
        tcp_err (nc->pcb, tcp_err_cb);

        kprintf("NETFS: accept: new TCP connection in slot %u\n", nslot);
        return (int)len;

    } else if (strcmp(cmd, "close") == 0) {                                         // 4. close

        if (c->pcb) {                                                               // graceful close & abort
            if (c->state == CONN_ESTABLISHED) tcp_close(c->pcb);
            else                              tcp_abort(c->pcb);
            c->pcb = 0;
        }

        c->state = CONN_FREE;                                                       // free
        wake_pid(&c->blocked_reader);                                               // wake reader
        return (int)len;
    }
    return -1;
}

// implement blocking read() -> TCP socket
static int tcp_data_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_tcp_t  *c = &tcp_conns[f->slot];

    if (c->state == CONN_FREE || c->state == CONN_ALLOCATED) return -1;     // reject invalid states

    while (c->rx_count == 0) {                                              // block if no data
        if (c->state == CONN_CLOSING || c->state == CONN_FREE) return 0;    // EOF
        block_self(&c->blocked_reader);
    }

    uint32_t n = (len < c->rx_count) ? len : c->rx_count;
    uint8_t *dst = (uint8_t *)buf;

    for (uint32_t i = 0; i < n; i++) {                                      // copy from buffer
        dst[i] = c->rx_buf[c->rx_head];
        c->rx_head = (c->rx_head + 1) % NETFS_RX_BUF;
    }

    c->rx_count -= n;                                                       // update count
    return (int)n;
}

// send data over TCP
static int tcp_data_write(vnode_t *v, const void *buf, uint32_t len, uint32_t off) {

    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_tcp_t  *c = &tcp_conns[f->slot];
    if (c->state != CONN_ESTABLISHED) return -1;                            // check state

    err_t e = tcp_write(c->pcb, buf, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) return -1;
    tcp_output(c->pcb);                                                     // flush
    return (int)len;
}

// return human readable state
static int tcp_status_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    const char *s;
    switch (tcp_conns[f->slot].state) {
        case CONN_FREE:        s = "Free\n";        break;
        case CONN_ALLOCATED:   s = "Allocated\n";   break;
        case CONN_CONNECTING:  s = "Connecting\n";  break;
        case CONN_ESTABLISHED: s = "Established\n"; break;
        case CONN_LISTEN:      s = "Listen\n";      break;
        case CONN_CLOSING:     s = "Closing\n";     break;
        default:               s = "Unknown\n";     break;
    }
    uint32_t n = (uint32_t)strlen(s);
    if (n > len) n = len;
    memcpy(buf, s, n);
    return (int)n;
}

// read/return local IP
static int tcp_local_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {
    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_tcp_t  *c = &tcp_conns[f->slot];
    char tmp[48];
    uint32_t n = format_ap(&c->local_ip, c->local_port, tmp, sizeof(tmp));
    if (n > len) n = len;
    memcpy(buf, tmp, n);
    return (int)n;
}

// read/retrun remote IP
static int tcp_remote_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {
    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_tcp_t  *c = &tcp_conns[f->slot];
    char tmp[48];
    uint32_t n = format_ap(&c->remote_ip, c->remote_port, tmp, sizeof(tmp));
    if (n > len) n = len;
    memcpy(buf, tmp, n);
    return (int)n;
}

// allocate new UDP slot
static int udp_clone_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    (void)v; (void)off;
    uint32_t slot = ~0u;
    for (uint32_t i = 0; i < NETFS_UDP_MAX; i++) {                      // find free slot
        if (udp_conns[i].state == CONN_FREE) { slot = i; break; }
    }
    if (slot == ~0u) return -1;

    netfs_udp_t *c = &udp_conns[slot];
    c->pcb = udp_new();                                                 // create UDP PCB
    if (!c->pcb) return -1;

    c->rx_head = 0; c->rx_count = 0;                                    // initialise state
    c->blocked_reader = NETFS_NO_WAITER;

    ip4_addr_set_any(&c->local_ip);                                     // set default addresses
    ip4_addr_set_any(&c->remote_ip);

    c->local_port = 0; c->remote_port = 0;
    c->state = CONN_ALLOCATED;
    udp_recv(c->pcb, udp_recv_cb, c);                                   // register recieve callback

    char tmp[16]; uint_to_str(slot, tmp, sizeof(tmp));
    uint32_t n = (uint32_t)strlen(tmp);
    if (n > len) n = len;
    memcpy(buf, tmp, n);
    return (int)n;
}

// control plane: UDP operations occur -> writing commands
static int udp_ctl_write(vnode_t *v, const void *buf, uint32_t len, uint32_t off) {

    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_udp_t  *c = &udp_conns[f->slot];
    if (c->state == CONN_FREE) return -1;

    char cmd[128];
    uint32_t n = (len < sizeof(cmd)-1) ? len : sizeof(cmd)-1;
    memcpy(cmd, buf, n); cmd[n] = '\0';
    trim_ws(cmd);

    if (strncmp(cmd, "connect ", 8) == 0) {                             // 1. connect

        ip4_addr_t rip; uint16_t rport;
        if (parse_addr_port(cmd + 8, &rip, &rport) < 0) return -1;
        c->remote_ip = rip; c->remote_port = rport;
        udp_connect(c->pcb, &rip, rport);
        c->state = CONN_ESTABLISHED;
        return (int)len;

    } else if (strncmp(cmd, "announce ", 9) == 0) {                     // 2. announce (bind)

        ip4_addr_t lip; uint16_t lport;
        if (parse_addr_port(cmd + 9, &lip, &lport) < 0) return -1;
        c->local_ip = lip; c->local_port = lport;
        err_t e = udp_bind(c->pcb, &lip, lport);
        if (e != ERR_OK) return -1;
        c->state = CONN_ESTABLISHED;
        return (int)len;

    } else if (strcmp(cmd, "close") == 0) {                             // 3. close

        if (c->pcb) { udp_remove(c->pcb); c->pcb = 0; }
        c->state = CONN_FREE;
        wake_pid(&c->blocked_reader);
        return (int)len;
    }
    return -1;
}

// blocking read() from UDP recieve buffer
static int udp_data_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_udp_t  *c = &udp_conns[f->slot];
    if (c->state == CONN_FREE || c->state == CONN_ALLOCATED) return -1;     // reject invalid states

    while (c->rx_count == 0) {                                              // block if no data
        if (c->state == CONN_FREE) return 0;
        block_self(&c->blocked_reader);
    }

    uint32_t n = (len < c->rx_count) ? len : c->rx_count;
    uint8_t *dst = (uint8_t *)buf;

    for (uint32_t i = 0; i < n; i++) {
        dst[i] = c->rx_buf[c->rx_head];                                     // copy from buffer
        c->rx_head = (c->rx_head + 1) % NETFS_RX_BUF;
    }
    c->rx_count -= n;
    return (int)n;
}

// send UDP packet
static int udp_data_write(vnode_t *v, const void *buf, uint32_t len, uint32_t off) {
    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_udp_t  *c = &udp_conns[f->slot];
    if (c->state != CONN_ESTABLISHED) return -1;                                // check state

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);          // allocate pbuf
    if (!p) return -1;
    memcpy(p->payload, buf, len);                                               // copy data

    err_t e;
    if (ip4_addr_isany_val(c->remote_ip))                                       // send data
        e = udp_sendto(c->pcb, p, &c->remote_ip, c->remote_port);
    else
        e = udp_send(c->pcb, p);

    pbuf_free(p);                                                               // free buffer
    return (e == ERR_OK) ? (int)len : -1;
}

// return free/allocated/established
static int udp_status_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {
    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    const char *s;
    switch (udp_conns[f->slot].state) {
        case CONN_FREE:        s = "Free\n";        break;
        case CONN_ALLOCATED:   s = "Allocated\n";   break;
        case CONN_ESTABLISHED: s = "Established\n"; break;
        default:               s = "Unknown\n";     break;
    }
    uint32_t n = (uint32_t)strlen(s);
    if (n > len) n = len;
    memcpy(buf, s, n);
    return (int)n;
}

// local IP!port -> human readable
static int udp_local_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {
    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_udp_t  *c = &udp_conns[f->slot];
    char tmp[48];
    uint32_t n = format_ap(&c->local_ip, c->local_port, tmp, sizeof(tmp));
    if (n > len) n = len;
    memcpy(buf, tmp, n);
    return (int)n;
}

// remote IP!port -> human readable
static int udp_remote_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {
    (void)off;
    netfs_file_t *f = (netfs_file_t *)v->data;
    netfs_udp_t  *c = &udp_conns[f->slot];
    char tmp[48];
    uint32_t n = format_ap(&c->remote_ip, c->remote_port, tmp, sizeof(tmp));
    if (n > len) n = len;
    memcpy(buf, tmp, n);
    return (int)n;
}

// /net/ififc/status: return current network interface as text
static int ipifc_status_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    (void)v; (void)off;
    char tmp[256]; tmp[0] = '\0';                                                   // temp buff to build output string manually

    if (netif_default) {                                                            // lwIP's default network interface (virtio-net)
        char ip_s[20], mask_s[20], gw_s[20];
        ip4addr_ntoa_r(netif_ip4_addr(netif_default),    ip_s,   sizeof(ip_s));     // extract IP address
        ip4addr_ntoa_r(netif_ip4_netmask(netif_default), mask_s, sizeof(mask_s));   // extract netmask
        ip4addr_ntoa_r(netif_ip4_gw(netif_default),      gw_s,   sizeof(gw_s));     // extract gateway

        uint8_t mac[6]; virtio_net_get_mac(mac);                                    // return MAC address
        // format: "ip=x.x.x.x mask=x.x.x.x gw=x.x.x.x mac=xx:xx:xx:xx:xx:xx\n"
        strncpy(tmp, "ip=",   sizeof(tmp)-1); kstrcat(tmp, ip_s);                   // building output string manually
        kstrcat(tmp, " mask="); kstrcat(tmp, mask_s);
        kstrcat(tmp, " gw=");   kstrcat(tmp, gw_s);

        char mac_str[20];
        static const char hex[] = "0123456789abcdef";                               // manual MAC formatting
        for (int i = 0, j = 0; i < 6; i++) {
            if (i > 0) mac_str[j++] = ':';
            mac_str[j++] = hex[(mac[i] >> 4) & 0xF];
            mac_str[j++] = hex[ mac[i]        & 0xF];
            mac_str[j]   = '\0';
        }
        kstrcat(tmp, " mac=");                                                      // append MAC manually (no sprintf in kernel)
        kstrcat(tmp, mac_str);
        kstrcat(tmp, "\n");
    } else {
        strncpy(tmp, "no interface\n", sizeof(tmp)-1);                              // no interface
    }

    uint32_t n = (uint32_t)strlen(tmp);
    if (n > len) n = len;
    memcpy(buf, tmp, n);                                                            // return to user
    return (int)n;
}

// /net/ififc/ctl: manually configure IP settings
static int ipifc_ctl_write(vnode_t *v, const void *buf, uint32_t len, uint32_t off) {

    (void)v; (void)off;
    if (!netif_default) return -1;

    char cmd[128];
    uint32_t n = (len < sizeof(cmd)-1) ? len : sizeof(cmd)-1;
    memcpy(cmd, buf, n); cmd[n] = '\0';
    trim_ws(cmd);                                                   // parse command

    if (strncmp(cmd, "add ", 4) == 0) {                             // only supported command ( add <ip> <mask> [gateway] )

        const char *p = cmd + 4;                                    // parsing pointer
        ip4_addr_t ip, mask, gw;
        ip4_addr_set_any(&gw);
        const char *tok = p;                                        // parse token start
        while (*p && *p != ' ') p++;                                // scan until " "
        char s[20];
        uint32_t sl = (uint32_t)(p - tok);                          // compute length
        if (sl >= sizeof(s)) return -1;
        memcpy(s, tok, sl);                                         // copy into temp buffer
        s[sl] = '\0';
        if (!ip4addr_aton(s, &ip)) return -1;                       // string -> IP
        while (*p == ' ') p++;                                      // skip spaces

        tok = p;                                                    // parse subnet mask (same logic above)
        while (*p && *p != ' ') p++;
        sl = (uint32_t)(p - tok);
        if (sl >= sizeof(s)) return -1;
        memcpy(s, tok, sl);
        s[sl] = '\0';
        if (!ip4addr_aton(s, &mask)) return -1;
        while (*p == ' ') p++;

        if (*p) ip4addr_aton(p, &gw);                               // parse optional gateway

        netif_set_addr(netif_default, &ip, &mask, &gw);             // apply configuration to lwIP
        netif_set_up(netif_default);
        kprintf("NETFS: ipifc: static IP configured\n");
        return (int)len;
    }
    return -1;
}

// ARP placeholder
static int arp_read(vnode_t *v, void *buf, uint32_t len, uint32_t off) {

    (void)v;
    (void)off;

    const char *msg = "arp table: see lwIP etharp internals\n";
    uint32_t n = (uint32_t)strlen(msg);
    if (n > len) n = len;
    memcpy(buf, msg, n);
    return (int)n;
}

// VFS ops tables: map files -> functions
static vfs_ops_t tcp_clone_ops  = { .read = tcp_clone_read };
static vfs_ops_t tcp_ctl_ops    = { .write = tcp_ctl_write };
static vfs_ops_t tcp_data_ops   = { .read = tcp_data_read,  .write = tcp_data_write };
static vfs_ops_t tcp_status_ops = { .read = tcp_status_read };
static vfs_ops_t tcp_local_ops  = { .read = tcp_local_read };
static vfs_ops_t tcp_remote_ops = { .read = tcp_remote_read };

static vfs_ops_t udp_clone_ops  = { .read = udp_clone_read };
static vfs_ops_t udp_ctl_ops    = { .write = udp_ctl_write };
static vfs_ops_t udp_data_ops   = { .read = udp_data_read,  .write = udp_data_write };
static vfs_ops_t udp_status_ops = { .read = udp_status_read };
static vfs_ops_t udp_local_ops  = { .read = udp_local_read };
static vfs_ops_t udp_remote_ops = { .read = udp_remote_read };

static vfs_ops_t ipifc_status_ops = { .read  = ipifc_status_read };
static vfs_ops_t ipifc_ctl_ops    = { .write = ipifc_ctl_write };
static vfs_ops_t arp_ops          = { .read  = arp_read };

// create filesystem vnode = network object
static vnode_t *make_vnode(vfs_ops_t *ops, uint8_t proto, uint8_t ftype, uint32_t slot) {

    netfs_file_t *fd = (netfs_file_t *)kmalloc(sizeof(netfs_file_t));   // store protocol, file type, slot
    if (!fd) return 0;
    fd->proto = proto; fd->ftype = ftype; fd->slot = slot;

    vnode_t *v = vnode_alloc(VNODE_DEV, ops, fd);                       // create vnode (type = device, ops = function table, data = my metadata)
    if (!v) { kfree(fd); return 0; }
    return v;
}

// register vnode -> RAMfs
static void reg(const char *path, vnode_t *v) {
    if (v) ramfs_register_dev(path, v);
}

// build path: dynamically construct ( base + N + suffix into out[outsz] )
static void build_path(char *out, uint32_t outsz, const char *base, uint32_t slot, const char *suffix) {
    strncpy(out, base, outsz - 1); out[outsz - 1] = '\0';
    char num[8]; uint_to_str(slot, num, sizeof(num));
    kstrcat(out, num);
    if (suffix) kstrcat(out, suffix);
}