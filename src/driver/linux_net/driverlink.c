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

/****************************************************************************
 * Driver for Solarflare network controllers and boards
 * Copyright 2005      Fen Systems Ltd.
 * Copyright 2005-2017 Solarflare Communications Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#include <linux/module.h>
#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
/* #include "driverlink_api.h" via net_driver.h */
#include "net_driver.h"
#include "efx.h"
#include "driverlink.h"
#include "filter.h"
#include "nic.h"
#include "workarounds.h"

/* Global lists are protected by rtnl_lock */

/* List of all registered drivers */
static LIST_HEAD(efx_driver_list);

/* List of all registered Efx ports. Protected by the rtnl_lock */
LIST_HEAD(efx_port_list);

/**
 * Driver link handle used internally to track devices
 * @efx_dev: driverlink device handle exported to consumers
 * @efx: efx_nic backing the driverlink device
 * @port_node: per-device list head
 * @driver_node: per-driver list head
 * @block_kernel_count: Number of times client has requested each kernel block,
 *     indexed by enum efx_dl_filter_block_kernel_type
 */
struct efx_dl_handle {
	struct efx_dl_device efx_dev;
	struct efx_nic *efx;
	struct list_head port_node;
	struct list_head driver_node;
	unsigned int block_kernel_count[EFX_DL_FILTER_BLOCK_KERNEL_MAX];
};

static struct efx_dl_handle *efx_dl_handle(struct efx_dl_device *efx_dev)
{
	return container_of(efx_dev, struct efx_dl_handle, efx_dev);
}

/* Warn if a driverlink call takes longer than 1 second */
#define EFX_DL_DURATION_WARN (1 * HZ)
/* Onload probe verifies a SW licenses which can take >1s. See SFC bug 62649 */
#define EFX_DL_DURATION_PROBE_WARN (3 * HZ)

#define _EFX_DL_CHECK_DURATION(duration, limit, label)			\
	do {                                                                   \
		if (duration > (limit) &&				\
		    !efx_nic_hw_unavailable(efx))                              \
			netif_warn(efx, drv, efx->net_dev,                     \
				   "%s: driverlink " label " took %ums\n",     \
				   efx_dev->driver->name,                      \
				   jiffies_to_msecs(duration));                \
	} while (0)
#define EFX_DL_CHECK_DURATION(duration, label) \
   _EFX_DL_CHECK_DURATION(duration, EFX_DL_DURATION_WARN, label)
#define EFX_DL_CHECK_DURATION_PROBE(duration, label) \
   _EFX_DL_CHECK_DURATION(duration, EFX_DL_DURATION_PROBE_WARN, label)

/* Remove an Efx device, and call the driver's remove() callback if
 * present. The caller must hold rtnl_lock. */
static void efx_dl_del_device(struct efx_dl_device *efx_dev)
{
	struct efx_dl_handle *efx_handle = efx_dl_handle(efx_dev);
	struct efx_nic *efx = efx_handle->efx;
	unsigned int type;
	u64 before, after, duration;

	netif_info(efx, drv, efx->net_dev,
		   "%s driverlink client unregistering\n",
		   efx_dev->driver->name);

	before = get_jiffies_64();

	if (efx_dev->driver->remove)
		efx_dev->driver->remove(efx_dev);

	after = get_jiffies_64();
	duration = after - before;
	EFX_DL_CHECK_DURATION(duration, "remove()");

	list_del(&efx_handle->driver_node);

	/* Disable and then re-enable NAPI when removing efx interface from list
	 * to prevent a race with read access from NAPI context; napi_disable()
	 * ensures that NAPI is no longer running when it returns.  Also
	 * internally lock NAPI while disabled to prevent busy-polling.
	 */
	efx_pause_napi(efx);
	list_del(&efx_handle->port_node);
	efx_resume_napi(efx);

	/* Remove this client's kernel blocks */
	mutex_lock(&efx->dl_block_kernel_mutex);
	for (type = 0; type < EFX_DL_FILTER_BLOCK_KERNEL_MAX; type++) {
		if (efx_handle->block_kernel_count[type]) {
			efx->dl_block_kernel_count[type] -=
				efx_handle->block_kernel_count[type];
			if (efx->dl_block_kernel_count[type] == 0)
				efx->type->filter_unblock_kernel(efx, type);
		}
	}
	mutex_unlock(&efx->dl_block_kernel_mutex);

	kfree(efx_handle);
}

