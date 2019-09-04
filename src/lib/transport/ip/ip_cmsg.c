/*
** Copyright 2005-2018  Solarflare Communications Inc.
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

/**************************************************************************\
*//*! \file
** <L5_PRIVATE L5_SOURCE>
** \author  Evgeniy Vladimirovich Bobkov <kavri@oktetlabs.ru>
**  \brief  Control messages / ancillary data.
**   \date  2004/12/23
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/
  
/*! \cidoxg_lib_transport_ip */
  
#include "ip_internal.h"


#define LPF "IP CMSG "

/* On some glibc versions CMSG_NXTHDR looks into unitialized data and
 * may return null */
#define NEED_A_WORKAROUND_FOR_GLIBC_BUG_13500



/**
 * Put a portion of ancillary data into msg ancillary data buffer.
 *
 * man cmsg says: Use CMSG_FIRSTHDR() on the msghdr to get the first
 * control message and CMSG_NEXTHDR() to get all subsequent ones.  In
 * each control message, initialize cmsg_len (with CMSG_LEN()), the
 * other cmsghdr header fields, and the data portion using
 * CMSG_DATA().  Finally, the msg_controllen field of the msghdr
 * should be set to the sum of the CMSG_SPACE() of the length of all
 * control messages in the buffer.
 */
void ci_put_cmsg(struct cmsg_state *cmsg_state,
                 int level, int type, socklen_t len, const void* data)
{

  int data_space, data_len = len;

  /* Calls to CMSG_FIRSTHDR and CMSG_NEXTHDR already check that there
   * is enough space for the cmsghdr itself, so just need to check
   * that it is != NULL here
   */
  if( *cmsg_state->p_msg_flags & MSG_CTRUNC )
    return;
  if( cmsg_state->cm == NULL ) {
    *cmsg_state->p_msg_flags |= MSG_CTRUNC;
    return;
  }

  data_space = ((unsigned char*)cmsg_state->msg->msg_control + 
                cmsg_state->msg->msg_controllen) - 
    (unsigned char*)CMSG_DATA(cmsg_state->cm);
  if( data_space < 0 ) {
    *cmsg_state->p_msg_flags |= MSG_CTRUNC;
    return;
  }

  if( data_len > data_space ) {
    *cmsg_state->p_msg_flags |= MSG_CTRUNC;
    data_len = data_space;
  }

  cmsg_state->cm->cmsg_len   = CMSG_LEN(data_len);
  cmsg_state->cm->cmsg_level = level;
  cmsg_state->cm->cmsg_type  = type;

  memcpy(CMSG_DATA(cmsg_state->cm), data, data_len);

  cmsg_state->cmsg_bytes_used += CMSG_SPACE(data_len);

  if( *cmsg_state->p_msg_flags & MSG_CTRUNC )
    return;

#if !defined(NEED_A_WORKAROUND_FOR_GLIBC_BUG_13500) || defined(__KERNEL__)
  cmsg_state->cm = CMSG_NXTHDR(cmsg_state->msg, cmsg_state->cm);
#else
  /* space has been already checked so just updating ptr */
  cmsg_state->cm = (struct cmsghdr*)(((char*)(cmsg_state->cm))
      + (CMSG_ALIGN(cmsg_state->cm->cmsg_len)));
#endif
}


#ifndef __KERNEL__

/**
 * Put an IP_PKTINFO control message into msg ancillary data buffer.
 */
static void ip_cmsg_recv_pktinfo(ci_netif* netif, ci_udp_state* us,
                                 const ci_ip_pkt_fmt* pkt,
                                 struct cmsg_state *cmsg_state)
{
  struct in_pktinfo info;
  ci_uint32 addr;
  ci_hwport_id_t hwport;
  const uint8_t* dst_mac;

  addr = oo_ip_hdr_const(pkt)->ip_daddr_be32;

