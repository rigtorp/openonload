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


/* efdelegated_client
 *
 * Copyright 2015 Solarflare Communications Inc.
 * Author: David Riddoch
 *
 * This is a sample application to demonstrate usage of OpenOnload's
 * "Delegated Sends" feature.  This API allows you to do delegate TCP sends
 * for a particular socket to some other mechanism.  For example, this
 * sample uses the ef_vi layer-2 API in order to get lower latency than is
 * possible with a normal send() call.
 *
 * The API essentially boils down to first retriving the packet headers,
 * adding your own payload to form a raw packet, sending the packet and
 * finally telling Onload what it was you sent so it can update the
 * internal TCP state of the socket.
 *
 * This sample application allows you to compare the performance of normal
 * sends using the kernel stack, using OpenOnload and using ef_vi with the
 * delegated sends API.  It establishes a TCP connection to the server
 * process, which starts sending UDP multicast messages.  The client
 * receives these messages, and replies to a subset of them with a TCP
 * message.  The server measures the latency from the UDP send to the TCP
 * receive.
 *
 * For normal sockt-based sends, run as follows:
 *
 *   onload -p latency ./efdelegated_server <mcast-intf>
 *   onload -p latency ./efdelegated_client <mcast-intf> <server>
 *
 * For "delegated" sends, run as follows:
 *
 *   onload -p latency ./efdelegated_server <mcast-intf>
 *   onload -p latency ./efdelegated_client -d <mcast-intf> <server>
 */

#include <etherfabric/vi.h>
#include <etherfabric/pio.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>
#include <onload/extensions.h>
#include "utils.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <ifaddrs.h>
#include <stdbool.h>
#include <netdb.h>


#define MTU                   1500
#define MAX_ETH_HEADERS       (14/*ETH*/ + 4/*802.1Q*/)
#define MAX_IP_TCP_HEADERS    (20/*IP*/ + 20/*TCP*/ + 12/*TCP options*/)
#define MAX_PACKET            (MTU + MAX_ETH_HEADERS)
#define MAX_MESSAGE           (MTU - MAX_IP_TCP_HEADERS)


static bool        cfg_delegated;
static int         cfg_rx_size = 300;
static int         cfg_tx_size = 200;
static const char* cfg_port = "8122";
static const char* cfg_mcast_addr = "224.1.2.3";


struct client_state {
  unsigned                     alarm_usec;
  bool                         alarm;
  int                          tcp_sock;
  int                          udp_sock;
  ef_pd                        pd;
  ef_pio                       pio;
  ef_driver_handle             dh;
  ef_vi                        vi;
  char*                        msg_buf;
  int                          msg_len;
  /* pio_pkt_len: Non-zero means that we have a prepared send ready to go. */
  int                          pio_pkt_len;
  bool                         pio_in_use;
  bool                         send_is_delegated;
  struct onload_delegated_send ods;
  char                         pkt_buf[MAX_PACKET];
  char                         recv_buf[MTU];
  unsigned                     n_normal_sends;
  unsigned                     n_delegated_sends;
};


static int min(int x, int y)
{
  return x < y ? x : y;
}


static void evq_poll(struct client_state* cs)
{
  ef_request_id ids[EF_VI_TRANSMIT_BATCH];
  ef_event      evs[EF_VI_EVENT_POLL_MIN_EVS];
  int           n_ev, i;

  n_ev = ef_eventq_poll(&(cs->vi), evs, sizeof(evs) / sizeof(evs[0]));
  for( i = 0; i < n_ev; ++i )
    switch( EF_EVENT_TYPE(evs[i]) ) {
    case EF_EVENT_TYPE_TX:
      if( ef_vi_transmit_unbundle(&(cs->vi), &evs[i], ids) )
        cs->pio_in_use = false;
      break;
    default:
      fprintf(stderr, "ERROR: unexpected event "EF_EVENT_FMT"\n",
              EF_EVENT_PRI_ARG(evs[i]));
      TEST(0);
      break;
    }
}


static int poll_udp_rx(struct client_state* cs)
{
  int rc = recv(cs->udp_sock, cs->recv_buf,
                sizeof(cs->recv_buf) - 1, MSG_DONTWAIT);
  if( rc >= 0 ) {
    cs->recv_buf[rc] = '\0';
    return strncmp(cs->recv_buf, "hit me", 6) == 0;
  }
  else if( rc == -1 && errno == EAGAIN )
    return -1;
  else
    TEST(0);
}


static void normal_send(struct client_state* cs)
{
  if( cs->send_is_delegated ) {
    TRY( onload_delegated_send_cancel(cs->tcp_sock) );
    cs->send_is_delegated = false;
  }

  ssize_t rc = send(cs->tcp_sock, cs->msg_buf, cs->msg_len, 0);
  if( rc != cs->msg_len )
    fprintf(stderr, "normal_send: len=%d rc=%d errno=%d pio_in_use=%d\n",
            cs->msg_len, (int) rc, errno, cs->pio_in_use);
  TEST( rc == cs->msg_len );
  ++(cs->n_normal_sends);
}


