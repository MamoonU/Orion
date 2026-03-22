// virtio_net.c - Legacy virtio-net NIC driver

#include "virtio_net.h"
#include "pci.h"
#include "ioport.h"
#include "irq.h"
#include "pmm.h"
#include "vmm.h"
#include "kprintf.h"

static uint32_t io_base = 0;            // base I/O port from PCI BAR0

// in/out helpers
static inline uint8_t  vio_in8 (uint16_t off) { return inb ((uint16_t)(io_base+off)); }
static inline uint16_t vio_in16(uint16_t off) { return inw ((uint16_t)(io_base+off)); }
static inline uint32_t vio_in32(uint16_t off) { return inl ((uint16_t)(io_base+off)); }
static inline void vio_out8 (uint16_t off, uint8_t  v) { outb((uint16_t)(io_base+off), v); }
static inline void vio_out16(uint16_t off, uint16_t v) { outw((uint16_t)(io_base+off), v); }
static inline void vio_out32(uint16_t off, uint32_t v) { outl((uint16_t)(io_base+off), v); }

#define INITIAL_IDENTITY_END  0x400000u     // vmm_init maps 0 – 4MB

// dynamic physical memory allocation
static uint32_t dma_alloc_frames(uint32_t n) {

    uint32_t phys = (n == 1) ? pmm_alloc_frame() : pmm_alloc_contiguous(n);     // if 1 page -> simple alloc, if multiple -> contiguous alloc
    if (!phys) return 0;                                                        // OOM

    for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = phys + i * PAGE_SIZE;
        if (addr >= INITIAL_IDENTITY_END) {
            vmm_map_page(addr, addr, VMM_KERNEL_RW);                            // extend identity map
        }
    }
    return phys;
}

// virtqueue structure
typedef struct {
    virtq_desc_t      *desc;        // descriptor table

    // available ring sub-fields (avail ring = flags:u16, idx:u16, ring[qsz]:u16)
    uint16_t          *avail_flags;
    uint16_t          *avail_idx;
    uint16_t          *avail_ring;  // avail_ring[i] = desc table index

    // used ring sub-fields (used ring = flags:u16, idx:u16, elem[qsz]:8 bytes each)
    uint16_t          *used_flags;
    uint16_t          *used_idx;
    virtq_used_elem_t *used_ring;   // used_ring[i].id / .len

    uint16_t           qsz;         // actual queue depth reported by device
    uint16_t           last_used;   // last used->idx we processed
    uint32_t           phys_base;
    uint32_t           num_pages;
} virtqueue_t;

// compute byte offset of used ring from queue base
static uint32_t used_ring_offset(uint16_t qsz) {
    uint32_t desc_bytes  = 16u * qsz;
    uint32_t avail_bytes = 4u + 2u * qsz;                   // flags + idx + ring[qsz]
    uint32_t raw         = desc_bytes + avail_bytes;

    return (raw + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);      // align to page
}

