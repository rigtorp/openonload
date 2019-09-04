/*
** Copyright 2005-2018  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of version 2.1 of the GNU Lesser General Public
** License as published by the Free Software Foundation.
**
** This library is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Lesser General Public License for more details.
*/

/****************************************************************************
 * Copyright 2017-2018: Solarflare Communications Inc,
 *                      7505 Irvine Center Drive, Suite 100
 *                      Irvine, CA 92618, USA
 *
 * Maintained by Solarflare Communications
 *  <linux-xen-drivers@solarflare.com>
 *  <onload-dev@solarflare.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************
 */

/**************************************************************************\
*//*! \file
** \author    Solarflare Communications, Inc.
** \brief     Checksum utility functions.
** \date      2018/11/06
** \copyright Copyright &copy; 2018 Solarflare Communications, Inc. All
**            rights reserved. Solarflare, OpenOnload and EnterpriseOnload
**            are trademarks of Solarflare Communications, Inc.
*//*
\**************************************************************************/

#ifndef __EFAB_CHECKSUM_H__
#define __EFAB_CHECKSUM_H__

#include <etherfabric/base.h>

#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! \brief Calculate the checksum for an IP header
**
** \param ip The IP header to use.
**
** \return The checksum of the IP header.
**
** Calculate the checksum for an IP header.  The IP header must be populated
** (with the exception of the checksum field itself, which is ignored) before
** calling this function.
*/
extern uint32_t ef_ip_checksum(const struct iphdr* ip);

/*! \brief Calculate the checksum for a UDP packet
**
** \param ip     The IP header for the packet.
** \param udp    The UDP header for the packet.
** \param iov    Start of the iovec array describing the UDP payload.
** \param iovlen Length of the iovec array.
**
** \return The checksum of the UDP packet.
**
** Calculate the checksum for a UDP packet.  The UDP header must be populated
** (with the exception of the checksum field itself, which is ignored) before
** calling this function.
*/
extern uint32_t
ef_udp_checksum(const struct iphdr* ip, const struct udphdr* udp,
                const struct iovec* iov, int iovlen);
                
/*! \brief Calculate the checksum for a TCP packet
**
** \param ip     The IP header for the packet.
** \param tcp    The TCP header for the packet.
** \param iov    Start of the iovec array describing the TCP payload.
** \param iovlen Length of the iovec array.
**
** \return The checksum of the TCP packet.
**
** Calculate the checksum for a TCP packet.  The TCP header must be populated
** (with the exception of the checksum field itself, which is ignored) before
** calling this function.
*/
extern uint32_t
ef_tcp_checksum(const struct iphdr* ip, const struct tcphdr* tcp,
                const struct iovec* iov, int iovlen);

#ifdef __cplusplus
}
#endif

#endif /* __EFAB_CHECKSUM_H__ */
