// virtio_net.c - Legacy virtio-net NIC driver
//
// Implements the virtio 0.9.5 PCI transport for the network device QEMU
// exposes as PCI vendor 0x1AF4 / device 0x1000.
//
// Memory model note:
//   This OS identity-maps the first 4MB in vmm_init. Frames allocated by
//   pmm_alloc_frame / pmm_alloc_contiguous above 0x400000 are not yet mapped.
//   dma_alloc() below handles this by calling vmm_map_page() for any frame
//   above the initial 4MB identity window, keeping DMA buffers accessible
//   via their physical addresses.

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
