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

/* This header describes the interface between the open source parts
 * of Onload and the binary-only control plane server.
 *
 * We use an md5sum over certain headers to ensure that userland and
 * kernel drivers are built against a compatible interface. The
 * control plane server and its clients will verify this hash against
 * the kernel module and refuse to start if there is a version
 * mismatch.
 *
 * Users should therefore not modify these headers because the
 * supplied control plane server will refuse to operate with the
 * resulting module.
 */

/* Cplane interface to be used from Onload */
#ifndef __TOOLS_CPLANE_ONLOAD_H__
#define __TOOLS_CPLANE_ONLOAD_H__

#include <cplane/mib.h>
#include <cplane/ioctl.h>
#include <ci/tools.h>

#ifdef __KERNEL__
#include <onload/cplane_driver.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __KERNEL__
#ifndef __CI_UL_SYSCALL_UNIX_H__
/* libonload provides this; cp_client provides this in another way */
extern int (* ci_sys_ioctl)(int, long unsigned int, ...);
#endif

static inline int cp_ioctl(int fd, long unsigned int op, void* arg)
{
  int saved_errno = errno;
  int rc = ci_sys_ioctl(fd, op, arg);
  if( rc == 0 )
    return 0;
  rc = -errno;
  errno = saved_errno;
  return rc;
}
#endif


#define CP_VERLOCK_START(var_, mib_, cp_) \
 _verlock_again: \
  (var_) = OO_ACCESS_ONCE(*(cp_)->mib[0].version);    \
  (mib_) = &(cp_)->mib[(var_) & 1]; \

#define CP_VERLOCK_STOP(var_, mib_) \
  if( (var_) != OO_ACCESS_ONCE(*(mib_)->version)) \
    goto _verlock_again;

enum { CP_CHSUM_STR_LEN  = 32 };

typedef struct oo_cp_version_check_s {
  char                    in_cp_intf_ver[CP_CHSUM_STR_LEN + 1];
} oo_cp_version_check_t;

extern oo_cp_version_check_t oo_cplane_api_version;

enum cp_sync_mode {
  CP_SYNC_NONE  = 0,
  CP_SYNC_LIGHT = 1,
  CP_SYNC_DUMP  = 2
};

#ifndef __KERNEL__
struct oo_cplane_handle {
  struct cp_mibs mib[2];
  int fd;
  uint32_t bytes;
};

int oo_cp_create(int fd, struct oo_cplane_handle* cp,
                 enum cp_sync_mode mode);
void oo_cp_destroy(struct oo_cplane_handle* cp);

#else

#include <onload/cplane_driver_handle.h>

extern int
__oo_cp_arp_confirm(struct oo_cplane_handle* cp, cicp_verinfo_t* verinfo);
extern int
__oo_cp_arp_resolve(struct oo_cplane_handle* cp, cicp_verinfo_t* verinfo);
extern cicp_hwport_mask_t
oo_cp_get_licensed_hwports(struct oo_cplane_handle*);
extern int oo_cp_get_acceleratable_llap_count(struct oo_cplane_handle*);
extern int oo_cp_get_acceleratable_ifindices(struct oo_cplane_handle*,
                                             ci_ifid_t* ifindices,
                                             int max_count);
#endif

extern int
oo_cp_get_hwport_properties(struct oo_cplane_handle*, ci_hwport_id_t hwport,
                            ci_uint8* out_mib_flags,
                            ci_uint32* out_oo_vi_flags_mask,
                            ci_uint32* out_efhw_flags_extra,
                            ci_uint8* out_pio_len_shift,
                            ci_uint32* out_ctpio_start_offset);


/* Initialize verinfo before the first use */
static inline void oo_cp_verinfo_init(cicp_verinfo_t* verinfo)
{
  verinfo->id = CICP_MAC_ROWID_BAD;
}

/* Confirm that the given ARP entry is valid (to be used with MSG_CONFIRM
 * or when TCP received a new ACK).
 * Fast exit of the inline function if the ARP entry is already fresh and
 * valid.
 * Fixme: do we want to pass current frc value as a parameter?  All the
 * callers probably have it.
 */
