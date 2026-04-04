// lwip_orion.h - Orion OS lwIP integration API

#ifndef LWIP_ORION_H
#define LWIP_ORION_H

// initialise lwIP stack and the virtio-net network interface
void lwip_orion_init(void);

// service all pending lwIP timeouts (DHCP, TCP retransmit, ARP expiry ...)
void lwip_orion_poll(void);

#endif