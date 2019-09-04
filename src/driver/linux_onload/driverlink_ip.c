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
*//*! \file driverlink_ip.c  Inter-driver communications for the IP driver
** <L5_PRIVATE L5_SOURCE>
** \author  gnb
**  \brief  Package - driver/efab	EtherFabric NIC driver
**   \date  2005/10/26
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*/

#include <linux/netfilter.h>
#include <linux/netfilter_arp.h>
#include <linux/netfilter_ipv4.h>
#include <linux/if_arp.h>

#include <ci/internal/ip.h>
#include <onload/driverlink_filter.h>
#include <ci/driver/driverlink_api.h>
#include <onload/linux_onload_internal.h>
#include <onload/tcp_helper_fns.h>
#include <onload/nic.h>
#include <onload/oof_interface.h>
#include <onload/oof_onload.h>
#include <ci/efrm/efrm_client.h>
#include "onload_internal.h"
#include "onload_kernel_compat.h"
#include <ci/driver/efab/hardware.h>


static int oo_use_vlans = 1;
module_param(oo_use_vlans, int, S_IRUGO);
MODULE_PARM_DESC(oo_use_vlans,
                 "Do use VLANs in Onload stack (on by default)");

static int oo_bond_poll_peak = (HZ/100);
module_param(oo_bond_poll_peak, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(oo_bond_poll_peak,
                 "Period (in jiffies) between peak-rate polls of /sys "
                 "for bonding state synchronisation");

static int oo_bond_peak_polls = 20;
module_param(oo_bond_peak_polls, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(oo_bond_peak_polls,
                 "Number of times to poll /sys at \"peak-rate\" before "
                 "reverting to base rate");


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
/* Define vlan_dev_real_dev() and vlan_dev_vlan_id()
 * if they are not defined. */
#ifndef VLAN_DEV_INFO
#define VLAN_DEV_INFO(netdev) vlan_dev_info(netdev)
#endif
#ifndef vlan_dev_real_dev
static inline struct net_device *
vlan_dev_real_dev(const struct net_device *dev)
{
	return VLAN_DEV_INFO(dev)->real_dev;
}
#endif
#ifndef vlan_dev_vlan_id
static inline u16 vlan_dev_vlan_id(const struct net_device *dev)
{
	return VLAN_DEV_INFO(dev)->vlan_id;
}
#endif
#endif


#if CI_CFG_TEAMING
# ifdef IFF_BONDING
#  define NETDEV_IS_BOND_MASTER(_dev)                                   \
  ((_dev->flags & (IFF_MASTER)) && (_dev->priv_flags & IFF_BONDING))
#  define NETDEV_IS_BOND(_dev)                                          \
  ((_dev->flags & (IFF_MASTER | IFF_SLAVE)) && (_dev->priv_flags & IFF_BONDING))
# else
#  define NETDEV_IS_BOND_MASTER(_dev) (_dev->flags & (IFF_MASTER))
#  define NETDEV_IS_BOND(_dev) (_dev->flags & (IFF_MASTER | IFF_SLAVE))
# endif
#else
# define NETDEV_IS_BOND_MASTER(_dev) 0
# define NETDEV_IS_BOND(_dev) 0
#endif


/* Check whether device may match software filters for Onload.
 *
 * In the ideal world, we'd like to have a fast check if the device is onloadable.
 * In the reality, there is no fast check for a teaming device, and teaming
 * device may be Onloadable.  So, we just check a device type.
 */
static inline int oo_nf_dev_match(const struct net_device *net_dev)
{
  return net_dev->type == ARPHRD_ETHER;
}

/* Find packet payload (whatever comes after the Ethernet header) */
static int oo_nf_skb_get_payload(struct sk_buff* skb, void** pdata, int* plen)
{
  if( skb_is_nonlinear(skb) ) {
    /* Look in the first page fragment */
    unsigned head_len = skb_headlen(skb);
    skb_frag_t* frag = &skb_shinfo(skb)->frags[0];

    if( skb_shinfo(skb)->frag_list || frag->page_offset < head_len )
      return 0;
    *pdata = skb_frag_address(frag) - head_len;
    *plen = frag->size + head_len;
    return 1;
  } else {
    *pdata = skb->data;
    *plen = skb->len;
    return 1;
  }
}

#if defined (RHEL_MAJOR) && defined (RHEL_MINOR)
#if RHEL_MAJOR == 7 && RHEL_MINOR >= 2
/* RHEL 7.2 kernel is crazy and can't be parsed by kernel_compat.sh correctly */
#define EFRM_HAVE_NETFILTER_INDEV_OUTDEV yes
#endif
#endif


#if defined(FUTURE_LINUX_RELEASE)
        /* put future variants here */
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
        /* Linux >= 4.4 : nf_hook_ops were replaced by void;
         * it is too hard to detect this from kernel_compat.sh */
# define NFHOOK_PARAMS \
    void *priv,         \
    struct sk_buff* skb,\
    const struct nf_hook_state *state
# define nfhook_skb skb
# define nfhook_indev state->in
#elif  defined(EFRM_HAVE_NETFILTER_HOOK_STATE) && \
      !defined(EFRM_HAVE_NETFILTER_INDEV_OUTDEV)
        /* linux < 4.4 */
# define NFHOOK_PARAMS \
    const struct nf_hook_ops* ops,  \
    struct sk_buff* skb,            \
    const struct nf_hook_state *state
# define nfhook_skb skb
# define nfhook_indev state->in
#elif defined(EFRM_HAVE_NETFILTER_HOOK_STATE) && \
      defined(EFRM_HAVE_NETFILTER_INDEV_OUTDEV)
        /* RHEL7 3.10 */
# define NFHOOK_PARAMS \
    const struct nf_hook_ops* ops,  \
    struct sk_buff* skb,            \
    const struct net_device* indev, \
    const struct net_device* outdev,\
    const struct nf_hook_state *state
# define nfhook_skb skb
# define nfhook_indev indev
#elif !defined(EFRM_HAVE_NETFILTER_HOOK_STATE) && \
       defined(EFRM_HAVE_NETFILTER_HOOK_OPS)
        /* linux < 4.1 */
# define NFHOOK_PARAMS \
    const struct nf_hook_ops* ops,  \
    struct sk_buff* skb,            \
    const struct net_device* indev, \
    const struct net_device* outdev,\
    int (*okfn)(struct sk_buff*)
# define nfhook_skb skb
# define nfhook_indev indev
#elif !defined(EFRM_HAVE_NETFILTER_HOOK_STATE) && \
      !defined(EFRM_HAVE_NETFILTER_HOOK_OPS) && \
      !defined(EFRM_HAVE_NETFILTER_INDIRECT_SKB)
        /* linux < 3.13 */
# define NFHOOK_PARAMS \
    unsigned int hooknum,           \
    struct sk_buff* skb,            \
    const struct net_device* indev, \
    const struct net_device* outdev,\
    int (*okfn)(struct sk_buff*)
# define nfhook_skb skb
# define nfhook_indev indev
#elif !defined(EFRM_HAVE_NETFILTER_HOOK_STATE) && \
      !defined(EFRM_HAVE_NETFILTER_HOOK_OPS) && \
       defined(EFRM_HAVE_NETFILTER_INDIRECT_SKB)
        /* linux < 2.6.24 */
# define NFHOOK_PARAMS \
    unsigned int hooknum,           \
    struct sk_buff** pskb,          \
    const struct net_device* indev, \
    const struct net_device* outdev,\
    int (*okfn)(struct sk_buff*)
# define nfhook_skb (*pskb)
# define nfhook_indev indev
#else
# error "Unsupported kernel version"
#endif



static unsigned int oo_netfilter_ip(NFHOOK_PARAMS)
{
  void* data;
  int len;

  if( oo_nf_dev_match(nfhook_indev) &&
      oo_nf_skb_get_payload(nfhook_skb, &data, &len) &&
      efx_dlfilter_handler(dev_net(nfhook_indev), nfhook_indev->ifindex,
                           efab_tcp_driver.dlfilter,
                           (const ci_ether_hdr*) skb_mac_header(nfhook_skb),
                           data, len) ) {
    kfree_skb(nfhook_skb);
    return NF_STOLEN;
  } else {
    return NF_ACCEPT;
  }
}

static struct nf_hook_ops oo_netfilter_ip_hook = {
  .hook = oo_netfilter_ip,
#ifdef EFRM_HAVE_NETFILTER_OPS_HAVE_OWNER
  .owner = THIS_MODULE,
#endif
#ifdef EFX_HAVE_NFPROTO_CONSTANTS
  .pf = NFPROTO_IPV4,
#else
  .pf = PF_INET,
#endif
#ifdef NF_IP_PRE_ROUTING
  .hooknum = NF_IP_PRE_ROUTING,
#else
  .hooknum = NF_INET_PRE_ROUTING,
#endif
  .priority = NF_IP_PRI_FIRST,
};


/* This function will create an oo_nic if one hasn't already been created.
 *
 * There are two code paths whereby this function can be called multiple
 * times for the same device:
 *
 * - If the interface was IFF_UP when this driver was loaded, then
 *   oo_netdev_event() will call oo_netdev_may_add() before dl_probe is run,
 *   which will call oo_netdev_may_add() itself.
 *
 * Once a device is noticed by onload, it should stay registered in cplane 
 * despite going up or being hotplugged.
 */
static struct oo_nic *oo_netdev_may_add(const struct net_device *net_dev)
{
  struct efhw_nic* efhw_nic;
  struct oo_nic* onic;

  onic = oo_nic_find_ifindex(net_dev->ifindex);
  if( onic == NULL )
    onic = oo_nic_add(net_dev->ifindex);

  if( onic != NULL ) {
    int up = net_dev->flags & IFF_UP;
    efhw_nic = efrm_client_get_nic(onic->efrm_client);
    oof_onload_hwport_up_down(&efab_tcp_driver, oo_nic_hwport(onic), up,
                              efhw_nic->devtype.arch == EFHW_ARCH_EF10 ? 1:0,
                              efhw_nic->flags & NIC_FLAG_VLAN_FILTERS ? 1:0, 0);
    if( up )
      onic->oo_nic_flags |= OO_NIC_UP;
    else
      onic->oo_nic_flags &= ~OO_NIC_UP;
    /* Remove OO_NIC_UNPLUGGED regardless of whether the interface is IFF_UP,
     * as we don't want to attempt to create ghost VIs now that the hardware is
     * back.
     */
    onic->oo_nic_flags &= ~OO_NIC_UNPLUGGED;
  }

  return onic;
}

static int oo_dl_probe(struct efx_dl_device* dl_dev,
                       const struct net_device* net_dev,
                       const struct efx_dl_device_info* dev_info,
                       const char* silicon_rev)
{
  struct oo_nic* onic = NULL;
  struct efx_dl_falcon_resources *res;

  efx_dl_for_each_device_info_matching(dev_info, EFX_DL_FALCON_RESOURCES,
                                       struct efx_dl_falcon_resources,
                                       hdr, res) {
    if( res->rx_usr_buf_size > FALCON_RX_USR_BUF_SIZE ) {
      ci_log("%s: ERROR: Net driver rx_usr_buf_size %u > %u", __func__,
             res->rx_usr_buf_size, FALCON_RX_USR_BUF_SIZE);
      return -1;
    }
  }

  if( ! netif_running(net_dev) ) {
    onic = oo_nic_find_ifindex(net_dev->ifindex);
    if( onic != NULL ) {
      /* We are dealing here with hotplug.  We block kernel traffic using drop
       * filters to prevent it hitting kernel and causing connection resets.
       * The filters will be redirected towards our RXQ when the interface
       * comes up.  The branch above that deals with the case where the device
       * is up will also insert appropriate filters, although this will be
       * deferred to the workqueue.  TODO: If we add support for hotplug on
       * generic Linux systems, we should also consider the case where [onic]
       * is NULL. */
      OO_DEBUG_VERB(ci_log("%s: Trigger drop filters on if %d", __func__,
                           net_dev->ifindex));

      /* Notify Onload that previous device on that hwport disappeared. */
      oof_onload_hwport_removed(&efab_tcp_driver, oo_nic_hwport(onic));
    }
  }

  if( onic == NULL ) {
    onic = oo_netdev_may_add(net_dev);
    if( onic == NULL )
      return -1;
  }

  dl_dev->priv = (void *)net_dev;
  return 0;
}


static void oo_dl_remove(struct efx_dl_device* dl_dev)
{
  /* We need to fini all of the hardware queues immediately. The net driver
   * will tidy up its own queues and *all* VIs, so if we don't free our own
   * queues they will be left dangling and will not be cleared even on an
   * entity reset.
   *   A note on locking: iterate_netifs_unlocked() will give us netif pointers
   * that are guaranteed to remain valid, but the state of the underlying
   * netifs may be unstable. However, we only touch immutable state. We can't
   * defer the work to the lock holders as we need to speak to the hardware
   * right now, before it goes away.
   */
  ci_netif* ni = NULL;
  struct net_device* netdev = dl_dev->priv;
  struct oo_nic* onic;
  if( (onic = oo_nic_find_ifindex(netdev->ifindex)) != NULL ) {
    /* Filter status need to be synced as after this function is finished
     * no further operations will be allowed.
     * Also note on polite hotplug oo_dl_remove() is called before
     * oo_netdev_going_down(), which will not have a chance to do its job
     * regarding filters.
     */
    oof_onload_hwport_up_down(&efab_tcp_driver,
                              oo_nic_hwport(onic), 0, 0, 0, 1);

    /* We need to prevent simultaneous resets so that the queues that are to be
     * shut down don't get brought back up again.  We do this by disabling any
     * further scheduling of resets, and then flushing any already scheduled on
     * each stack. */
    efrm_client_disable_post_reset(onic->efrm_client);

    onic->oo_nic_flags |= OO_NIC_UNPLUGGED;
    while( iterate_netifs_unlocked(&ni, 0, 0) == 0 )
      tcp_helper_flush_resets(ni);

    /* The actual business of flushing the queues will be handled by the
     * resource driver in its own driverlink removal hook in a moment. */
  }
}


static void oo_dl_reset_suspend(struct efx_dl_device* dl_dev)
{
  ci_log("%s:", __FUNCTION__);
}


static void oo_dl_reset_resume(struct efx_dl_device* dl_dev, int ok)
{
  ci_log("%s:", __FUNCTION__);
}


static void oo_fixup_wakeup_breakage(int ifindex)
{
  /* This is needed after a hardware interface is brought up, and after an
   * MTU change.  When a netdev goes down, or the MTU is changed, the net
   * driver event queues are destroyed and brought back.  This can cause
   * wakeup events to get lost.
   *
   * NB. This should cease to be necessary once the net driver is changed
   * to keep event queues up when the interface goes down.
   */
  struct oo_nic* onic;
  ci_netif* ni = NULL;
  int hwport, intf_i;
  if( (onic = oo_nic_find_ifindex(ifindex)) != NULL ) {
    hwport = onic - oo_nics;
    while( iterate_netifs_unlocked(&ni, 0, 0) == 0 )
      if( (intf_i = ni->hwport_to_intf_i[hwport]) >= 0 )
        ci_bit_clear(&ni->state->evq_primed, intf_i);
  }
}


static void oo_netdev_up(struct net_device* netdev)
{
  struct oo_nic *onic;
  struct efhw_nic* efhw_nic;
  /* Does efrm own this device? */
  if( efrm_nic_present(netdev->ifindex) ) {
    /* oo_netdev_may_add may trigger oof_hwport_up_down only
     * once on probe time */
    onic = oo_netdev_may_add(netdev);
    if( onic != NULL ) {
      oo_fixup_wakeup_breakage(netdev->ifindex);
      onic->oo_nic_flags |= OO_NIC_UP;
      efhw_nic = efrm_client_get_nic(onic->efrm_client);
      oof_onload_hwport_up_down(&efab_tcp_driver,oo_nic_hwport(onic), 1,
                               efhw_nic->devtype.arch == EFHW_ARCH_EF10 ? 1:0,
                               efhw_nic->flags & NIC_FLAG_VLAN_FILTERS ? 1:0, 0);
    }
  }
}


static void oo_netdev_going_down(struct net_device* netdev)
{
  struct oo_nic *onic;

  onic = oo_nic_find_ifindex(netdev->ifindex);
  if( onic != NULL ) {
      oof_onload_hwport_up_down(&efab_tcp_driver,
                                oo_nic_hwport(onic), 0, 0, 0, 0);
                                onic->oo_nic_flags &= ~OO_NIC_UP;
  }
}


/* Context: rtnl lock held */
static int oo_netdev_event(struct notifier_block *this,
                           unsigned long event, void *ptr)
{
  struct net_device *netdev = netdev_notifier_info_to_dev(ptr);

  switch( event ) {
  case NETDEV_UP:
    oo_netdev_up(netdev);
    break;

  case NETDEV_GOING_DOWN:
    oo_netdev_going_down(netdev);
    break;

  case NETDEV_CHANGEMTU:
    oo_fixup_wakeup_breakage(netdev->ifindex);

#ifdef EFRM_RTMSG_IFINFO_EXPORTED
    /* The control plane has to know about the new MTU value.
     * rtnetlink_event() converts most of NETDEV_* events into RTM_NEWLINK
     * messages, but it ignores NETDEV_CHANGEMTU.
     *
     * For older kernels rtmsg_ifinfo() is not available, so we rely on
     * periodic dump of the OS state in the onload_cp_server.  In many
     * cases (such as MTU change for an SFC NIC) the RTM_NEWLINK message
     * is delivered because of interface flags change; the only known issue
     * is with the bond interface.  See bug 74973 for details.
     */
    rtmsg_ifinfo(RTM_NEWLINK, netdev, 0
#ifdef EFRM_RTMSG_IFINFO_NEEDS_GFP_FLAGS
                 /* linux >= 3.13 require gfp_t argument */
                 , GFP_KERNEL
#endif
                 );
#else
    /* rtmsg_ifinfo() is exported in linux >= 3.9 and in the last
     * RHEL6 updates.  In 4.15 it is no longer exported, but
     * rtnetlink_event() doesn't ignore NETDEV_CHANGEMTU, so we don't
     * need to do anything.
     */
#endif
    break;

  default:
    break;
  }

  return NOTIFY_DONE;
}


static struct notifier_block oo_netdev_notifier = {
  .notifier_call = oo_netdev_event,
};


static struct efx_dl_driver oo_dl_driver = {
  .name = "onload",
  .flags = EFX_DL_DRIVER_CHECKS_FALCON_RX_USR_BUF_SIZE
#if EFX_DRIVERLINK_API_VERSION > 22 || (EFX_DRIVERLINK_API_VERSION == 22 && \
                                        EFX_DRIVERLINK_API_VERSION_MINOR > 4)
           | EFX_DL_DRIVER_CHECKS_MEDFORD2_VI_STRIDE
#endif
    ,
  .probe = oo_dl_probe,
  .remove = oo_dl_remove,
  .reset_suspend = oo_dl_reset_suspend,
  .reset_resume = oo_dl_reset_resume
};


int oo_driverlink_register(void)
{
  int rc;

  rc = register_netdevice_notifier(&oo_netdev_notifier);
  if (rc != 0)
    goto fail1;

  rc = efx_dl_register_driver(&oo_dl_driver);
  if (rc != 0)
    goto fail2;

#ifndef EFRM_HAVE_NF_NET_HOOK
  rc = nf_register_hook(&oo_netfilter_ip_hook);
  if( rc < 0 )
    goto fail4;
#endif

  return 0;

#ifndef EFRM_HAVE_NF_NET_HOOK
  fail4:
   efx_dl_unregister_driver(&oo_dl_driver);
#endif
 fail2:
  unregister_netdevice_notifier(&oo_netdev_notifier);
 fail1:
  ci_log("%s: efx_dl_register_driver failed (%d)", __FUNCTION__, rc);
  return rc;
}


void oo_driverlink_unregister(void)
{
#ifndef EFRM_HAVE_NF_NET_HOOK
  nf_unregister_hook(&oo_netfilter_ip_hook);
#endif
  unregister_netdevice_notifier(&oo_netdev_notifier);
  efx_dl_unregister_driver(&oo_dl_driver);
}

#ifdef EFRM_HAVE_NF_NET_HOOK
int oo_register_nfhook(struct net *net)
{
  return nf_register_net_hook(net, &oo_netfilter_ip_hook);
}
void oo_unregister_nfhook(struct net *net)
{
  nf_unregister_net_hook(net, &oo_netfilter_ip_hook);
}
#endif

/*! \cidoxg_end */