/* Attempt to probe the given device with the driver, creating a
 * new &struct efx_dl_device. If the probe routine returns an error,
 * then the &struct efx_dl_device is destroyed */
static void efx_dl_try_add_device(struct efx_nic *efx,
				  struct efx_dl_driver *driver)
{
	struct efx_dl_handle *efx_handle;
	struct efx_dl_handle *ex_efx_handle;
	struct efx_dl_device *efx_dev;
	int rc;
	bool added = false;
	u64 before, after, duration;

	/* First check if device is supported by driver */
	if ((efx->pci_dev->device & 0x0f00) >= 0x0b00 &&
	    !(driver->flags & EFX_DL_DRIVER_CHECKS_MEDFORD2_VI_STRIDE)) {
		netif_info(efx, drv, efx->net_dev,
			   "%s driverlink client skipped: does not support X2 adapters\n",
			   driver->name);
		return;
	}

	efx_handle = kzalloc(sizeof(*efx_handle), GFP_KERNEL);
	if (!efx_handle)
		goto fail;
	efx_dev = &efx_handle->efx_dev;
	efx_handle->efx = efx;
	efx_dev->driver = driver;
	efx_dev->pci_dev = efx->pci_dev;
	INIT_LIST_HEAD(&efx_handle->port_node);
	INIT_LIST_HEAD(&efx_handle->driver_node);

	before = get_jiffies_64();

	rc = driver->probe(efx_dev, efx->net_dev, efx->dl_info, "");

	after = get_jiffies_64();
	duration = after - before;
#if defined(EFX_WORKAROUND_62649)
	EFX_DL_CHECK_DURATION_PROBE(duration, "probe()");
#else
	EFX_DL_CHECK_DURATION(duration, "probe()");
#endif

	if (rc)
		goto fail;

	/* Rather than just add to the end of the list,
	 * find the point that is at the end of the desired priority level
	 * and insert there. This will ensure that remove() callbacks are
	 * called in the reverse of the order of insertion.
	 */

	list_for_each_entry(ex_efx_handle, &efx->dl_device_list, port_node) {
		if (ex_efx_handle->efx_dev.driver->priority >
			driver->priority) {
			list_add_tail(&efx_handle->port_node, &ex_efx_handle->port_node);
			added = true;
			break;
		}
	}

	if (!added)
		list_add_tail(&efx_handle->port_node, &efx->dl_device_list);

	list_add_tail(&efx_handle->driver_node, &driver->device_list);

	netif_info(efx, drv, efx->net_dev,
		   "%s driverlink client registered\n", driver->name);
	return;

fail:
	netif_info(efx, drv, efx->net_dev,
		   "%s driverlink client skipped\n", driver->name);

	kfree(efx_handle);
}

/* Unregister a driver from the driverlink layer, calling the
 * driver's remove() callback for every attached device */
void efx_dl_unregister_driver(struct efx_dl_driver *driver)
{
	struct efx_dl_handle *efx_handle, *efx_handle_n;

	printk(KERN_INFO "Efx driverlink unregistering %s driver\n",
		 driver->name);

	rtnl_lock();

	list_for_each_entry_safe(efx_handle, efx_handle_n,
				 &driver->device_list, driver_node)
		efx_dl_del_device(&efx_handle->efx_dev);

	list_del(&driver->node);

	rtnl_unlock();
}
EXPORT_SYMBOL(efx_dl_unregister_driver);

/* Register a new driver with the driverlink layer. The driver's
 * probe routine will be called for every attached nic. */