  /* Set the ifindex the pkt was received at. */
  dst_mac = oo_ether_hdr_const(pkt)->ether_dhost;

  /* If the last packet was the same, then we can use the caches info */
  if( pkt->intf_i == us->ip_pktinfo_cache.intf_i &&
      pkt->vlan == us->ip_pktinfo_cache.vlan &&
      addr == us->ip_pktinfo_cache.daddr &&
      memcmp(dst_mac, us->ip_pktinfo_cache.dmac, ETH_ALEN) == 0) {
    ci_put_cmsg(cmsg_state, IPPROTO_IP, IP_PKTINFO, sizeof(struct in_pktinfo),
                &us->ip_pktinfo_cache.ipi);
    return;
  }

  if( dst_mac[0] == 1 && dst_mac[1] == 0 && dst_mac[2] == 0x5e )
    dst_mac = NULL;
  hwport = netif->state->intf_i_to_hwport[pkt->intf_i];
  info.ipi_ifindex = oo_cp_hwport_vlan_to_ifindex(netif->cplane,
                                                  hwport, pkt->vlan,
                                                  dst_mac);
  info.ipi_addr.s_addr = addr;
  if( info.ipi_ifindex <= 0 ) {
    LOG_E(ci_log("%s: oo_cp_hwport_vlan_to_ifindex(intf_i=%d => hwport=%d, "
                 "vlan_id=%d mac="CI_MAC_PRINTF_FORMAT" ) failed",
                 __FUNCTION__, pkt->intf_i, hwport, pkt->vlan,
                 CI_MAC_PRINTF_ARGS(oo_ether_hdr_const(pkt)->ether_dhost)));
  }

  /* RFC1122: The specific-destination address is defined to be the
   * destination address in the IP header unless the header contains a
   * broadcast or multicast address, in which case the specific-destination
   * is an IP address assigned to the physical interface on which the
   * datagram arrived.
   *
   * Onload does not work with broadcast (?), so we check for multicast
   * only.
   */
  if( CI_IP_IS_MULTICAST(oo_ip_hdr_const(pkt)->ip_daddr_be32) ) {
    info.ipi_spec_dst.s_addr = oo_cp_ifindex_to_ip(netif->cplane,
                                                   info.ipi_ifindex);
  }
  else
    info.ipi_spec_dst.s_addr = oo_ip_hdr_const(pkt)->ip_daddr_be32;

  ci_put_cmsg(cmsg_state, IPPROTO_IP, IP_PKTINFO, sizeof(info), &info);

  /* Cache the info: */
  us->ip_pktinfo_cache.intf_i = pkt->intf_i;
  us->ip_pktinfo_cache.vlan = pkt->vlan;
  us->ip_pktinfo_cache.daddr = oo_ip_hdr_const(pkt)->ip_daddr_be32;
  memcpy(&us->ip_pktinfo_cache.dmac, oo_ether_hdr_const(pkt)->ether_dhost,
         ETH_ALEN);
  memcpy(&us->ip_pktinfo_cache.ipi, &info, sizeof(info));
}

/**
 * Put an IP_RECVTTL control message into msg ancillary data buffer.
 */
ci_inline void ip_cmsg_recv_ttl(const ci_ip_pkt_fmt *pkt, 
                                struct cmsg_state *cmsg_state)
{
  int ttl = oo_ip_hdr_const(pkt)->ip_ttl;

  ci_put_cmsg(cmsg_state, IPPROTO_IP, IP_TTL, sizeof(ttl), &ttl);
}

/**
 * Put an IP_RECVTOS control message into msg ancillary data buffer.
 */
ci_inline void ip_cmsg_recv_tos(const ci_ip_pkt_fmt *pkt, 
                                struct cmsg_state *cmsg_state)
{
  int tos = oo_ip_hdr_const(pkt)->ip_tos;

  ci_put_cmsg(cmsg_state, IPPROTO_IP, IP_TOS, sizeof(tos), &tos);
}