// initalise virt queues
static int virtq_init(virtqueue_t *vq, uint16_t queue_idx) {

    vio_out16(VIRTIO_PCI_QUEUE_SELECT, queue_idx);                  // select queue

    uint16_t qsz = vio_in16(VIRTIO_PCI_QUEUE_SIZE);                 // read queue size
    if (qsz == 0) {
        kprintf("VIRTIO-NET: queue %u has size 0\n", queue_idx);
        return -1;
    }

    if (qsz > VIRTQ_SIZE_MAX) qsz = VIRTQ_SIZE_MAX;                 // clamp size
    vq->qsz = qsz;

    uint32_t u_off   = used_ring_offset(qsz);                       // compute memory requirements
    uint32_t u_bytes = 4u + 8u * qsz;                               // flags + idx + elem[qsz]
    uint32_t total   = u_off + u_bytes;
    uint32_t pages   = (total + PAGE_SIZE - 1u) / PAGE_SIZE;
    vq->num_pages    = pages;

    uint32_t phys = dma_alloc_frames(pages);                        // allocate memory
    if (!phys) {
        kprintf("VIRTIO-NET: OOM for virtqueue %u (%u pages)\n", queue_idx, pages);
        return -1;
    }
    vq->phys_base = phys;

    uint8_t *base = (uint8_t *)phys;
    for (uint32_t i = 0; i < pages * PAGE_SIZE; i++) base[i] = 0;   // zero memory

    uint32_t desc_bytes = 16u * qsz;
    vq->desc        = (virtq_desc_t *)phys;                         // set internal pointers
    vq->avail_flags = (uint16_t *)(phys + desc_bytes);
    vq->avail_idx   = (uint16_t *)(phys + desc_bytes + 2);
    vq->avail_ring  = (uint16_t *)(phys + desc_bytes + 4);
    vq->used_flags  = (uint16_t *)(phys + u_off);
    vq->used_idx    = (uint16_t *)(phys + u_off + 2);
    vq->used_ring   = (virtq_used_elem_t *)(phys + u_off + 4);
    vq->last_used   = 0;                                            // initialise state

    vio_out32(VIRTIO_PCI_QUEUE_PFN, phys / PAGE_SIZE);              // tell device

    kprintf("VIRTIO-NET: queue %u  depth=%u  phys=0x%p  pages=%u\n", queue_idx, qsz, phys, pages);
    return 0;
}

// add a descriptor index to available ring and bump avail->idx
static void virtq_push_avail(virtqueue_t *vq, uint16_t desc_idx) {
    vq->avail_ring[(*vq->avail_idx) & (vq->qsz - 1)] = desc_idx;    // insert descriptor index
    asm volatile("" ::: "memory");                                  // memory barrier: desc must be visible first
    (*vq->avail_idx)++;                                             // increment index
    asm volatile("" ::: "memory");                                  // memory barrier
}

// notify device that new available ring entries are ready
static void virtq_kick(uint16_t queue_idx) {
    vio_out16(VIRTIO_PCI_QUEUE_NOTIFY, queue_idx);
}

static virtqueue_t vq_rx;               // RX queue
static virtqueue_t vq_tx;               // TX queue

static uint8_t net_mac[6];

static uint32_t rx_phys[VIRTQ_SIZE_MAX];        // physical address of each RX buffer

static uint32_t tx_phys[VIRTIO_NET_TX_POOL];    // physical address of each TX buffer
static uint8_t  tx_free[VIRTIO_NET_TX_POOL];    // 1 = free, 0 = in-flight
static uint16_t tx_pool_size = 0;

static virtio_net_rx_cb_t rx_callback = 0;

// fill up to VIRTIO_NET_RX_FILL descriptor slots in the RX available ring
static int rx_fill(void) {

    uint16_t fill = vq_rx.qsz < VIRTIO_NET_RX_FILL ? vq_rx.qsz : VIRTIO_NET_RX_FILL;

    for (uint16_t i = 0; i < fill; i++) {

        uint32_t phys = dma_alloc_frames(1);                                    // allocate buffer
        if (!phys) {
            kprintf("VIRTIO-NET: OOM for RX buffer %u\n", i);
            return -1;
        }

        rx_phys[i] = phys;                                                      // save address

        // descriptor: write-only (device fills buffer with hdr + frame)
        vq_rx.desc[i].addr  = (uint64_t)phys;
        vq_rx.desc[i].len   = VIRTIO_NET_BUF_SIZE;
        vq_rx.desc[i].flags = VIRTQ_DESC_F_WRITE;
        vq_rx.desc[i].next  = 0;

        virtq_push_avail(&vq_rx, i);                                            // add to available
    }
    virtq_kick(VIRTIO_NET_RX_QUEUE);                                            // kick
    return 0;
}

