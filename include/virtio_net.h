// virtio_net.h - Legacy virtio-net NIC driver (virtio 0.9.5, PCI)

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>

// PCI identity
#define VIRTIO_PCI_VENDOR       0x1AF4u     // vendor = Red Hat (QEMU virtual devices)
#define VIRTIO_PCI_DEV_NET      0x1000u     // device = legacy virtio-net

// legacy virtio PCI register offsets (relative to BAR0 I/O base)

// core registers
#define VIRTIO_PCI_HOST_FEATURES    0x00    // device feature bits              (32-bit, R)
#define VIRTIO_PCI_GUEST_FEATURES   0x04    // driver accepted features         (32-bit, W)
#define VIRTIO_PCI_QUEUE_PFN        0x08    // virtqueue physical page number   (32-bit, W)
#define VIRTIO_PCI_QUEUE_SIZE       0x0C    // virtqueue depth                  (16-bit, R)
#define VIRTIO_PCI_QUEUE_SELECT     0x0E    // select active virtqueue          (16-bit, W)
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10    // kick device (process the queue)  (16-bit, W)
#define VIRTIO_PCI_STATUS           0x12    // device status register           (8-bit, R/W)
#define VIRTIO_PCI_ISR              0x13    // interrupt status: read to ACK    (8-bit, R)
#define VIRTIO_PCI_CONFIG           0x14    // device-specific config           (MAC, status, ...)

// device status bits
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01    // acknowledge device
#define VIRTIO_STATUS_DRIVER        0x02    // negotiate features & setup queues
#define VIRTIO_STATUS_DRIVER_OK     0x04    // ready
#define VIRTIO_STATUS_FAILED        0x80    // failed

// virtio-net feature bits
#define VIRTIO_NET_F_MAC        (1u << 5)   // device has a MAC address
#define VIRTIO_NET_F_STATUS     (1u << 16)  // device has a link-status field

// virtqueue indices for virtio-net
#define VIRTIO_NET_RX_QUEUE     0           // RX_QUEUE = incoming packets
#define VIRTIO_NET_TX_QUEUE     1           // TX_QUEUE = outgoing packets

// virtqueue descriptor flags
#define VIRTQ_DESC_F_NEXT       0x01        // descriptor chains to .next
#define VIRTQ_DESC_F_WRITE      0x02        // device writes into this buffer (RX)

// queue sizing
#define VIRTQ_SIZE_MAX          256u        // max queue depth supported (device may report less)
#define VIRTQ_RING_PAGES        3u          // 3 pages total = descriptor table (4096) + available ring (518) → pad to 8192, used ring (2054)

#define VIRTIO_NET_BUF_SIZE     2048u       // RX/TX buffer: virtio_net_header (10) + max Ethernet frame (1514) + headroom
#define VIRTIO_NET_RX_FILL      32u         // number of RX slots to pre-fill (does not need to equal queue depth)
#define VIRTIO_NET_TX_POOL      16u         // number of TX buffers in the pool

// legacy virtio-net packet header structure (10 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} virtio_net_hdr_t;

// virtqueue descriptor structure (16 bytes)
typedef struct __attribute__((packed)) {
    uint64_t addr;          // buffer physical address
    uint32_t len;           // bytes length
    uint16_t flags;         // VIRTQ_DESC_F_*
    uint16_t next;          // index of next descriptor
} virtq_desc_t;

// used ring element: device writes one per completed buffer
typedef struct __attribute__((packed)) {
    uint32_t id;            // descriptor table index
    uint32_t len;           // bytes written by device (for RX)
} virtq_used_elem_t;

// RX callback: network stack hook
typedef void (*virtio_net_rx_cb_t)(const void *data, uint32_t len);     // packet arrives -> driver calls this -> pass raw ethernet frame

// public API
int  virtio_net_init(void);                                             // initialise: find dev -> enable -> setup queues -> negotiate features
int  virtio_net_send(const void *data, uint32_t len);                   // send packets
void virtio_net_get_mac(uint8_t mac[6]);                                // read MAC from config space
void virtio_net_set_rx_callback(virtio_net_rx_cb_t cb);                 // register packet handler

#endif