static inline void
oo_cp_arp_confirm(struct oo_cplane_handle* cp,
                  cicp_verinfo_t* verinfo)
{
  struct cp_mibs* mib = &cp->mib[0];
  struct cp_fwd_rw_row* fwd_rw;

  if( ! CICP_MAC_ROWID_IS_VALID(verinfo->id) ||
      ! cp_fwd_version_matches(mib, verinfo) )
    return;

  fwd_rw = cp_get_fwd_rw(mib, verinfo);
  if( ! (fwd_rw->flags & CICP_FWD_RW_FLAG_ARP_NEED_REFRESH) )
    return;
  ci_atomic32_and(&fwd_rw->flags, ~CICP_FWD_RW_FLAG_ARP_NEED_REFRESH);

#ifndef __KERNEL__
  cp_ioctl(cp->fd, OO_IOC_CP_ARP_CONFIRM, verinfo);
#else
  __oo_cp_arp_confirm(cp, verinfo);
#endif
}

/* Resolve an ARP entry.  In many cases it is not necessary, because ARP is
 * resolved when we send via OS.  However, it is always good to resolve ARP
 * at connect() time without waiting for send(). */
static inline void
oo_cp_arp_resolve(struct oo_cplane_handle* cp,
                  cicp_verinfo_t* verinfo)
{
  struct cp_mibs* mib = &cp->mib[0];

  ci_assert(CICP_MAC_ROWID_IS_VALID(verinfo->id));

  /* The most probable reason for verinfo to be invalid is ARP resolution.
   * If ARP is really resolved, then there is no need to go further.
   * After all, we always can resolve the ARP by sending a packet via OS. */
  if( cp_get_fwd_data(mib, verinfo)->arp_valid ||
      ! cp_fwd_version_matches(mib, verinfo) )
    return;

#ifndef __KERNEL__
  cp_ioctl(cp->fd, OO_IOC_CP_ARP_RESOLVE, verinfo);
#else
  __oo_cp_arp_resolve(cp, verinfo);
#endif
}

extern int
__oo_cp_route_resolve(struct oo_cplane_handle* cp,
                    cicp_verinfo_t* verinfo,
                    struct cp_fwd_key* req,
                    int/*bool*/ ask_server,
                    struct cp_fwd_data* data);

static inline int
oo_cp_verinfo_is_valid(struct oo_cplane_handle* cp,
                       cicp_verinfo_t* verinfo)
{
  struct cp_mibs* mib = &cp->mib[0];
  return verinfo->id != CICP_MAC_ROWID_BAD &&
         cp_fwd_version_matches(mib, verinfo);
}

#if defined(__KERNEL__)
int oo_op_route_resolve(struct oo_cplane_handle* cp,
                        struct cp_fwd_key* key);
#endif

/* Resolve the route, update the version info.
 *
 * Returns:
 *  1 if verinfo is valid;
 *  0 if route is resolved, new data and verinfo are filled in;
 *  -errno in case of error.
 */
static inline int
oo_cp_route_resolve(struct oo_cplane_handle* cp,
                    cicp_verinfo_t* verinfo,
                    struct cp_fwd_key* key,
                    struct cp_fwd_data* data)
{
  /* Are we lucky?  Is the verlock valid? */
  if( oo_cp_verinfo_is_valid(cp, verinfo) ) {
    struct cp_mibs* mib = &cp->mib[0];
    if( cp_get_fwd(mib, verinfo)->flags & CICP_FWD_FLAG_STALE )
      cp_get_fwd_rw(mib, verinfo)->frc_used = ci_frc64_get();
    memcpy(data, cp_get_fwd_data(mib, verinfo), sizeof(*data));
    ci_rmb();
    if( cp_fwd_version_matches(mib, verinfo) )
      return 1;
  }

  /* We are unlucky. Let's go via slow path. */
  return __oo_cp_route_resolve(cp, verinfo, key, 1, data);
}

/* Find the network interface by the incoming packet:
 * hwport + vlan => ifindex. */
static inline ci_ifid_t
oo_cp_hwport_vlan_to_ifindex(struct oo_cplane_handle* cp,
                             ci_hwport_id_t hwport, ci_uint16 vlan_id,
                             const uint8_t* mac)
{
  struct cp_mibs* mib = &cp->mib[0];
  cp_version_t version;
  ci_ifid_t ifindex = 0;
  cicp_rowid_t i;

  ci_assert_nequal(hwport, CI_HWPORT_ID_BAD);
  ci_assert_lt(hwport, mib->dim->hwport_max);

  CP_VERLOCK_START(version, mib, cp)

  for( i = 0; i < mib->dim->llap_max; i++ ) {
    if( (mib->llap[i].rx_hwports & cp_hwport_make_mask(hwport)) &&
        ! (mib->llap[i].encap.type & CICP_LLAP_TYPE_SLAVE) &&
        (mib->llap[i].encap.type & CICP_LLAP_TYPE_VLAN) == (vlan_id != 0) &&
        ( ! (mib->llap[i].encap.type & CICP_LLAP_TYPE_VLAN) ||
          mib->llap[i].encap.vlan_id == vlan_id ) &&
        (mac == NULL || memcmp(mac, mib->llap[i].mac, 6) == 0) ) {
      ifindex = mib->llap[i].ifindex;
      break;
    }
    if( cicp_llap_row_is_free(&mib->llap[i]) )
      break;
  }

  CP_VERLOCK_STOP(version, mib)

  return ifindex;

}