int __efx_dl_register_driver(struct efx_dl_driver *driver)
{
	struct efx_nic *efx;

	if (!(driver->flags & EFX_DL_DRIVER_CHECKS_FALCON_RX_USR_BUF_SIZE)) {
		pr_err("Efx driverlink: %s did not promise to check rx_usr_buf_size\n",
		       driver->name);
		return -EPERM;
	}

	if (driver->flags & EFX_DL_DRIVER_REQUIRES_MINOR_VER &&
	    driver->minor_ver > EFX_DRIVERLINK_API_VERSION_MINOR) {
		pr_err("Efx driverlink: %s requires API %d.%d, sfc has %d.%d\n",
		       driver->name, EFX_DRIVERLINK_API_VERSION,
		       driver->minor_ver, EFX_DRIVERLINK_API_VERSION,
		       EFX_DRIVERLINK_API_VERSION_MINOR);
		return -EPERM;
	}
	driver->flags |= EFX_DL_DRIVER_SUPPORTS_MINOR_VER;

	if (driver->rx_packet)
		pr_warn("Efx driverlink: %s includes rx_packet handler, but this feature is deprecated and will be removed in a future release of this driver.\n",
			driver->name);

	printk(KERN_INFO "Efx driverlink registering %s driver\n",
		 driver->name);

	INIT_LIST_HEAD(&driver->node);
	INIT_LIST_HEAD(&driver->device_list);

	rtnl_lock();

	list_add_tail(&driver->node, &efx_driver_list);
	list_for_each_entry(efx, &efx_port_list, dl_node)
		efx_dl_try_add_device(efx, driver);

	rtnl_unlock();
	return 0;
}
EXPORT_SYMBOL(__efx_dl_register_driver);

void efx_dl_unregister_nic(struct efx_nic *efx)
{
	struct efx_dl_handle *efx_handle, *efx_handle_n;

	ASSERT_RTNL();

	list_for_each_entry_safe_reverse(efx_handle, efx_handle_n,
					 &efx->dl_device_list,
					 port_node)
		efx_dl_del_device(&efx_handle->efx_dev);

	list_del(&efx->dl_node);
}

void efx_dl_register_nic(struct efx_nic *efx)
{
	struct efx_dl_driver *driver;

	ASSERT_RTNL();

	list_add_tail(&efx->dl_node, &efx_port_list);
	list_for_each_entry(driver, &efx_driver_list, node)
		efx_dl_try_add_device(efx, driver);
}

struct efx_dl_device *efx_dl_dev_from_netdev(const struct net_device *net_dev,
					     struct efx_dl_driver *driver)
{
	struct efx_dl_handle *efx_handle;
	struct efx_nic *efx;

	ASSERT_RTNL();

	if (!efx_dl_netdev_is_ours(net_dev))
		return NULL;

	efx = netdev_priv((struct net_device *)net_dev);
	list_for_each_entry(efx_handle, &efx->dl_device_list, port_node) {
		if (efx_handle->efx_dev.driver == driver)
			return &efx_handle->efx_dev;
	}

	return NULL;
}
EXPORT_SYMBOL(efx_dl_dev_from_netdev);

void efx_dl_schedule_reset(struct efx_dl_device *efx_dev)
{
	struct efx_dl_handle *efx_handle = efx_dl_handle(efx_dev);
	struct efx_nic *efx = efx_handle->efx;

	efx_schedule_reset(efx, RESET_TYPE_ALL);
}
EXPORT_SYMBOL(efx_dl_schedule_reset);

/* Suspend ready for reset, calling the reset_suspend() callback of every
 * registered driver */
void efx_dl_reset_suspend(struct efx_nic *efx)
{
	struct efx_dl_handle *efx_handle;
	struct efx_dl_device *efx_dev;

	ASSERT_RTNL();

	list_for_each_entry_reverse(efx_handle,
				    &efx->dl_device_list,
				    port_node) {
		efx_dev = &efx_handle->efx_dev;
		if (efx_dev->driver->reset_suspend) {
			u64 before, after, duration;

			before = get_jiffies_64();

			efx_dev->driver->reset_suspend(efx_dev);

			after = get_jiffies_64();
			duration = after - before;
			EFX_DL_CHECK_DURATION(duration, "reset_suspend()");
		}
	}
}

