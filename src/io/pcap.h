#ifndef _PCAP_H_
#define _PCAP_H_

#include "../config.h"
#include <stdint.h>

#define NET_LINK_DOWN 0x01U

typedef void (*HOSTNET_RX_CB)(void* opaque, const uint8_t* data, int len);
typedef void (*HOSTNET_LINK_CB)(void* opaque, uint32_t link_state);

#ifdef USE_PCAP

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <pcap.h>

void pcap_rx_handler(u_char* param, const struct pcap_pkthdr* header, const u_char* pkt_data);
void pcap_listdevs();
int hostnet_attach(int dev, HOSTNET_RX_CB rx_cb, void* rx_opaque, HOSTNET_LINK_CB link_cb, void* link_opaque);
void hostnet_tx(const uint8_t* data, int len);
uint32_t hostnet_link_state(void);
int hostnet_is_attached(void);
void pcap_check_packets(void* dummy);

#else

static inline void pcap_listdevs(void)
{
}

static inline int hostnet_attach(int dev, HOSTNET_RX_CB rx_cb, void* rx_opaque, HOSTNET_LINK_CB link_cb, void* link_opaque)
{
	(void)dev;
	(void)rx_cb;
	(void)rx_opaque;
	(void)link_cb;
	(void)link_opaque;
	return 0;
}

static inline void hostnet_tx(const uint8_t* data, int len)
{
	(void)data;
	(void)len;
}

static inline uint32_t hostnet_link_state(void)
{
	return NET_LINK_DOWN;
}

static inline int hostnet_is_attached(void)
{
	return 0;
}

static inline void pcap_check_packets(void* dummy)
{
	(void)dummy;
}

#endif //USE_PCAP

#endif //_PCAP_H_