/**
 * Put a SO_TIMESTAMP control message into msg ancillary data buffer.
 */
void ip_cmsg_recv_timestamp(ci_netif *ni, ci_uint64 timestamp, 
                                      struct cmsg_state *cmsg_state)
{
  struct timespec ts;
  struct timeval tv;

  ci_udp_compute_stamp(ni, timestamp, &ts);
  tv.tv_sec = ts.tv_sec;
  tv.tv_usec = ts.tv_nsec / 1000;

  ci_put_cmsg(cmsg_state, SOL_SOCKET, SO_TIMESTAMP, sizeof(tv), &tv);
}

/**
 * Put a SO_TIMESTAMPNS control message into msg ancillary data buffer.
 */
void ip_cmsg_recv_timestampns(ci_netif *ni, ci_uint64 timestamp, 
                                        struct cmsg_state *cmsg_state)
{
  struct timespec ts;

  ci_udp_compute_stamp(ni, timestamp, &ts);

  ci_put_cmsg(cmsg_state, SOL_SOCKET, SO_TIMESTAMPNS, sizeof(ts), &ts);
}


#if CI_CFG_TIMESTAMPING
/**
 * Put a SO_TIMESTAMPING control message into msg ancillary data buffer.
 */
void ip_cmsg_recv_timestamping(ci_netif *ni,
      ci_uint64 sys_timestamp, struct timespec* hw_timestamp,
      int flags, struct cmsg_state *cmsg_state)
{
  struct {
    struct timespec systime;
    struct timespec hwtimetrans;
    struct timespec hwtimeraw;
  } ts;

  int c_flags = 0;

  if( hw_timestamp->tv_sec != 0 )
    c_flags = flags & (ONLOAD_SOF_TIMESTAMPING_RAW_HARDWARE
                      | ONLOAD_SOF_TIMESTAMPING_SYS_HARDWARE);
  if( sys_timestamp != 0 )
    c_flags |= flags & ONLOAD_SOF_TIMESTAMPING_SOFTWARE;

  memset(&ts, 0, sizeof(ts));
  if( c_flags & ONLOAD_SOF_TIMESTAMPING_SOFTWARE )
    ci_udp_compute_stamp(ni, sys_timestamp, &ts.systime);
  if( c_flags & ONLOAD_SOF_TIMESTAMPING_RAW_HARDWARE )
    ts.hwtimeraw = *hw_timestamp;
  if( c_flags & ONLOAD_SOF_TIMESTAMPING_SYS_HARDWARE )
    ts.hwtimetrans = *hw_timestamp;

  ci_put_cmsg(cmsg_state, SOL_SOCKET, ONLOAD_SO_TIMESTAMPING, sizeof(ts), &ts);
}
#endif

void ci_ip_cmsg_finish(struct cmsg_state* cmsg_state)
{
#ifndef NEED_A_WORKAROUND_FOR_GLIBC_BUG_13500
  /* This is to ensure that a client unaware of the bug
   * will not miss last cmsg.
   */
  if( (cmsg_state->cm) &&
      ( ((char*)((&cmsg_state->cm->cmsg_len) + 1))
          - ((char*)cmsg_state->msg->msg_control)
        <= cmsg_state->msg->msg_controllen ) )
    cmsg_state->cm->cmsg_len = 0;
#endif

  cmsg_state->msg->msg_controllen = cmsg_state->cmsg_bytes_used;
}

/**
 * Fill in the msg ancillary data buffer with all control messages
 * according to cmsg_flags the user has set beforehand.
 */
void ci_ip_cmsg_recv(ci_netif* ni, ci_udp_state* us, const ci_ip_pkt_fmt *pkt,
                     struct msghdr *msg, int netif_locked, int *p_msg_flags)
{
  unsigned flags = us->s.cmsg_flags;
  struct cmsg_state cmsg_state;

