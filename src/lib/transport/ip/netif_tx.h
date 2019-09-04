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

#ifndef __NETIF_TX_H__
#define __NETIF_TX_H__


/**********************************************************************
 * Sending packet helper
 */

ci_inline void ci_netif_pkt_tx_assert_len(ci_netif* ni, ci_ip_pkt_fmt* pkt,
                                          unsigned n)
{
#ifndef NDEBUG
  ci_ip_pkt_fmt* first = pkt;
  int i, t = 0;
  for( i = 0; ; ) {
    t += pkt->buf_len;
    ci_assert_le(t, first->pay_len);
    if( ++i == n )
      break;
    pkt = PKT_CHK(ni, pkt->frag_next);
  }
  ci_assert_equal(t, first->pay_len);
#endif
}


ci_inline void ci_netif_pkt_to_iovec(ci_netif* ni, ci_ip_pkt_fmt* pkt,
                                     ef_iovec* iov, unsigned iovlen)
{
  int i, intf_i = pkt->intf_i;
  unsigned n = pkt->n_buffers;

  ci_assert_lt((unsigned) intf_i, CI_CFG_MAX_INTERFACES);
  ci_assert_ge(iovlen, n);

#if CI_CFG_NETIF_HARDEN
  if( n > iovlen )
    n = iovlen;
#endif

  ci_netif_pkt_tx_assert_len(ni, pkt, n);

  for( i = 0; ; ) {
    iov[i].iov_base = pkt->dma_addr[intf_i] + pkt->pkt_start_off;
    iov[i].iov_len = pkt->buf_len;
    if( ++i == n )
      return;
    pkt = PKT_CHK(ni, pkt->frag_next);
  }
}


ci_inline unsigned ci_netif_pkt_to_host_iovec(ci_netif* ni,
                                              ci_ip_pkt_fmt* pkt,
                                              struct iovec* iov,
                                              unsigned iovlen)
{
  unsigned n = pkt->n_buffers;
  int i;
  unsigned total_length = 0;

  ci_assert_lt((unsigned) pkt->intf_i, CI_CFG_MAX_INTERFACES);
  ci_assert_ge(iovlen, n);

#if CI_CFG_NETIF_HARDEN
  if( n > iovlen )
    n = iovlen;
#endif

  ci_netif_pkt_tx_assert_len(ni, pkt, n);

  for( i = 0; ; ) {
    iov[i].iov_base = pkt->dma_start + pkt->pkt_start_off;
    iov[i].iov_len = pkt->buf_len;
    total_length += pkt->buf_len;
    if( ++i == n )
      return total_length;
    pkt = PKT_CHK(ni, pkt->frag_next);
  }
}


/**********************************************************************
 * CTPIO.
 */

ci_inline int /*bool*/ ci_netif_may_ctpio(ci_netif* ni, int intf_i,
                                          size_t frame_len)
{
#if CI_CFG_USE_CTPIO && ! defined(__KERNEL__)
  /* Only use CTPIO if not desisted, frame length is below threshold, and
   * TX ring is not very full.  (It is essential that we have room to post
   * a fallback).
   */
  const ci_netif_state_nic_t* nsn = &ni->state->nic[intf_i];
  const ci_netif_nic_t* nnic = &ni->nic_hw[intf_i];
  int max_fill = ef_vi_transmit_capacity(&nnic->vi) >> 2;
  return frame_len <= nsn->ctpio_frame_len_check &&
         ef_vi_transmit_fill_level(&nnic->vi) < max_fill;
#else
  return 0;
#endif
}

ci_inline void ci_netif_ctpio_desist(ci_netif* ni, int intf_i)
{
#if CI_CFG_CTPIO
  ci_netif_state_nic_t* nsn = &ni->state->nic[intf_i];
  nsn->ctpio_frame_len_check = 0;
#endif
}

ci_inline void ci_netif_ctpio_resume(ci_netif* ni, int intf_i)
{
#if CI_CFG_CTPIO
  ci_netif_state_nic_t* nsn = &ni->state->nic[intf_i];
  nsn->ctpio_frame_len_check = nsn->ctpio_max_frame_len;
#endif
}

/**********************************************************************
 * DMA queues.
 */

/* Moves packets from the overflow queue to the hardware ring iff the
 * hardware queue has lots of space.
 */
extern void ci_netif_dmaq_shove1(ci_netif*, int intf_i);

/* Moves packets from the overflow queue to the hardware ring if the
 * hardware queue has at least space for one packet.
 */
extern void ci_netif_dmaq_shove2(ci_netif*, int intf_i, int is_fresh);


#define ci_netif_dmaq(ni, nic_i)  (&(ni)->state->nic[nic_i].dmaq)


#define ci_netif_dmaq_is_empty(ni, nic_i)               \
        oo_pktq_is_empty(ci_netif_dmaq((ni), (nic_i)))

#define ci_netif_dmaq_not_empty(ni, nic_i)               \
        oo_pktq_not_empty(ci_netif_dmaq((ni), (nic_i)))


#define __ci_netif_dmaq_put(ni, q, pkt)                         \
  do {                                                          \
    __oo_pktq_put((ni), (q), (pkt), netif.tx.dmaq_next);        \
    /* ?? pkt->usage += CI_CFG_BUFFER_TRACE_DMAQIN; */          \
  } while(0)


ci_inline void ci_netif_dmaq_and_vi_for_pkt(ci_netif* ni, ci_ip_pkt_fmt* pkt,
                                            oo_pktq** dmaq, ef_vi** vi) {
  *dmaq = &ni->state->nic[pkt->intf_i].dmaq;
  *vi = &ni->nic_hw[pkt->intf_i].vi;
}


#define __ci_netif_dmaq_insert_prep_pkt(ni, pkt)                        \
  do {                                                                  \
    ci_assert( ! ((pkt)->flags & CI_PKT_FLAG_TX_PENDING) );             \
    (pkt)->flags |= CI_PKT_FLAG_TX_PENDING;                             \
    ++(ni)->state->nic[(pkt)->intf_i].tx_dmaq_insert_seq;               \
    (ni)->state->nic[(pkt)->intf_i].tx_bytes_added+=TX_PKT_LEN(pkt);    \
    if( oo_tcpdump_check(ni, pkt, (pkt)->intf_i) ) {                    \
      ci_frc64(&((pkt)->tstamp_frc));                                   \
      oo_tcpdump_dump_pkt(ni, pkt);                                     \
    }                                                                   \
  } while(0)


#define __ci_netif_dmaq_insert_prep_pkt_warm_undo(ni, pkt)              \
  do {                                                                  \
    (pkt)->flags &=~ (CI_PKT_FLAG_TX_PENDING | CI_PKT_FLAG_MSG_WARM);   \
    --(ni)->state->nic[(pkt)->intf_i].tx_dmaq_insert_seq;               \
    (ni)->state->nic[(pkt)->intf_i].tx_bytes_added-=TX_PKT_LEN(pkt);    \
    ci_netif_pkt_release(ni, pkt);                                      \
  } while(0)


#endif  /* __NETIF_TX_H__ */