/* Resume after a reset, calling the resume() callback of every registered
 * driver */
void efx_dl_reset_resume(struct efx_nic *efx, int ok)
{
	struct efx_dl_handle *efx_handle;
	struct efx_dl_device *efx_dev;

	ASSERT_RTNL();

	list_for_each_entry(efx_handle, &efx->dl_device_list,
			    port_node) {
		efx_dev = &efx_handle->efx_dev;
		if (efx_dev->driver->reset_resume) {
			u64 before, after, duration;

			before = get_jiffies_64();

			efx_dev->driver->reset_resume(efx_dev, ok);

			after = get_jiffies_64();
			duration = after - before;
			EFX_DL_CHECK_DURATION(duration, "reset_resume()");
		}
	}
}

int efx_dl_handle_event(struct efx_nic *efx, void *event, int budget)
{
	struct efx_dl_handle *efx_handle;
	struct efx_dl_device *efx_dev;

	list_for_each_entry(efx_handle, &efx->dl_device_list, port_node) {
		efx_dev = &efx_handle->efx_dev;
		if (efx_dev->driver->handle_event ) {
			u64 before, after, duration;
			int rc;

			before = get_jiffies_64();

			rc = efx_dev->driver->handle_event(efx_dev,
							   event, budget);

			after = get_jiffies_64();
			duration = after - before;
			EFX_DL_CHECK_DURATION(duration, "handle_event()");

			if (rc >= 0 )
				return rc > budget ? budget : rc;
		}
	}

	return -EINVAL;
}

bool efx_dl_rx_packet(struct efx_nic *efx, int channel, u8 *pkt_hdr, int len)
{
	struct efx_dl_handle *efx_handle;
	struct efx_dl_device *efx_dev;
	bool discard = false;

	list_for_each_entry(efx_handle, &efx->dl_device_list, port_node) {
		efx_dev = &efx_handle->efx_dev;
		if (efx_dev->driver->rx_packet) {
			int rc;
#ifdef DEBUG
			u64 before, after, duration;

			before = get_jiffies_64();
#endif
			rc = efx_dev->driver->rx_packet(efx_dev, channel,
							pkt_hdr, len);
#ifdef DEBUG
			after = get_jiffies_64();
			duration = after - before;
			EFX_DL_CHECK_DURATION(duration, "rx_packet()");
#endif
			if (rc)
				discard = true;
		}
	}

	return discard;
}

u32 efx_dl_rss_flags_default(struct efx_dl_device *efx_dev)
{
	struct efx_nic *efx = efx_dl_handle(efx_dev)->efx;

	if (efx->type->rx_get_default_rss_flags)
		return efx->type->rx_get_default_rss_flags(efx);
	/* NIC does not support RSS flags, so any value will do */
	return 0;
}
EXPORT_SYMBOL(efx_dl_rss_flags_default);

int efx_dl_rss_context_new(struct efx_dl_device *efx_dev, const u32 *indir,
			   const u8 *key, u32 flags, u8 num_queues,
			   u32 *rss_context)
{
	struct efx_nic *efx = efx_dl_handle(efx_dev)->efx;
	struct efx_rss_context *ctx;
	int rc;

	/* num_queues=0 is used internally by the driver to represent
	 * efx->rss_spread, and is not appropriate for driverlink clients
	 */
	if (!num_queues)
		return -EINVAL;

	if (!efx->type->rx_push_rss_context_config)
		return -EOPNOTSUPP;

	mutex_lock(&efx->rss_lock);
	ctx = efx_alloc_rss_context_entry(efx);
	if (!ctx) {
		rc = -ENOMEM;
		goto out_unlock;
	}
	if (!indir) {
		efx_set_default_rx_indir_table(efx, ctx);
		indir = ctx->rx_indir_table;
	}
	if (!key) {
		netdev_rss_key_fill(ctx->rx_hash_key, sizeof(ctx->rx_hash_key));
		key = ctx->rx_hash_key;
	}
	ctx->flags = flags;
	ctx->num_queues = num_queues;
	rc = efx->type->rx_push_rss_context_config(efx, ctx, indir, key);
	if (rc)
		efx_free_rss_context_entry(ctx);
	else
		*rss_context = ctx->user_id;
out_unlock:
	mutex_unlock(&efx->rss_lock);
	return rc;
}
EXPORT_SYMBOL(efx_dl_rss_context_new);