  cmsg_state.msg = msg;
  cmsg_state.cmsg_bytes_used = 0;
  cmsg_state.cm = CMSG_FIRSTHDR(msg);
  cmsg_state.p_msg_flags = p_msg_flags;

  if( pkt->flags & CI_PKT_FLAG_RX_INDIRECT )
    pkt = PKT_CHK_NML(ni, pkt->frag_next, netif_locked);

  if (flags & CI_IP_CMSG_PKTINFO) {
    ++us->stats.n_rx_pktinfo;
    ip_cmsg_recv_pktinfo(ni, us, pkt, &cmsg_state);
  }

  if (flags & CI_IP_CMSG_TTL)
    ip_cmsg_recv_ttl(pkt, &cmsg_state);

  if (flags & CI_IP_CMSG_TOS)
    ip_cmsg_recv_tos(pkt, &cmsg_state);

  if( flags & CI_IP_CMSG_TIMESTAMPNS )
    ip_cmsg_recv_timestampns(ni, pkt->tstamp_frc, &cmsg_state);
  else /* SO_TIMESTAMP gets ignored when SO_TIMESTAMPNS one is set */
    if( flags & CI_IP_CMSG_TIMESTAMP )
      ip_cmsg_recv_timestamp(ni, pkt->tstamp_frc, &cmsg_state);

#if CI_CFG_TIMESTAMPING
  if( flags & CI_IP_CMSG_TIMESTAMPING ) {
    int flags = us->s.timestamping_flags;
    if( flags & (ONLOAD_SOF_TIMESTAMPING_RAW_HARDWARE
                | ONLOAD_SOF_TIMESTAMPING_SYS_HARDWARE
                | ONLOAD_SOF_TIMESTAMPING_SOFTWARE) ) {
      struct timespec rx_hw_stamp;
      rx_hw_stamp.tv_sec = pkt->pf.udp.rx_hw_stamp.tv_sec;
      rx_hw_stamp.tv_nsec = pkt->pf.udp.rx_hw_stamp.tv_nsec;
      if( ! (rx_hw_stamp.tv_nsec & CI_IP_PKT_HW_STAMP_FLAG_IN_SYNC) )
          flags &= ~ONLOAD_SOF_TIMESTAMPING_SYS_HARDWARE;
      ip_cmsg_recv_timestamping(ni, pkt->tstamp_frc, &rx_hw_stamp,
                                flags, &cmsg_state);
    }
  }
#endif

  ci_ip_cmsg_finish(&cmsg_state);
}

#endif /* !__KERNEL__ */


/**
 * Find out all control messages the user has provided with msg.
 *
 * \param info_out    Must be a valid pointer.
 */
int ci_ip_cmsg_send(const struct msghdr* msg, struct in_pktinfo** info_out)
{
  struct cmsghdr *cmsg;

  /* NB. I don't think CMSG_NXTHDR() modifies [*msg], but for some
   * reason it takes a non-const arg.
   */
  for( cmsg = CMSG_FIRSTHDR(msg); cmsg;
       cmsg = CMSG_NXTHDR((struct msghdr*) msg, cmsg) ) {

    if( cmsg->cmsg_len < sizeof(struct cmsghdr) ||
        (socklen_t)(((char*)cmsg - (char*)msg->msg_control)
                    + cmsg->cmsg_len) > msg->msg_controllen )
      return -EINVAL;

    if( cmsg->cmsg_level != IPPROTO_IP )
      continue;

    switch( cmsg->cmsg_type ) {
    case IP_RETOPTS:
      /* TODO: implementation required */
      return -ENOPROTOOPT;

    case IP_PKTINFO:
      if (cmsg->cmsg_len != CMSG_LEN(sizeof(struct in_pktinfo)))
        return -EINVAL;
      *info_out = (struct in_pktinfo *)CMSG_DATA(cmsg);
      break;

    default:
      return -EINVAL;
    }
  }

  return 0;
}

/*! \cidoxg_end */