/**********************************************************************/
/* Below is the most interesting bit of code in the app */
/**********************************************************************/

/* Prepare to do a delegated send.  You want to try to call this out
 * of the critical path of sending or else you will not be getting
 * very much benefit from the API.
 *
 * In this app, we call this function right after having done the
 * previous send and while we wait for the next ping from the client.
 *
 * This function can be called speculatively.  If later on you decide
 * that you don't want to do a delegated send, you can call
 * onload_delegated_send_cancel().
 */
static void delegated_prepare(struct client_state* s)
{
  /* Prepare to do a delegated send: Tell Onload how much data we might
   * send, and retrieve the current packet headers.  In this sample
   * application we always send a message of length s->msg_len, but we pass
   * a larger value to onload_delegated_send_prepare() just to show that
   * this is possible.
   */
  s->ods.headers = s->pkt_buf;
  s->ods.headers_len = MAX_ETH_HEADERS + MAX_IP_TCP_HEADERS;
  TEST( onload_delegated_send_prepare(s->tcp_sock, s->msg_len * 2, 0, &(s->ods))
          == ONLOAD_DELEGATED_SEND_RC_OK );
  s->send_is_delegated = true;

  /* We've probably put the headers in the wrong place, because we've
   * allowed enough space for worst-case headers (including VLAN tag and
   * TCP options).  Move the headers up so that the end of the headers
   * meets the start of the message.
   */
  s->ods.headers = s->msg_buf - s->ods.headers_len;
  memmove(s->ods.headers, s->pkt_buf, s->ods.headers_len);

  /* If we want to send more than MSS (maximum segment size), we will have
   * to segment the message into multiple packets.  We do not handle that
   * case in this demo app.
   */
  TEST( s->msg_len <= s->ods.mss );

  /* Figure out how much we are actually allowed to send. */
  int allowed_to_send = min(s->ods.send_wnd, s->ods.cong_wnd);
  allowed_to_send = min(allowed_to_send, s->ods.mss);

  if( s->msg_len <= allowed_to_send ) {
    s->pio_pkt_len = s->ods.headers_len + s->msg_len;
    onload_delegated_send_tcp_update(&(s->ods), s->msg_len, 1);
    TRY( ef_pio_memcpy(&(s->vi), s->ods.headers, 0, s->pio_pkt_len) );
  }
  else {
    /* We can't send at the moment, due to congestion window or receive
     * window being closed (or message larger than MSS).  Cancel the
     * delegated send and use normal send instead.
     */
    TRY(onload_delegated_send_cancel(s->tcp_sock));
    s->pio_pkt_len = 0;
    s->send_is_delegated = false;
  }
}


static void delegated_send(struct client_state* cs)
{
  /* Fast path send: */
  TRY( ef_vi_transmit_pio(&(cs->vi), 0, cs->pio_pkt_len, 0) );
  cs->pio_pkt_len = 0;
  cs->pio_in_use = 1;

  /* Now tell Onload what we've sent.  It needs to know so that it can
   * update internal state (eg. sequence numbers) and take a copy of the
   * payload sent so that it can be retransmitted if needed.
   *
   * NB. This does not have to happen immediately after the delegated send
   * (and is not part of the critical path) but should be done soon after.
   */
  struct iovec iov;
  iov.iov_len  = cs->msg_len;
  iov.iov_base = cs->msg_buf;
  TRY( onload_delegated_send_complete(cs->tcp_sock, &iov, 1, 0) );

  ++(cs->n_delegated_sends);
}


static void ev_loop_sock(struct client_state* cs)
{
  while( 1 ) {
    /* Spend most of our time polling the UDP socket, since that is the
     * latency sensitive path.
     */
    int i;
    for( i = 0; i < 100; ++i ) {
      if( poll_udp_rx(cs) > 0 ) {
        if( cs->pio_pkt_len )
          delegated_send(cs);
        else
          normal_send(cs);
      }
    }

    /* Less often poll ef_vi to pick-up TX completions, get ready for sends
     * and poll for TCP receives.
     */
    evq_poll(cs);
    if( ! cs->pio_in_use && (cs->alarm || ! cs->pio_pkt_len) ) {
      /* Get ready for the next delegated send (or refresh headers)... */
      delegated_prepare(cs);
      cs->alarm = false;
    }
    if( recv(cs->tcp_sock, cs->recv_buf,
             sizeof(cs->recv_buf), MSG_DONTWAIT) == 0 )
      break;
  }

  if( cs->pio_pkt_len )
    TRY(onload_delegated_send_cancel(cs->tcp_sock));
  close(cs->tcp_sock);

  printf("n_normal_sends: %u\n", cs->n_normal_sends);
  printf("n_delegated_sends: %u\n", cs->n_delegated_sends);
}