// process all entries the device has placed in the used ring
static void rx_process(void) {

    asm volatile("" ::: "memory");                                              // memory barrier

    while (vq_rx.last_used != *vq_rx.used_idx) {                                // process all completed buffers

        uint16_t slot = vq_rx.last_used & (vq_rx.qsz - 1);
        uint16_t did  = (uint16_t)vq_rx.used_ring[slot].id;                     // descriptor ID
        uint32_t wlen = vq_rx.used_ring[slot].len;                              // length written

        if (wlen > sizeof(virtio_net_hdr_t) && rx_callback) {
            uint8_t  *buf     = (uint8_t *)rx_phys[did];
            uint8_t  *payload = buf + sizeof(virtio_net_hdr_t);                 // extract payload
            uint32_t  plen    = wlen - (uint32_t)sizeof(virtio_net_hdr_t);
            rx_callback(payload, plen);                                         // call callback
        }

        // return descriptor to available ring for device reuse
        vq_rx.desc[did].flags = VIRTQ_DESC_F_WRITE;
        vq_rx.desc[did].len   = VIRTIO_NET_BUF_SIZE;
        virtq_push_avail(&vq_rx, did);                                          // recycle descriptor

        vq_rx.last_used++;                                                      // advance index
    }
    virtq_kick(VIRTIO_NET_RX_QUEUE);                                            // kick
}

// reclaim any TX descriptors the device has finished sending
static void tx_reclaim(void) {

    asm volatile("" ::: "memory");                              // memory barrier

    while (vq_tx.last_used != *vq_tx.used_idx) {                // loop over completed descriptors

        uint16_t slot = vq_tx.last_used & (vq_tx.qsz - 1);      // convert -> ring index
        uint16_t did  = (uint16_t)vq_tx.used_ring[slot].id;     // descriptor ID
        if (did < tx_pool_size) tx_free[did] = 1;               // return buffer to pool
        vq_tx.last_used++;                                      // advance pointer
    }
}

// allocate a free TX buffer from the pool
static int tx_alloc_slot(void) {
    for (uint16_t i = 0; i < tx_pool_size; i++) {               // for each buffer
        if (tx_free[i]) {                                       // if free
            tx_free[i] = 0;                                     // mark used
            return (int)i;                                      // return index
        }
    }
    return -1;
}

// send packets 
int virtio_net_send(const void *data, uint32_t len) {

    if (!io_base)                                              return -1;               // device initialised check
    if (len > VIRTIO_NET_BUF_SIZE - sizeof(virtio_net_hdr_t))  return -1;               // buffer overflow check

    tx_reclaim();                                                                       // reclaim old buffers

    int idx = tx_alloc_slot();                                                          // alloc buffer
    if (idx < 0) {
        kprintf("VIRTIO-NET: TX pool exhausted\n");
        return -1;
    }

    uint8_t          *buf = (uint8_t *)tx_phys[idx];                                    // get buffer
    virtio_net_hdr_t *hdr = (virtio_net_hdr_t *)buf;                                    // header
    uint8_t          *pkt = buf + sizeof(virtio_net_hdr_t);                             // packet

    for (uint32_t i = 0; i < sizeof(virtio_net_hdr_t); i++) ((uint8_t*)hdr)[i] = 0;     // clear header
    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) pkt[i] = src[i];                                 // copy payload

    // build descriptor: read-only
    vq_tx.desc[idx].addr  = (uint64_t)tx_phys[idx];                         // physical address
    vq_tx.desc[idx].len   = (uint32_t)sizeof(virtio_net_hdr_t) + len;       // length
    vq_tx.desc[idx].flags = 0;
    vq_tx.desc[idx].next  = 0;

    asm volatile("" ::: "memory");                                          // memory barrier
    virtq_push_avail(&vq_tx, (uint16_t)idx);                                // add to available ring
    virtq_kick(VIRTIO_NET_TX_QUEUE);                                        // kick device

    return 0;
}

// NIC interrupt handler
static void virtio_net_irq(regs_t *r) {

    (void)r;

    uint8_t isr = vio_in8(VIRTIO_PCI_ISR);                                  // reading ISR acknowledges the interrupt to the device
    if (!isr) return;

    if (isr & 0x01) {                                                       // virtqueue activity
        tx_reclaim();
        rx_process();
    }
    if (isr & 0x02) {                                                       // device config changed
        kprintf("VIRTIO-NET: device config change notification\n");
    }
}