typedef int/*bool*/ (*oo_cp_ipif_check)(
        struct oo_cplane_handle* cp,
        cicp_ipif_row_t* ipif,
        void* data);

/* Find the network interface by an IP address.  Returns 1 if found,
 * 0 otherwise.  The "check" parameter is the local function which checks if
 * the interface parameters fulfil the caller requirements and saves any
 * additional ipif parameters if necessary.
 *
 * The "data" parameter is passed to the "check" callback.  It can be used
 * ot pass Onload stack handle or to store ipif row parameters ti make them
 * available to the caller.
 */
static inline int/*bool*/
oo_cp_find_ipif_by_ip(struct oo_cplane_handle* cp, ci_ip_addr_t ip,
                      oo_cp_ipif_check check, void* data)
{
  struct cp_mibs* mib;
  cicp_rowid_t id;
  cp_version_t version;
  int rc = 0;

  CP_VERLOCK_START(version, mib, cp)

  for( id = 0; id < mib->dim->ipif_max; id++ ) {
    if( cicp_ipif_row_is_free(&mib->ipif[id]) )
      break;

    if( mib->ipif[id].net_ip == ip ) {
      if( check(cp, &mib->ipif[id], data) ) {
        rc = 1;
        break;
      }
    }
  }
  CP_VERLOCK_STOP(version, mib)
  return rc;
}


typedef int/*bool*/ (*oo_cp_ipif_llap_check)(
        struct oo_cplane_handle* cp,
        cicp_llap_row_t* llap, cicp_ipif_row_t* ipif,
        void* data);
/* Same as oo_cp_find_ipif_by_ip(), but also looks up a llap row. */
static inline int
oo_cp_find_llap_by_ip(struct oo_cplane_handle* cp, ci_ip_addr_t ip,
                      oo_cp_ipif_llap_check check, void* data)
{
  struct cp_mibs* mib;
  cicp_rowid_t id;
  cp_version_t version;
  int rc = 0;

  CP_VERLOCK_START(version, mib, cp)

  for( id = 0; id < mib->dim->ipif_max; id++ ) {
    if( cicp_ipif_row_is_free(&mib->ipif[id]) )
      break;

    if( mib->ipif[id].net_ip == ip ) {
      cicp_rowid_t llap_id = cp_llap_find_row(mib, mib->ipif[id].ifindex);
      if( llap_id == CICP_ROWID_BAD )
        continue;
      if( check(cp, &mib->llap[llap_id], &mib->ipif[id], data) ) {
        rc = 1;
        break;
      }
    }
  }

  CP_VERLOCK_STOP(version, mib)
  return rc;
}

/* Keep this function inline to guarantee that it is properly optimized
 * when the most of the parameters are NULL. */
static inline int
oo_cp_find_llap(struct oo_cplane_handle* cp, ci_ifid_t ifindex,
                ci_mtu_t *out_mtu, cicp_hwport_mask_t *out_hwports,
                cicp_hwport_mask_t *out_rx_hwports,
                ci_mac_addr_t *out_mac,
                cicp_encap_t *out_encap)
{
  struct cp_mibs* mib;
  cicp_rowid_t id;
  cp_version_t version;
  int rc = 0;

  CP_VERLOCK_START(version, mib, cp)

  id = cp_llap_find_row(mib, ifindex);
  if( id == CICP_ROWID_BAD ) {
    rc = -ENOENT;
    goto out;
  }

  if( out_mtu != NULL )
    *out_mtu = mib->llap[id].mtu;
  if( out_hwports != NULL )
    *out_hwports = mib->llap[id].tx_hwports;
  if( out_rx_hwports != NULL )
    *out_rx_hwports = mib->llap[id].rx_hwports;
  if( out_mac != NULL )
    memcpy(out_mac, mib->llap[id].mac, sizeof(*out_mac));
  if( out_encap != NULL )
    *out_encap = mib->llap[id].encap;

 out:
  CP_VERLOCK_STOP(version, mib)
  return rc;
}