/**********************************************************************
 * The alarm thread just sets a flag periodically to remind the event loop
 * to refresh the headers.
 *
 * (In a real application you would want this thread to run on a junk
 * core).
 */

static void* alarm_thread(void* arg)
{
  struct client_state* cs = (void*) arg;
  while( 1 ) {
    usleep(cs->alarm_usec);
    cs->alarm = true;
  }
  return NULL;
}


/**********************************************************************
 * Initialisation code follows...
 */

static void ef_vi_init(struct client_state* cs)
{
  int ifindex;

  cs->pio_pkt_len = 0;
  cs->pio_in_use = ! cfg_delegated;
  TRY( sock_get_ifindex(cs->tcp_sock, &ifindex) );
  TRY( ef_driver_open(&(cs->dh)) );
  TRY( ef_pd_alloc(&(cs->pd), cs->dh, ifindex, EF_PD_DEFAULT) );
  TRY( ef_vi_alloc_from_pd(&(cs->vi), cs->dh, &(cs->pd), cs->dh,
                           -1, 0,-1, NULL, -1, EF_VI_FLAGS_DEFAULT) );
  TRY( ef_pio_alloc(&(cs->pio), cs->dh, &(cs->pd), -1, cs->dh));
  TRY( ef_pio_link_vi(&(cs->pio), cs->dh, &(cs->vi), cs->dh));
}


static void init(struct client_state* cs, const char* mcast_intf,
                 const char* server, const char* port)
{
  cs->alarm = false;
  cs->alarm_usec = 20000;
  cs->msg_len = cfg_tx_size;
  cs->msg_buf = cs->pkt_buf + MAX_ETH_HEADERS + MAX_IP_TCP_HEADERS;

  /* Create TCP socket, connect to server, give it configuration. */
  TRY( cs->tcp_sock = mk_socket(0, SOCK_STREAM, connect, server, port) );
  int one = 1;
  TRY( setsockopt(cs->tcp_sock, SOL_TCP, TCP_NODELAY, &one, sizeof(one)) );
  sock_put_int(cs->tcp_sock, cfg_tx_size);
  sock_put_int(cs->tcp_sock, cfg_rx_size);

  /* Create UDP socket, bind, join multicast group. */
  TRY( cs->udp_sock = mk_socket(0, SOCK_DGRAM, bind,
                                cfg_mcast_addr, cfg_port) );
  if( mcast_intf != NULL ) {
    struct ip_mreqn mreqn;
    TEST( inet_aton(cfg_mcast_addr, &mreqn.imr_multiaddr) );
    mreqn.imr_address.s_addr = htonl(INADDR_ANY);
    TEST( (mreqn.imr_ifindex = if_nametoindex(mcast_intf)) != 0 );
    TRY( setsockopt(cs->udp_sock, SOL_IP, IP_ADD_MEMBERSHIP,
                    &mreqn, sizeof(mreqn)) );
  }

  ef_vi_init(cs);
}


static void usage_msg(FILE* f)
{
  fprintf(f, "\nusage:\n");
  fprintf(f, "  efdelegated_client [options] <mcast-interface> <server>\n");
  fprintf(f, "\noptions:\n");
  fprintf(f, "  -h                - print usage info\n");
  fprintf(f, "  -d                - use delegated sends API to send\n");
  fprintf(f, "  -s <msg-size>     - TX (TCP) message size\n");
  fprintf(f, "  -r <msg-size>     - RX (UDP) message size\n");
  fprintf(f, "  -p <port>         - set TCP/UDP port number\n");
  fprintf(f, "\n");
}


static void usage_err(void)
{
  usage_msg(stderr);
  exit(1);
}


int main(int argc, char* argv[])
{
  int c;

  while( (c = getopt(argc, argv, "hs:r:dp:")) != -1 )
    switch( c ) {
    case 'h':
      usage_msg(stdout);
      exit(0);
      break;
    case 'd':
      cfg_delegated = 1;
      break;
    case 's':
      cfg_tx_size = atoi(optarg);
      break;
    case 'r':
      cfg_rx_size = atoi(optarg);
      break;
    case 'p':
      cfg_port = optarg;
      break;
    case '?':
      usage_err();
      break;
    default:
      TEST(0);
      break;
    }
  argc -= optind;
  argv += optind;
  if( argc != 2 )
    usage_err();
  const char* mcast_intf = argv[0];
  const char* server = argv[1];

  if( cfg_delegated && ! onload_is_present() ) {
    fprintf(stderr, "ERROR: Must run with Onload to use delegated sends\n");
    exit(1);
  }

  struct client_state* cs = calloc(1, sizeof(*cs));
  init(cs, mcast_intf, server, cfg_port);
  pthread_t tid;
  TEST( pthread_create(&tid, NULL, alarm_thread, cs) == 0 );
  ev_loop_sock(cs);
  return 0;
}

/*! \cidoxg_end */