int efx_dl_rss_context_set(struct efx_dl_device *efx_dev, const u32 *indir,
			   const u8 *key, u32 flags, u32 rss_context)
{
	struct efx_nic *efx = efx_dl_handle(efx_dev)->efx;
	struct efx_rss_context *ctx;
	u32 old_flags;
	int rc;

	if (!efx->type->rx_push_rss_context_config)
		return -EOPNOTSUPP;

	mutex_lock(&efx->rss_lock);
	ctx = efx_find_rss_context_entry(efx, rss_context);
	if (!ctx) {
		rc = -ENOENT;
		goto out_unlock;
	}

	if (!indir) /* no change */
		indir = ctx->rx_indir_table;
	if (!key) /* no change */
		key = ctx->rx_hash_key;
	old_flags = ctx->flags;
	ctx->flags = flags;
	rc = efx->type->rx_push_rss_context_config(efx, ctx, indir, key);
	if (rc) /* restore old RSS flags on failure */
		ctx->flags = old_flags;
out_unlock:
	mutex_unlock(&efx->rss_lock);
	return rc;
}
EXPORT_SYMBOL(efx_dl_rss_context_set);

int efx_dl_rss_context_free(struct efx_dl_device *efx_dev, u32 rss_context)
{
	struct efx_nic *efx = efx_dl_handle(efx_dev)->efx;
	struct efx_rss_context *ctx;
	int rc;

	if (!efx->type->rx_push_rss_context_config)
		return -EOPNOTSUPP;

	mutex_lock(&efx->rss_lock);
	ctx = efx_find_rss_context_entry(efx, rss_context);
	if (!ctx) {
		rc = -ENOENT;
		goto out_unlock;
	}

	rc = efx->type->rx_push_rss_context_config(efx, ctx, NULL, NULL);
	if (!rc)
		efx_free_rss_context_entry(ctx);
out_unlock:
	mutex_unlock(&efx->rss_lock);
	return rc;
}
EXPORT_SYMBOL(efx_dl_rss_context_free);

/* We additionally include priority in the filter ID so that we
 * can pass it back into efx_filter_remove_id_safe().
 */
#define EFX_FILTER_PRI_SHIFT	28
#define EFX_FILTER_ID_MASK	((1 << EFX_FILTER_PRI_SHIFT) - 1)

int efx_dl_filter_insert(struct efx_dl_device *efx_dev,
			 const struct efx_filter_spec *spec,
			 bool replace_equal)
{
	s32 filter_id = efx_filter_insert_filter(efx_dl_handle(efx_dev)->efx,
						 spec, replace_equal);
	if (filter_id >= 0) {
		EFX_WARN_ON_PARANOID(filter_id & ~EFX_FILTER_ID_MASK);
		filter_id |= spec->priority << EFX_FILTER_PRI_SHIFT;
	}
	return filter_id;
}
EXPORT_SYMBOL(efx_dl_filter_insert);

int efx_dl_filter_remove(struct efx_dl_device *efx_dev, int filter_id)
{
	if (filter_id < 0)
		return -EINVAL;
	return efx_filter_remove_id_safe(efx_dl_handle(efx_dev)->efx,
					 filter_id >> EFX_FILTER_PRI_SHIFT,
					 filter_id & EFX_FILTER_ID_MASK);
}
EXPORT_SYMBOL(efx_dl_filter_remove);

int efx_dl_filter_redirect(struct efx_dl_device *efx_dev,
			   int filter_id, int rxq_i, int stack_id)
{
	struct efx_nic *efx = efx_dl_handle(efx_dev)->efx;

	if (WARN_ON(filter_id < 0))
		return -EINVAL;
	return efx->type->filter_redirect(efx, filter_id & EFX_FILTER_ID_MASK,
					  NULL, rxq_i, stack_id);
}
EXPORT_SYMBOL(efx_dl_filter_redirect);