static inline ci_ip_addr_t
oo_cp_ifindex_to_ip(struct oo_cplane_handle* cp, ci_ifid_t ifindex)
{
  struct cp_mibs* mib;
  cicp_rowid_t id;
  cp_version_t version;
  ci_ip_addr_t ip = INADDR_ANY;

  CP_VERLOCK_START(version, mib, cp)

  for( id = 0; id < mib->dim->ipif_max; id++ ) {
    if( cicp_ipif_row_is_free(&mib->ipif[id]) )
      break;
    /* Fixme: check IFA_F_SECONDARY flag, get a primary address */
    if( mib->ipif[id].ifindex == ifindex ) {
      ip = mib->ipif[id].net_ip;
      break;
    }
  }

  CP_VERLOCK_STOP(version, mib)
  return ip;
}



/*
 * Bonding support
 */


#define CICP_HASH_STATE_FLAGS_IS_IP      0x1
#define CICP_HASH_STATE_FLAGS_IS_TCP_UDP 0x2
#define CICP_HASH_STATE_FLAGS_IS_FRAG    0x4

struct cicp_hash_state {
  int flags;
  ci_mac_addr_t src_mac;
  ci_mac_addr_t dst_mac;
  ci_ip_addr_t src_addr_be32;
  ci_ip_addr_t dst_addr_be32;
  ci_uint16 src_port_be16;
  ci_uint16 dst_port_be16;
};

ci_inline int cicp_layer2_hash(struct cicp_hash_state *hs, int num_slaves)
{
  return (hs->src_mac[5] ^ hs->dst_mac[5]) % num_slaves;
}

ci_inline int cicp_layer23_hash(struct cicp_hash_state *hs, int num_slaves)
{
  /* TODO do we ever call this with non-IP traffic */
  if( hs->flags & CICP_HASH_STATE_FLAGS_IS_IP ) {
    return
      ((CI_BSWAP_BE32(hs->src_addr_be32 ^ hs->dst_addr_be32) & 0xffff) ^ 
       (hs->src_mac[5] ^ hs->dst_mac[5])) % num_slaves;
  }
  else
    return cicp_layer2_hash(hs, num_slaves);
}

ci_inline int cicp_layer34_hash(struct cicp_hash_state *hs, int num_slaves)
{
  /* TODO do we ever call this with non-IP traffic */
  if( hs->flags & CICP_HASH_STATE_FLAGS_IS_IP ) {
    if( !(hs->flags & CICP_HASH_STATE_FLAGS_IS_FRAG) &&
        (hs->flags & CICP_HASH_STATE_FLAGS_IS_TCP_UDP) ) {
      return
        (CI_BSWAP_BE16(hs->src_port_be16 ^ hs->dst_port_be16) ^
         (CI_BSWAP_BE32(hs->src_addr_be32 ^ hs->dst_addr_be32) & 0xffff))
        % num_slaves;
    } else {
      return (CI_BSWAP_BE32(hs->src_addr_be32 ^ hs->dst_addr_be32) & 0xffff)
        % num_slaves;
    }
  }
  else {
    return cicp_layer2_hash(hs, num_slaves);
  }
}

static inline ci_hwport_id_t
oo_cp_hwport_bond_get(struct oo_cplane_handle* cp, ci_ifid_t ifindex, 
                      cicp_encap_t* encap, cicp_hwport_mask_t hwports,
                      struct cicp_hash_state* hs)
{
  ci_hwport_id_t hwport[sizeof(cicp_hwport_mask_t) * 8];
  int i;

  if( hwports == 0 )
    return CI_HWPORT_ID_BAD;

  hwport[0] = CI_HWPORT_ID_BAD; /* appease gcc */

  for( i = 0; hwports != 0 ; hwports &= (hwports-1), i++ )
    hwport[i] = cp_hwport_mask_first(hwports);

  if( encap->type & CICP_LLAP_TYPE_XMIT_HASH_LAYER34 )
    return hwport[cicp_layer34_hash(hs, i)];
  else if( encap->type & CICP_LLAP_TYPE_XMIT_HASH_LAYER23 )
    return hwport[cicp_layer23_hash(hs, i)];
  else {
    ci_assert_flags(encap->type, CICP_LLAP_TYPE_XMIT_HASH_LAYER2);
    return hwport[cicp_layer2_hash(hs, i)];
  }
}

#ifdef __cplusplus
}
#endif

#endif /* __TOOLS_CPLANE_ONLOAD_H__ */