// copy NIC's MAC address to caller
void virtio_net_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = net_mac[i];
}

// registers a function to handle incoming packets
void virtio_net_set_rx_callback(virtio_net_rx_cb_t cb) {
    rx_callback = cb;
}

int virtio_net_init(void) {

    kprintf("VIRTIO-NET: scanning PCI...\n");

    pci_device_t pci;                                                                       // PCI discovery
    if (!pci_find_device(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEV_NET, &pci)) {
        kprintf("VIRTIO-NET: device not found\n");
        return -1;
    }

    kprintf("VIRTIO-NET: PCI %u:%u.%u  IRQ %u  BAR0=0x%p\n", pci.bus, pci.device, pci.function, pci.irq_line, pci.bar[0]);

    if (!(pci.bar[0] & 0x1u)) {                                                             // get I/O base (BAR0)
        kprintf("VIRTIO-NET: BAR0 is not an I/O BAR\n");
        return -1;
    }

    io_base = pci.bar[0] & ~0x3u;                                                           // clear flag bits (leave port base)
    kprintf("VIRTIO-NET: I/O base = 0x%p\n", io_base);

    pci_enable_device(&pci);                                                                // enable PCI device

    // 1. reset
    vio_out8(VIRTIO_PCI_STATUS, 0);

    // 2. acknowledge
    vio_out8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    // 3. claim driver status
    vio_out8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // 4. feature negotiation: accept MAC only, decline all offloads
    uint32_t dev_feat = vio_in32(VIRTIO_PCI_HOST_FEATURES);
    kprintf("VIRTIO-NET: device features = 0x%p\n", dev_feat);
    uint32_t drv_feat = dev_feat & VIRTIO_NET_F_MAC;
    vio_out32(VIRTIO_PCI_GUEST_FEATURES, drv_feat);

    // 5. read MAC
    if (dev_feat & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++) {
            net_mac[i] = vio_in8(VIRTIO_PCI_CONFIG + (uint16_t)i);
        }
    } else {
        net_mac[0]=0x52; net_mac[1]=0x54;
        net_mac[2]=0x00; net_mac[3]=0x12;
        net_mac[4]=0x34; net_mac[5]=0x56;
    }
    kprintf("VIRTIO-NET: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", net_mac[0], net_mac[1], net_mac[2], net_mac[3], net_mac[4], net_mac[5]);

    // 6. initialise virtqueues
    if (virtq_init(&vq_rx, VIRTIO_NET_RX_QUEUE) != 0) goto fail;
    if (virtq_init(&vq_tx, VIRTIO_NET_TX_QUEUE) != 0) goto fail;

    // 7. pre-fill RX ring queue
    if (rx_fill() != 0) goto fail;

    // 8. allocate TX buffer pool
    tx_pool_size = (vq_tx.qsz < VIRTIO_NET_TX_POOL) ? vq_tx.qsz : VIRTIO_NET_TX_POOL;
    for (uint16_t i = 0; i < tx_pool_size; i++) {
        uint32_t phys = dma_alloc_frames(1);
        if (!phys) {
            kprintf("VIRTIO-NET: OOM for TX buffer %u\n", i);
            goto fail;
        }
        tx_phys[i] = phys;
        tx_free[i] = 1;
    }
    kprintf("VIRTIO-NET: TX pool: %u buffers\n", tx_pool_size);

    // 9. register IRQ
    irq_install_handler(pci.irq_line, virtio_net_irq);
    kprintf("VIRTIO-NET: IRQ %u registered\n", pci.irq_line);

    // 10. set DRIVER_OK: device is now live
    vio_out8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    kprintf("VIRTIO-NET: driver ready\n");
    return 0;

fail:
    vio_out8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
    kprintf("VIRTIO-NET: initialisation failed\n");
    return -1;
}