int efx_dl_filter_redirect_rss(struct efx_dl_device *efx_dev,
			       int filter_id, int rxq_i, u32 rss_context,
			       int stack_id)
{
	struct efx_nic *efx = efx_dl_handle(efx_dev)->efx;

	if (WARN_ON(filter_id < 0))
		return -EINVAL;
	return efx->type->filter_redirect(efx, filter_id & EFX_FILTER_ID_MASK,
					  &rss_context, rxq_i, stack_id);
}
EXPORT_SYMBOL(efx_dl_filter_redirect_rss);

int efx_dl_vport_filter_insert(struct efx_dl_device *efx_dev,
			       unsigned int vport_id,
			       const struct efx_filter_spec *spec,
			       u64 *filter_id_out, bool *is_exclusive_out)
{
	struct efx_nic *efx = efx_dl_handle(efx_dev)->efx;
	return efx->type->vport_filter_insert(efx, vport_id, spec,
					      filter_id_out, is_exclusive_out);
}
EXPORT_SYMBOL(efx_dl_vport_filter_insert);

int efx_dl_vport_filter_remove(struct efx_dl_device *efx_dev,
			       unsigned int vport_id,
			       u64 filter_id, bool is_exclusive)
{
	struct efx_nic *efx = efx_dl_handle(efx_dev)->efx;
	return efx->type->vport_filter_remove(efx, vport_id, filter_id,
					      is_exclusive);
}
EXPORT_SYMBOL(efx_dl_vport_filter_remove);

int efx_dl_mcdi_rpc(struct efx_dl_device *efx_dev, unsigned int cmd,
		    size_t inlen, size_t outlen, size_t *outlen_actual,
		    const u8 *inbuf, u8 *outbuf)
{
	/* FIXME: Buffer parameter types should be changed to __le32 *
	 * so we can reasonably assume they are properly padded even
	 * if the lengths are not multiples of 4.
	 */
	if (WARN_ON(inlen & 3 || outlen & 3))
		return -EINVAL;

	return efx_mcdi_rpc_quiet(efx_dl_handle(efx_dev)->efx, cmd,
				  (const efx_dword_t *)inbuf, inlen,
				  (efx_dword_t *)outbuf, outlen, outlen_actual);
}
EXPORT_SYMBOL(efx_dl_mcdi_rpc);

int efx_dl_filter_block_kernel(struct efx_dl_device *efx_dev,
			       enum efx_dl_filter_block_kernel_type type)
{
	struct efx_dl_handle *handle = efx_dl_handle(efx_dev);
	struct efx_nic *efx = handle->efx;
	int rc;

	mutex_lock(&efx->dl_block_kernel_mutex);

	if (efx->dl_block_kernel_count[type] == 0) {
		rc = efx->type->filter_block_kernel(efx, type);
		if (rc)
			goto unlock;
	}
	++handle->block_kernel_count[type];
	++efx->dl_block_kernel_count[type];

unlock:
	mutex_unlock(&efx->dl_block_kernel_mutex);

	return rc;
}
EXPORT_SYMBOL(efx_dl_filter_block_kernel);

void efx_dl_filter_unblock_kernel(struct efx_dl_device *efx_dev,
				  enum efx_dl_filter_block_kernel_type type)
{
	struct efx_dl_handle *handle = efx_dl_handle(efx_dev);
	struct efx_nic *efx = handle->efx;

	mutex_lock(&efx->dl_block_kernel_mutex);

	if (WARN_ON(handle->block_kernel_count[type] == 0))
		goto unlock;

	--handle->block_kernel_count[type];
	--efx->dl_block_kernel_count[type];

	if (efx->dl_block_kernel_count[type] == 0)
		efx->type->filter_unblock_kernel(efx, type);
unlock:
	mutex_unlock(&efx->dl_block_kernel_mutex);
}
EXPORT_SYMBOL(efx_dl_filter_unblock_kernel);
