/*
  PCulator: A portable, open-source x86 PC emulator.
  Copyright (C)2025 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
	pcap host networking interface
*/

#include "../config.h"

#ifdef USE_PCAP

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../debuglog.h"
#include "../utility.h"
#include "../timing.h"
#include "pcap.h"

pcap_t* pcap_adhandle;

static HOSTNET_RX_CB pcap_rx_cb = NULL;
static void* pcap_rx_opaque = NULL;
static HOSTNET_LINK_CB pcap_link_cb = NULL;
static void* pcap_link_opaque = NULL;

int pcap_active = 0;

uint32_t pcap_timer;

void pcap_listdevs() {
	pcap_if_t* alldevs;
	pcap_if_t* d;
	int i = 0;
	char errbuf[PCAP_ERRBUF_SIZE];

	/* Retrieve the device list from the local machine */
#ifdef _WIN32
	if (pcap_findalldevs_ex(PCAP_SRC_IF_STRING, NULL /* auth is not needed */, &alldevs, errbuf) == -1)
#else
	if (pcap_findalldevs(&alldevs, errbuf) == -1)
#endif
	{
		fprintf(stderr, "Error in pcap_findalldevs_ex: %s\n", errbuf);
		exit(1);
	}

	/* Print the list */
	for (d = alldevs; d != NULL; d = d->next)
	{
		printf("%d. %s", ++i, d->name);
		if (d->description)
			printf(" (%s)\n", d->description);
		else
			printf(" (No description available)\n");
	}

	if (i == 0)
	{
		printf("\nNo interfaces found! Make sure pcap is installed.\n");
		return;
	}

	/* We don't need any more the device list. Free it */
	pcap_freealldevs(alldevs);
}

static void hostnet_notify_link(uint32_t link_state)
{
	if (pcap_link_cb != NULL) {
		pcap_link_cb(pcap_link_opaque, link_state);
	}
}

int hostnet_attach(int dev, HOSTNET_RX_CB rx_cb, void* rx_opaque, HOSTNET_LINK_CB link_cb, void* link_opaque) {
	pcap_if_t* alldevs;
	pcap_if_t* d;
	int i = 0;
	char errbuf[PCAP_ERRBUF_SIZE];

#ifdef _WIN32
	if (pcap_findalldevs_ex(PCAP_SRC_IF_STRING, NULL, &alldevs, errbuf) == -1) {
#else
	if (pcap_findalldevs(&alldevs, errbuf) == -1) {
#endif
		debug_log(DEBUG_ERROR, "Error in pcap_findalldevs: %s\n", errbuf);
		return -1;
	}

	if (dev <= 0) {
		pcap_freealldevs(alldevs);
		debug_log(DEBUG_ERROR, "[PCAP] Invalid host interface index %d\n", dev);
		return -1;
	}

	for (d = alldevs, i = 0; (i < (dev - 1)) && (d != NULL); d = d->next, i++);
	if (d == NULL) {
		pcap_freealldevs(alldevs);
		debug_log(DEBUG_ERROR, "[PCAP] Host interface index %d was not found\n", dev);
		return -1;
	}

	debug_log(DEBUG_DETAIL, "[PCAP] Initializing pcap library using device: \"%s\"\r\n", d->description ? d->description : "No description available");

#ifdef _WIN32
	if ((pcap_adhandle = pcap_open(d->name, 65536, PCAP_OPENFLAG_PROMISCUOUS, 1000, NULL, errbuf)) == NULL) {
#else
	if ((pcap_adhandle = pcap_open_live(d->name, 65535, 1, -1, errbuf)) == NULL) {
#endif
		debug_log(DEBUG_ERROR, "\nUnable to open the adapter. %s is not supported by pcap: %s\n", d->name, errbuf);
		pcap_freealldevs(alldevs);
		return -1;
	}
	if (pcap_setnonblock(pcap_adhandle, 1, errbuf) != 0) {
		debug_log(DEBUG_ERROR, "[PCAP] Failed to put capture handle in nonblocking mode: %s\n", errbuf);
		pcap_close(pcap_adhandle);
		pcap_adhandle = NULL;
		pcap_freealldevs(alldevs);
		return -1;
	}
	pcap_active = 1;

	pcap_freealldevs(alldevs);

	pcap_rx_cb = rx_cb;
	pcap_rx_opaque = rx_opaque;
	pcap_link_cb = link_cb;
	pcap_link_opaque = link_opaque;

	pcap_timer = timing_addTimer(pcap_check_packets, NULL, 1000, TIMING_ENABLED);
	hostnet_notify_link(0);

	return 0;
}

struct pcap_packet_s {
	u_char data[4096];
	int len;
} pcap_buffer[128];

int pcap_bfrcount = 0;

void pcap_rx_buf(u_char* param, const struct pcap_pkthdr* header, const u_char* pkt_data) {
	(void)(param); //unused variable
	int copy_len;

	if (pcap_bfrcount == 128) pcap_bfrcount = 0; //if we've overflowed, just dump all the packets in memory and start over
	copy_len = (header->caplen > 4096) ? 4096 : (int)header->caplen;
	pcap_buffer[pcap_bfrcount].len = copy_len;
	memcpy(pcap_buffer[pcap_bfrcount].data, pkt_data, copy_len);
	pcap_bfrcount++;
}

void pcap_check_packets(void* dummy) {
	int i;

	(void) dummy;

	if (!pcap_active) return;

	while (1) {
		if (!pcap_dispatch(pcap_adhandle, 1, pcap_rx_buf, NULL)) break; // pcap_rx_handler, NULL);
	}

	if ((pcap_bfrcount == 0) || (pcap_rx_cb == NULL)) return;
	pcap_rx_cb(pcap_rx_opaque, pcap_buffer[0].data, pcap_buffer[0].len);

	for (i = 1; i < pcap_bfrcount; i++) {
		memcpy(pcap_buffer[i - 1].data, pcap_buffer[i].data, 4096);
		pcap_buffer[i - 1].len = pcap_buffer[i].len;
	}
	pcap_bfrcount--;
}

void pcap_rx_handler(u_char* param, const struct pcap_pkthdr* header, const u_char* pkt_data) {
	(void)(param); //unused variable

	if (pcap_rx_cb != NULL) {
		pcap_rx_cb(pcap_rx_opaque, (const uint8_t*)pkt_data, (int)header->caplen);
	}
}

void hostnet_tx(const uint8_t* data, int len) {
	if (!pcap_active || (pcap_adhandle == NULL)) {
		return;
	}

	pcap_sendpacket(pcap_adhandle, data, len);
}

uint32_t hostnet_link_state(void)
{
	return pcap_active ? 0U : NET_LINK_DOWN;
}

int hostnet_is_attached(void)
{
	return pcap_active;
}

#endif
