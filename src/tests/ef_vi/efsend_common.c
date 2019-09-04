/*
** Copyright 2005-2017  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/*
** Copyright 2005-2017  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**
** * Redistributions of source code must retain the above copyright notice,
**   this list of conditions and the following disclaimer.
**
** * Redistributions in binary form must reproduce the above copyright
**   notice, this list of conditions and the following disclaimer in the
**   documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
** IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
** TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
** PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


/* Function definitions common to apps in the efsend suite.
 *
 * CUSTOMER NOTE: This code  is not intended to be used outside of the efsend
 * suite!
 */

#include "efsend_common.h"

static uint8_t mcast_mac[6];
static struct sockaddr_in sa_local, sa_mcast;

int init_udp_pkt(void* pkt_buf, int paylen, ef_vi *vi,
                 ef_driver_handle dh)
{
  int ip_len = sizeof(ci_ip4_hdr) + sizeof(ci_udp_hdr) + paylen;
  ci_ether_hdr* eth;
  ci_ip4_hdr* ip4;
  ci_udp_hdr* udp;

  eth = pkt_buf;
  ip4 = (void*) ((char*) eth + 14);
  udp = (void*) (ip4 + 1);

  memcpy(eth->ether_dhost, mcast_mac, 6);
  ef_vi_get_mac(vi, dh, eth->ether_shost);
  eth->ether_type = htons(0x0800);
  ci_ip4_hdr_init(ip4, CI_NO_OPTS, ip_len, 0, IPPROTO_UDP,
		  sa_local.sin_addr.s_addr,
		  sa_mcast.sin_addr.s_addr, 0);
  ci_udp_hdr_init(udp, ip4, sa_local.sin_port,
		  sa_mcast.sin_port, udp + 1, paylen, 0);

  return ETH_HLEN + ip_len;
}

void common_usage()
{
  fprintf(stderr, "usage:\n");
  fprintf(stderr, "  efsend [options] <interface> <mcast-ip> <mcast-port>\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "positionals:\n");
  fprintf(stderr, " <interface>     local interface for sends and receives\n");
  fprintf(stderr, " <mcast-ip>      multicast ip address to send packets to\n");
  fprintf(stderr, " <mcast-port>    multicast port to send packets to\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "options:\n");
  fprintf(stderr, "  -n <iterations>     - number of packets to send\n");
  fprintf(stderr, "  -m <message-size>   - set udp payload size\n");
  fprintf(stderr, "  -s <microseconds>   - time to sleep between sends\n");
  fprintf(stderr, "  -l <local-port>     - change local port to send from\n");
}

void parse_args(char *argv[], int *ifindex, int local_port)
{
  const char *interface, *mcast_ip;
  char* local_ip;
  int mcast_port;

  interface = (argv++)[0];
  mcast_ip = (argv++)[0];
  mcast_port = atoi(argv[0]);

  get_ipaddr_of_intf(interface, &local_ip);
  CL_CHK(parse_interface(interface, ifindex));
  CL_CHK(parse_host(local_ip, &sa_local.sin_addr));
  sa_local.sin_port = htons(local_port);
  CL_CHK(parse_host(mcast_ip, &sa_mcast.sin_addr));
  sa_mcast.sin_port = htons(mcast_port);

  mcast_mac[0] = 0x1;
  mcast_mac[1] = 0;
  mcast_mac[2] = 0x5e;
  mcast_mac[3] = 0x7f & (sa_mcast.sin_addr.s_addr >> 8);
  mcast_mac[4] = 0xff & (sa_mcast.sin_addr.s_addr >> 16);
  mcast_mac[5] = 0xff & (sa_mcast.sin_addr.s_addr >> 24);
}
