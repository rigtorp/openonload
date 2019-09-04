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

/***************************************************************************
 * Driver for Solarflare network controllers and boards
 * Copyright 2012-2015 Solarflare Communications Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#include "net_driver.h"
#include "ef10_regs.h"
#include "io.h"
#include "mcdi.h"
#include "mcdi_pcol.h"
#include "nic.h"
#include "workarounds.h"
#include "selftest.h"
#include "sriov.h"
#include "ef10_sriov.h"
#include <linux/in.h>
#include <linux/jhash.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#ifdef EFX_NOT_UPSTREAM
#include "efx_ioctl.h"
#endif
#include <linux/module.h>
#include "debugfs.h"

#if defined(EFX_USE_KCOMPAT) && !defined(EFX_HAVE_HW_ENC_FEATURES) && !defined(__VMKLNX__)
#undef EFX_USE_OVERLAY_TX_CSUM
#endif

static unsigned int tx_push_max_fill = 0xffffffff;
module_param(tx_push_max_fill, uint, 0444);
MODULE_PARM_DESC(tx_push_max_fill,
		 "[SFC9100-family] Only use Tx push when the queue is below "
		 "this fill level; 0=>never push 1=>push when empty; "
		 "default always to push");

static bool tx_coalesce_doorbell = false;
module_param(tx_coalesce_doorbell, bool, 0444);
MODULE_PARM_DESC(tx_coalesce_doorbell,
		 "[SFC9100-family] Coalesce notification to NIC of pending TX"
		 "data when set this option sets tx_push_max_fill=0:"
		 ":default=N");

static bool multicast_chaining = true;
module_param(multicast_chaining, bool, 0444);
MODULE_PARM_DESC(multicast_chaining,
		 "[SFC9100-family] Enabled multicast filter chaining in "
		 "firmware; default=Y");

#ifdef EFX_NOT_UPSTREAM
static bool monitor_hw_available = false;
module_param(monitor_hw_available, bool, 0644);
MODULE_PARM_DESC(monitor_hw_available,
		 "[SFC9100-family] Check hardware availability during periodic "
		 "monitor; default=N");
#endif

#ifdef EFX_NOT_UPSTREAM
static bool tx_non_csum_queue = false;
module_param(tx_non_csum_queue, bool, 0644);
MODULE_PARM_DESC(tx_non_csum_queue,
		 "[SFC9100-family] Allocate dedicated TX queues for traffic "
		 "not requiring checksum offload; default=N");
#endif

/* Hardware control for EF10 architecture including 'Huntington'. */

#define EFX_EF10_DRVGEN_EV		7
enum {
	EFX_EF10_TEST = 1,
	EFX_EF10_REFILL,
#ifdef EFX_NOT_UPSTREAM
	EFX_EF10_RERING_RX_DOORBELL,
#endif
};

#ifdef EFX_NOT_UPSTREAM
#define EF10_ONLOAD_PF_VIS 240
#define EF10_ONLOAD_VF_VIS 0
#endif
/* The reserved RSS context value */
#define EFX_EF10_RSS_CONTEXT_INVALID	0xffffffff
/* The maximum size of a shared RSS context */
/* TODO: this should really be from the mcdi protocol export */
#define EFX_EF10_MAX_SHARED_RSS_CONTEXT_SIZE 64UL

/* The filter table(s) are managed by firmware and we have write-only
 * access.  When removing filters we must identify them to the
 * firmware by a 64-bit handle, but this is too wide for Linux kernel
 * interfaces (32-bit for RX NFC, 16-bit for RFS).  Also, we need to
 * be able to tell in advance whether a requested insertion will
 * replace an existing filter.  Therefore we maintain a software hash
 * table, which should be at least as large as the hardware hash
 * table.
 *
 * Huntington has a single 8K filter table shared between all filter
 * types and both ports.
 */
#define HUNT_FILTER_TBL_ROWS 8192

#define EFX_EF10_FILTER_ID_INVALID 0xffff

#define EFX_EF10_FILTER_DEV_UC_MAX	32
#define EFX_EF10_FILTER_DEV_MC_MAX	256

/* VLAN list entry */
struct efx_ef10_vlan {
	struct list_head list;
	u16 vid;
};

enum efx_ef10_default_filters {
	EFX_EF10_BCAST,
	EFX_EF10_UCDEF,
	EFX_EF10_MCDEF,
	EFX_EF10_VXLAN4_UCDEF,
	EFX_EF10_VXLAN4_MCDEF,
	EFX_EF10_VXLAN6_UCDEF,
	EFX_EF10_VXLAN6_MCDEF,
	EFX_EF10_NVGRE4_UCDEF,
	EFX_EF10_NVGRE4_MCDEF,
	EFX_EF10_NVGRE6_UCDEF,
	EFX_EF10_NVGRE6_MCDEF,
	EFX_EF10_GENEVE4_UCDEF,
	EFX_EF10_GENEVE4_MCDEF,
	EFX_EF10_GENEVE6_UCDEF,
	EFX_EF10_GENEVE6_MCDEF,

	EFX_EF10_NUM_DEFAULT_FILTERS
};

/* Per-VLAN filters information */
struct efx_ef10_filter_vlan {
	struct list_head list;
	u16 vid;
	u16 uc[EFX_EF10_FILTER_DEV_UC_MAX];
	u16 mc[EFX_EF10_FILTER_DEV_MC_MAX];
	u16 default_filters[EFX_EF10_NUM_DEFAULT_FILTERS];
	bool warn_on_zero_filters;
};

struct efx_ef10_dev_addr {
	u8 addr[ETH_ALEN];
};

struct efx_ef10_filter_table {
/* The MCDI match masks supported by this fw & hw, in order of priority */
	u32 rx_match_mcdi_flags[
		MC_CMD_GET_PARSER_DISP_INFO_OUT_SUPPORTED_MATCHES_MAXNUM * 2];
	unsigned int rx_match_count;

	struct {
		unsigned long spec;	/* pointer to spec plus flag bits */
/* BUSY flag indicates that an update is in progress.  AUTO_OLD is
 * used to mark and sweep MAC filters for the device address lists.
 */
#define EFX_EF10_FILTER_FLAG_BUSY	1UL
#define EFX_EF10_FILTER_FLAG_AUTO_OLD	2UL
#define EFX_EF10_FILTER_FLAGS		3UL
		u64 handle;		/* firmware handle */
	} *entry;
	wait_queue_head_t waitq;
/* Shadow of net_device address lists, guarded by mac_lock */
	struct efx_ef10_dev_addr dev_uc_list[EFX_EF10_FILTER_DEV_UC_MAX];
	struct efx_ef10_dev_addr dev_mc_list[EFX_EF10_FILTER_DEV_MC_MAX];
	int dev_uc_count;
	int dev_mc_count;
	bool uc_promisc;
	bool mc_promisc;
	bool mc_promisc_last;
	bool mc_overflow; /* Too many MC addrs; should always imply mc_promisc */
	bool vlan_filter;
	struct list_head vlan_list;
#ifdef EFX_NOT_UPSTREAM
	bool kernel_blocked[EFX_DL_FILTER_BLOCK_KERNEL_MAX];
#endif
};

/* An arbitrary search limit for the software hash table */
#define EFX_EF10_FILTER_SEARCH_LIMIT 200

static void efx_ef10_rx_free_indir_table(struct efx_nic *efx);
static int efx_ef10_filter_table_probe_supported_filters(struct efx_nic *efx,
                                        struct efx_ef10_filter_table *table);
static void efx_ef10_filter_table_remove(struct efx_nic *efx);
static bool efx_ef10_hw_unavailable(struct efx_nic *efx);
static void _efx_ef10_rx_write(struct efx_rx_queue *rx_queue);
static int efx_ef10_filter_add_vlan(struct efx_nic *efx, u16 vid);
static void efx_ef10_filter_del_vlan_internal(struct efx_nic *efx,
					      struct efx_ef10_filter_vlan *vlan);
static void efx_ef10_filter_del_vlan(struct efx_nic *efx, u16 vid);
static int efx_ef10_set_udp_tnl_ports(struct efx_nic *efx, bool unloading);
#ifdef CONFIG_SFC_SRIOV
static void efx_ef10_vf_update_stats_work(struct work_struct *data);
#endif
#ifdef EFX_NOT_UPSTREAM
static struct efx_tx_queue *
efx_ef10_select_tx_queue_non_csum(struct efx_channel *channel,
				  struct sk_buff *skb);
#endif
static struct efx_tx_queue *
efx_ef10_select_tx_queue(struct efx_channel *channel, struct sk_buff *skb);
#ifdef EFX_USE_OVERLAY_TX_CSUM
#ifdef EFX_NOT_UPSTREAM
static struct efx_tx_queue *
efx_ef10_select_tx_queue_non_csum_overlay(struct efx_channel *channel,
					  struct sk_buff *skb);
#endif
static struct efx_tx_queue *
efx_ef10_select_tx_queue_overlay(struct efx_channel *channel,
				 struct sk_buff *skb);
#endif /* EFX_USE_OVERLAY_TX_CSUM */

static struct efx_filter_spec *
efx_ef10_filter_entry_spec(const struct efx_ef10_filter_table *table,
			   unsigned int filter_idx)
{
	return (struct efx_filter_spec *)(table->entry[filter_idx].spec &
					  ~EFX_EF10_FILTER_FLAGS);
}

static unsigned int
efx_ef10_filter_entry_flags(const struct efx_ef10_filter_table *table,
			    unsigned int filter_idx)
{
	return table->entry[filter_idx].spec & EFX_EF10_FILTER_FLAGS;
}

static u32 efx_ef10_filter_get_unsafe_id(struct efx_nic *efx, u32 filter_id)
{
	(void) efx;
	WARN_ON_ONCE(filter_id == EFX_EF10_FILTER_ID_INVALID);
	return filter_id & (HUNT_FILTER_TBL_ROWS - 1);
}

static unsigned int efx_ef10_filter_get_unsafe_pri(u32 filter_id)
{
	return filter_id / (HUNT_FILTER_TBL_ROWS * 2);
}

static u32 efx_ef10_make_filter_id(unsigned int pri, u16 idx)
{
	return pri * HUNT_FILTER_TBL_ROWS * 2 + idx;
}

static int efx_ef10_get_warm_boot_count(struct efx_nic *efx)
{
	efx_dword_t reg;

	efx_readd(efx, &reg, ER_DZ_BIU_MC_SFT_STATUS);

	if (EFX_DWORD_FIELD(reg, EFX_DWORD_0) == 0xffffffff) {
		netif_err(efx, hw, efx->net_dev, "Hardware unavailable\n");
		efx->state = STATE_DISABLED;
		return -ENETDOWN;
	} else {
		return EFX_DWORD_FIELD(reg, EFX_WORD_1) == 0xb007 ?
			EFX_DWORD_FIELD(reg, EFX_WORD_0) : -EIO;
	}
}

static unsigned int efx_ef10_initial_mem_map_size(struct efx_nic *efx)
{
	/* For the initial memory mapping, map only one VI's-worth. This is
	 * enough to get the VI-independent registers.
	 */
#ifdef EFX_NOT_UPSTREAM
	/* ...but is not so much as to interfere with pre-existing mappings of
	 * portions of the BAR held by Onload.
	 */
#endif
	return EFX_VI_PAGE_SIZE;
}

#if defined(EFX_NOT_UPSTREAM) && defined(CONFIG_SFC_AOE)
static inline bool aoe_enabled(struct efx_ef10_nic_data *nic)
{
	return (nic->caps & (1 << MC_CMD_CAPABILITIES_AOE_LBN));
}
#endif

static unsigned int efx_ef10_bar_size(struct efx_nic *efx)
{
	int bar;

	bar = efx->type->mem_bar;
	return resource_size(&efx->pci_dev->resource[bar]);
}

static bool efx_ef10_is_vf(struct efx_nic *efx)
{
	return efx->type->is_vf;
}

static int efx_ef10_get_pf_index(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF(outbuf, MC_CMD_GET_FUNCTION_INFO_OUT_LEN);
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	size_t outlen;
	int rc;

	rc = efx_mcdi_rpc(efx, MC_CMD_GET_FUNCTION_INFO, NULL, 0, outbuf,
			  sizeof(outbuf), &outlen);
	if (rc)
		return rc;
	if (outlen < sizeof(outbuf))
		return -EIO;

	nic_data->pf_index = MCDI_DWORD(outbuf, GET_FUNCTION_INFO_OUT_PF);
	return 0;
}

#ifdef CONFIG_SFC_SRIOV
static int efx_ef10_get_vf_index(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF(outbuf, MC_CMD_GET_FUNCTION_INFO_OUT_LEN);
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	size_t outlen;
	int rc;

	rc = efx_mcdi_rpc(efx, MC_CMD_GET_FUNCTION_INFO, NULL, 0, outbuf,
			  sizeof(outbuf), &outlen);
	if (rc)
		return rc;
	if (outlen < sizeof(outbuf))
		return -EIO;

	nic_data->vf_index = MCDI_DWORD(outbuf, GET_FUNCTION_INFO_OUT_VF);
	return 0;
}
#endif

static int efx_ef10_init_datapath_caps(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF(outbuf, MC_CMD_GET_CAPABILITIES_V2_OUT_LEN);
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	size_t outlen;
	int rc;

	BUILD_BUG_ON(MC_CMD_GET_CAPABILITIES_IN_LEN != 0);

	rc = efx_mcdi_rpc(efx, MC_CMD_GET_CAPABILITIES, NULL, 0,
			  outbuf, sizeof(outbuf), &outlen);
	if (rc)
		return rc;
	if (outlen < MC_CMD_GET_CAPABILITIES_OUT_LEN) {
		netif_err(efx, drv, efx->net_dev,
			  "unable to read datapath firmware capabilities\n");
		return -EIO;
	}

	nic_data->datapath_caps =
		MCDI_DWORD(outbuf, GET_CAPABILITIES_OUT_FLAGS1);

	if (outlen >= MC_CMD_GET_CAPABILITIES_V2_OUT_LEN) {
		nic_data->datapath_caps2 = MCDI_DWORD(outbuf,
				GET_CAPABILITIES_V2_OUT_FLAGS2);
		nic_data->piobuf_size = MCDI_WORD(outbuf,
				GET_CAPABILITIES_V2_OUT_SIZE_PIO_BUFF);
#ifdef EFX_NOT_UPSTREAM
		/* Does the largest sw-possible PIO packet fit inside a hw
		 * PIO buffer?
		 */
		EFX_WARN_ON_PARANOID(nic_data->piobuf_size < ER_DZ_TX_PIOBUF_SIZE);
#endif
	} else {
		nic_data->datapath_caps2 = 0;
		nic_data->piobuf_size = ER_DZ_TX_PIOBUF_SIZE;
	}

	if (!(nic_data->datapath_caps &
	      (1 << MC_CMD_GET_CAPABILITIES_OUT_RX_PREFIX_LEN_14_LBN))) {
		netif_err(efx, probe, efx->net_dev,
			  "current firmware does not support an RX prefix\n");
		return -ENODEV;
	}

	return 0;
}

static void efx_ef10_read_licensed_features(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_LICENSING_V3_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_LICENSING_V3_OUT_LEN);
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	size_t outlen;
	int rc;

	MCDI_SET_DWORD(inbuf, LICENSING_V3_IN_OP,
		       MC_CMD_LICENSING_V3_IN_OP_REPORT_LICENSE);
	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_LICENSING_V3, inbuf, sizeof(inbuf),
				outbuf, sizeof(outbuf), &outlen);
	if (!rc && (outlen >= MC_CMD_LICENSING_V3_OUT_LEN)) {
		nic_data->licensed_features = MCDI_QWORD(outbuf,
					 LICENSING_V3_OUT_LICENSED_FEATURES);
		return;
	}
	if (rc != -MC_CMD_ERR_ENOSYS)
		efx_mcdi_display_error(efx, MC_CMD_LICENSING_V3,
				       MC_CMD_LICENSING_V3_IN_LEN, outbuf,
				       outlen, rc);


	/* LICENSING_V3 will fail on older firmwares, so fall back to
	 * LICENSED_APP_STATE.
	 */
	BUILD_BUG_ON(MC_CMD_GET_LICENSED_APP_STATE_IN_LEN >
		     MC_CMD_LICENSING_V3_IN_LEN);
	BUILD_BUG_ON(MC_CMD_GET_LICENSED_APP_STATE_OUT_LEN >
		     MC_CMD_LICENSING_V3_OUT_LEN);

	MCDI_SET_DWORD(inbuf, GET_LICENSED_APP_STATE_IN_APP_ID,
		       LICENSED_APP_ID_PTP);
	rc = efx_mcdi_rpc(efx, MC_CMD_GET_LICENSED_APP_STATE, inbuf,
			  MC_CMD_GET_LICENSED_APP_STATE_IN_LEN,
			  outbuf, MC_CMD_GET_LICENSED_APP_STATE_OUT_LEN,
			  &outlen);
	if (rc || (outlen < MC_CMD_GET_LICENSED_APP_STATE_OUT_LEN))
	    return;

	if (MCDI_QWORD(outbuf, GET_LICENSED_APP_STATE_OUT_STATE) ==
	    MC_CMD_GET_LICENSED_APP_STATE_OUT_LICENSED)
		nic_data->licensed_features |=
			(1 << LICENSED_V3_FEATURES_TX_TIMESTAMPS_LBN);
}

static int efx_ef10_get_sysclk_freq(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF(outbuf, MC_CMD_GET_CLOCK_OUT_LEN);
	int rc;

	rc = efx_mcdi_rpc(efx, MC_CMD_GET_CLOCK, NULL, 0,
			  outbuf, sizeof(outbuf), NULL);
	if (rc)
		return rc;
	rc = MCDI_DWORD(outbuf, GET_CLOCK_OUT_SYS_FREQ);
	return rc > 0 ? rc : -ERANGE;
}

static int efx_ef10_get_timer_workarounds(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	unsigned int implemented;
	unsigned int enabled;
	int rc;

	nic_data->workaround_35388 = false;
	nic_data->workaround_61265 = false;

	rc = efx_mcdi_get_workarounds(efx, &implemented, &enabled);

	if (rc == -ENOSYS) {
		/* Firmware without GET_WORKAROUNDS - not a problem. */
		rc = 0;
	} else if (rc == 0) {
		/* Bug61265 workaround is always enabled if implemented. */
		if (enabled & MC_CMD_GET_WORKAROUNDS_OUT_BUG61265)
			nic_data->workaround_61265 = true;

		if (enabled & MC_CMD_GET_WORKAROUNDS_OUT_BUG35388) {
			nic_data->workaround_35388 = true;
		} else if (implemented & MC_CMD_GET_WORKAROUNDS_OUT_BUG35388) {
			/* Workaround is implemented but not enabled.
			 * Try to enable it.
			 */
			rc = efx_mcdi_set_workaround(efx,
						     MC_CMD_WORKAROUND_BUG35388,
						     true, NULL);
			if (rc == 0)
				nic_data->workaround_35388 = true;
			/* If we failed to set the workaround just carry on. */
			rc = 0;
		}
	}

	netif_dbg(efx, probe, efx->net_dev,
		  "workaround for bug 35388 is %sabled\n",
		  nic_data->workaround_35388 ? "en" : "dis");
	netif_dbg(efx, probe, efx->net_dev,
		  "workaround for bug 61265 is %sabled\n",
		  nic_data->workaround_61265 ? "en" : "dis");

	return rc;
}

static void efx_ef10_process_timer_config(struct efx_nic *efx,
					  const efx_dword_t *data)
{
	unsigned int max_count;

	if (EFX_EF10_WORKAROUND_61265(efx)) {
		efx->timer_quantum_ns = MCDI_DWORD(data,
			GET_EVQ_TMR_PROPERTIES_OUT_MCDI_TMR_STEP_NS);
		efx->timer_max_ns = MCDI_DWORD(data,
			GET_EVQ_TMR_PROPERTIES_OUT_MCDI_TMR_MAX_NS);
	} else if (EFX_EF10_WORKAROUND_35388(efx)) {
		efx->timer_quantum_ns = MCDI_DWORD(data,
			GET_EVQ_TMR_PROPERTIES_OUT_BUG35388_TMR_NS_PER_COUNT);
		max_count = MCDI_DWORD(data,
			GET_EVQ_TMR_PROPERTIES_OUT_BUG35388_TMR_MAX_COUNT);
		efx->timer_max_ns = max_count * efx->timer_quantum_ns;
	} else {
		efx->timer_quantum_ns = MCDI_DWORD(data,
			GET_EVQ_TMR_PROPERTIES_OUT_TMR_REG_NS_PER_COUNT);
		max_count = MCDI_DWORD(data,
			GET_EVQ_TMR_PROPERTIES_OUT_TMR_REG_MAX_COUNT);
		efx->timer_max_ns = max_count * efx->timer_quantum_ns;
	}

	netif_dbg(efx, probe, efx->net_dev,
		  "got timer properties from MC: quantum %u ns; max %u ns\n",
		  efx->timer_quantum_ns, efx->timer_max_ns);
}

static int efx_ef10_get_timer_config(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF(outbuf, MC_CMD_GET_EVQ_TMR_PROPERTIES_OUT_LEN);
	int rc;

	rc = efx_ef10_get_timer_workarounds(efx);
	if (rc)
		return rc;

	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_GET_EVQ_TMR_PROPERTIES, NULL, 0,
				outbuf, sizeof(outbuf), NULL);

	if (rc == 0) {
		efx_ef10_process_timer_config(efx, outbuf);
	} else if (rc == -ENOSYS || rc == -EPERM) {
		/* Not available - fall back to Huntington defaults. */
		unsigned int quantum;

		rc = efx_ef10_get_sysclk_freq(efx);
		if (rc < 0)
			return rc;

		quantum = 1536000 / rc; /* 1536 cycles */
		efx->timer_quantum_ns = quantum;
		efx->timer_max_ns = efx->type->timer_period_max * quantum;
		rc = 0;
	} else {
		efx_mcdi_display_error(efx, MC_CMD_GET_EVQ_TMR_PROPERTIES,
				       MC_CMD_GET_EVQ_TMR_PROPERTIES_OUT_LEN,
				       NULL, 0, rc);
	}

	return rc;
}

static int efx_ef10_get_mac_address_pf(struct efx_nic *efx, u8 *mac_address)
{
	MCDI_DECLARE_BUF(outbuf, MC_CMD_GET_MAC_ADDRESSES_OUT_LEN);
	size_t outlen;
	int rc;

	BUILD_BUG_ON(MC_CMD_GET_MAC_ADDRESSES_IN_LEN != 0);

	rc = efx_mcdi_rpc(efx, MC_CMD_GET_MAC_ADDRESSES, NULL, 0,
			  outbuf, sizeof(outbuf), &outlen);
#ifdef EFX_NOT_UPSTREAM
	/* XXX remove fallback in 2013-10
	 * efx_mcdi_get_board_cfg() should also be made static in siena.c then
	 */
	if (rc == -ENOSYS && efx_port_num(efx) < 2) {
		netif_warn(efx, probe, efx->net_dev,
			   "current firmware does not support GET_MAC_ADDRESSES; falling back to GET_BOARD_CFG\n");
		return efx_mcdi_get_board_cfg(efx, mac_address, NULL, NULL);
	}
#endif
	if (rc)
		return rc;
	if (outlen < MC_CMD_GET_MAC_ADDRESSES_OUT_LEN)
		return -EIO;

	ether_addr_copy(mac_address,
			MCDI_PTR(outbuf, GET_MAC_ADDRESSES_OUT_MAC_ADDR_BASE));
#ifdef EFX_NOT_UPSTREAM
	if (mac_address[0] & 2)
		netif_warn(efx, probe, efx->net_dev,
			   "static config does not include a global MAC address pool; using local address\n");
#endif
	return 0;
}

static int efx_ef10_get_mac_address_vf(struct efx_nic *efx, u8 *mac_address)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VPORT_GET_MAC_ADDRESSES_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_VPORT_GET_MAC_ADDRESSES_OUT_LENMAX);
	size_t outlen;
	int num_addrs, rc;

	MCDI_SET_DWORD(inbuf, VPORT_GET_MAC_ADDRESSES_IN_VPORT_ID,
		       EVB_PORT_ID_ASSIGNED);
	rc = efx_mcdi_rpc(efx, MC_CMD_VPORT_GET_MAC_ADDRESSES, inbuf,
			  sizeof(inbuf), outbuf, sizeof(outbuf), &outlen);

	if (rc)
		return rc;
	if (outlen < MC_CMD_VPORT_GET_MAC_ADDRESSES_OUT_LENMIN)
		return -EIO;

	num_addrs = MCDI_DWORD(outbuf,
			       VPORT_GET_MAC_ADDRESSES_OUT_MACADDR_COUNT);

	WARN_ON(num_addrs != 1);

	ether_addr_copy(mac_address,
			MCDI_PTR(outbuf, VPORT_GET_MAC_ADDRESSES_OUT_MACADDR));

	return 0;
}

static ssize_t efx_ef10_show_forward_fcs(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct efx_nic *efx = pci_get_drvdata(to_pci_dev(dev));

	return scnprintf(buf, PAGE_SIZE, "%d\n", efx->forward_fcs);
}

static ssize_t efx_ef10_store_forward_fcs(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct efx_nic *efx = pci_get_drvdata(to_pci_dev(dev));
	bool old_forward_fcs = efx->forward_fcs;

	if (!strcmp(buf, "1\n"))
		efx->forward_fcs = true;
	else if (!strcmp(buf, "0\n"))
		efx->forward_fcs = false;
	else
		return -EINVAL;

	if (efx->forward_fcs != old_forward_fcs) {
		mutex_lock(&efx->mac_lock);
		(void)efx_mac_reconfigure(efx, false);
		mutex_unlock(&efx->mac_lock);
	}

	return count;
}

#ifdef EFX_NOT_UPSTREAM
static ssize_t efx_ef10_show_physical_port(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct efx_nic *efx = pci_get_drvdata(to_pci_dev(dev));
	return sprintf(buf, "%d\n", efx->port_num);
}
#endif

static ssize_t efx_ef10_show_link_control_flag(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct efx_nic *efx = pci_get_drvdata(to_pci_dev(dev));

	return sprintf(buf, "%d\n",
		       ((efx->mcdi->fn_flags) &
			(1 << MC_CMD_DRV_ATTACH_EXT_OUT_FLAG_LINKCTRL))
		       ? 1 : 0);
}

static ssize_t efx_ef10_show_primary_flag(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct efx_nic *efx = pci_get_drvdata(to_pci_dev(dev));

	return sprintf(buf, "%d\n",
		       ((efx->mcdi->fn_flags) &
			(1 << MC_CMD_DRV_ATTACH_EXT_OUT_FLAG_PRIMARY))
		       ? 1 : 0);
}

static struct efx_ef10_vlan *efx_ef10_find_vlan(struct efx_nic *efx, u16 vid)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_ef10_vlan *vlan;

	WARN_ON(!mutex_is_locked(&nic_data->vlan_lock));

	list_for_each_entry(vlan, &nic_data->vlan_list, list) {
		if (vlan->vid == vid)
			return vlan;
	}

	return NULL;
}

static int efx_ef10_add_vlan(struct efx_nic *efx, u16 vid)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_ef10_vlan *vlan;
	int rc;

	mutex_lock(&nic_data->vlan_lock);

	vlan = efx_ef10_find_vlan(efx, vid);
	if (vlan) {
		/* We add VID 0 on init. 8021q adds it on module init
		 * for all interfaces with VLAN filtring feature.
		 */
		if (vid == 0)
			goto done_unlock;
		netif_warn(efx, drv, efx->net_dev,
			   "VLAN %u already added\n", vid);
		rc = -EALREADY;
		goto fail_exist;
	}

	rc = -ENOMEM;
	vlan = kzalloc(sizeof(*vlan), GFP_KERNEL);
	if (!vlan)
		goto fail_alloc;

	vlan->vid = vid;

	list_add_tail(&vlan->list, &nic_data->vlan_list);

	if (efx->filter_state) {
		mutex_lock(&efx->mac_lock);
		down_write(&efx->filter_sem);
		rc = efx_ef10_filter_add_vlan(efx, vlan->vid);
		up_write(&efx->filter_sem);
		mutex_unlock(&efx->mac_lock);
		if (rc)
			goto fail_filter_add_vlan;
	}

done_unlock:
	mutex_unlock(&nic_data->vlan_lock);
	return 0;

fail_filter_add_vlan:
	list_del(&vlan->list);
	kfree(vlan);
fail_alloc:
fail_exist:
	mutex_unlock(&nic_data->vlan_lock);
	return rc;
}

static void efx_ef10_del_vlan_internal(struct efx_nic *efx,
				       struct efx_ef10_vlan *vlan)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	WARN_ON(!mutex_is_locked(&nic_data->vlan_lock));

	if (efx->filter_state) {
		down_write(&efx->filter_sem);
		efx_ef10_filter_del_vlan(efx, vlan->vid);
		up_write(&efx->filter_sem);
	}

	list_del(&vlan->list);
	kfree(vlan);
}

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NDO_VLAN_RX_ADD_VID)
static int efx_ef10_del_vlan(struct efx_nic *efx, u16 vid)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_ef10_vlan *vlan;
	int rc = 0;

	/* 8021q removes VID 0 on module unload for all interfaces
	 * with VLAN filtering feature. We need to keep it to receive
	 * untagged traffic.
	 */
	if (vid == 0)
		return 0;

	mutex_lock(&nic_data->vlan_lock);

	vlan = efx_ef10_find_vlan(efx, vid);
	if (!vlan) {
		netif_err(efx, drv, efx->net_dev,
			  "VLAN %u to be deleted not found\n", vid);
		rc = -ENOENT;
	} else {
		efx_ef10_del_vlan_internal(efx, vlan);
	}

	mutex_unlock(&nic_data->vlan_lock);

	return rc;
}
#endif

static void efx_ef10_cleanup_vlans(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_ef10_vlan *vlan, *next_vlan;

	mutex_lock(&nic_data->vlan_lock);
	list_for_each_entry_safe(vlan, next_vlan, &nic_data->vlan_list, list)
		efx_ef10_del_vlan_internal(efx, vlan);
	mutex_unlock(&nic_data->vlan_lock);
}

static DEVICE_ATTR(forward_fcs, 0644, efx_ef10_show_forward_fcs,
		   efx_ef10_store_forward_fcs);
#ifdef EFX_NOT_UPSTREAM
static DEVICE_ATTR(physical_port, 0444, efx_ef10_show_physical_port,
		   NULL);
#endif
static DEVICE_ATTR(link_control_flag, 0444, efx_ef10_show_link_control_flag,
		   NULL);
static DEVICE_ATTR(primary_flag, 0444, efx_ef10_show_primary_flag,
		   NULL);

static int efx_ef10_probe(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data;
	unsigned int bar_size = efx_ef10_bar_size(efx);
	int i, rc;

	if (WARN_ON(bar_size == 0))
		return -EIO;

	nic_data = kzalloc(sizeof(*nic_data), GFP_KERNEL);
	if (!nic_data)
		return -ENOMEM;
	efx->nic_data = nic_data;
	nic_data->efx = efx;

	/* we assume later that we can copy from this buffer in dwords */
	BUILD_BUG_ON(MCDI_CTL_SDU_LEN_MAX_V2 % 4);

	rc = efx_nic_alloc_buffer(efx, &nic_data->mcdi_buf,
				  8 + MCDI_CTL_SDU_LEN_MAX_V2, GFP_KERNEL);
	if (rc)
		goto fail1;

	/* Get the MC's warm boot count.  In case it's rebooting right
	 * now, be prepared to retry.
	 */
	i = 0;
	for (;;) {
		rc = efx_ef10_get_warm_boot_count(efx);
		if (rc >= 0)
			break;
		if (++i == 5)
			goto fail2;
		ssleep(1);
	}
	nic_data->warm_boot_count = rc;

	nic_data->rx_rss_context = EFX_EF10_RSS_CONTEXT_INVALID;

	nic_data->vport_id = EVB_PORT_ID_ASSIGNED;

	/* In case we're recovering from a crash (kexec), we want to
	 * cancel any outstanding request by the previous user of this
	 * function.  We send a special message using the least
	 * significant bits of the 'high' (doorbell) register.
	 */
	_efx_writed(efx, cpu_to_le32(1), ER_DZ_MC_DB_HWRD);

	rc = efx_mcdi_init(efx);
	if (rc)
		goto fail2;
#ifdef EFX_NOT_UPSTREAM
	/* If we're not meant to expose this as a network device, stop here */
	if (efx->mcdi->fn_flags &
			(1 << MC_CMD_DRV_ATTACH_EXT_OUT_FLAG_NO_ACTIVE_PORT))
		return 0;
#endif

	mutex_init(&nic_data->udp_tunnels_lock);
	nic_data->udp_tunnels_dirty = true;

	/* Reset (most) configuration for this function */
	rc = efx_mcdi_reset(efx, RESET_TYPE_ALL);
	if (rc)
		goto fail3;

	/* Enable event logging */
	rc = efx_mcdi_log_ctrl(efx, true, false, 0);
	if (rc)
		goto fail3;

	rc = device_create_file(&efx->pci_dev->dev, &dev_attr_link_control_flag);
	if (rc)
		goto fail3;

	rc = device_create_file(&efx->pci_dev->dev, &dev_attr_primary_flag);
	if (rc)
		goto fail4;

	rc = efx_ef10_get_pf_index(efx);
	if (rc)
		goto fail5;

	rc = efx_ef10_init_datapath_caps(efx);
	if (rc < 0)
		goto fail5;

	efx_ef10_read_licensed_features(efx);
	efx->tx_queues_per_channel = 1;
	efx->select_tx_queue = efx_ef10_select_tx_queue;
#ifdef EFX_USE_OVERLAY_TX_CSUM
	if (nic_data->datapath_caps &
	    (1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN) &&
	    !efx_ef10_is_vf(efx)) {
		efx->tx_queues_per_channel = 2;
		efx->select_tx_queue = efx_ef10_select_tx_queue_overlay;
	}
#endif
#ifdef EFX_NOT_UPSTREAM
	if (tx_non_csum_queue) {
		efx->tx_queues_per_channel = 2;
		efx->select_tx_queue = efx_ef10_select_tx_queue_non_csum;
#ifdef EFX_USE_OVERLAY_TX_CSUM
		if (nic_data->datapath_caps &
		    (1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN) &&
		    !efx_ef10_is_vf(efx)) {
			efx->tx_queues_per_channel = 3;
			efx->select_tx_queue =
				efx_ef10_select_tx_queue_non_csum_overlay;
		}
#endif
	}
#endif

	/* We can have one VI for each 8K region.
	 * Note we have more TX queues than channels, so TX queues are the
	 * limit on the number of channels we can have with our VIs.
	 */
	efx->max_channels = efx->max_tx_channels =
		min_t(unsigned int,
		      EFX_MAX_CHANNELS,
		      (bar_size /
			(EFX_VI_PAGE_SIZE * efx->tx_queues_per_channel)));
	if (efx->max_channels == 0) {
		rc = -EIO;
		goto fail5;
	}

	efx->rx_packet_len_offset =
		ES_DZ_RX_PREFIX_PKTLEN_OFST - ES_DZ_RX_PREFIX_SIZE;

	if (nic_data->datapath_caps &
	    (1 << MC_CMD_GET_CAPABILITIES_OUT_RX_INCLUDE_FCS_LBN)) {
		rc = device_create_file(&efx->pci_dev->dev,
					&dev_attr_forward_fcs);
		if (rc)
			goto fail5;
	}

	rc = efx_mcdi_port_get_number(efx);
	if (rc < 0)
		goto fail6;
	efx->port_num = rc;
#ifdef EFX_NOT_UPSTREAM
	rc = device_create_file(&efx->pci_dev->dev, &dev_attr_physical_port);
	if (rc)
		goto fail6;
#endif

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_USE_NETDEV_PERM_ADDR)
	rc = efx->type->get_mac_address(efx, efx->net_dev->perm_addr);
#else
	rc = efx->type->get_mac_address(efx, efx->perm_addr);
#endif
	if (rc) {
#ifdef EFX_NOT_UPSTREAM
		goto fail7;
#else
		goto fail6;
#endif
	}

	rc = efx_ef10_get_timer_config(efx);
	if (rc < 0) {
#ifdef EFX_NOT_UPSTREAM
		goto fail7;
#else
		goto fail6;
#endif
        }

#if defined(EFX_NOT_UPSTREAM) && defined(CONFIG_SFC_AOE)
	/* Get capabilities mask */
	nic_data->caps = 0;
	rc = efx_mcdi_get_board_cfg(efx, NULL, NULL, &nic_data->caps);
	if (rc)
		goto fail7;
#endif
	rc = efx_mcdi_mon_probe(efx);
	if (rc && rc != -EPERM) {
#ifdef EFX_NOT_UPSTREAM
		goto fail7;
#else
		goto fail6;
#endif
	}

#ifdef CONFIG_SFC_SRIOV
	efx_sriov_init_max_vfs(efx, nic_data->pf_index);
#endif

#if defined(EFX_NOT_UPSTREAM) && defined(CONFIG_SFC_AOE)
        if (aoe_enabled(efx->nic_data)) {
                rc = efx_aoe_attach(efx);
                if (rc)
                	goto fail7;
        }
#endif

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_TSTAMP)
	if (efx_ptp_use_mac_tx_timestamps(efx))
		efx_ptp_defer_probe_with_channel(efx);
	else
		efx_ptp_probe(efx, NULL);
#else
	efx_ptp_defer_probe_with_channel(efx);
#endif
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_NEED_GET_PHYS_PORT_ID)
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_PHYSFN)
#ifdef CONFIG_SFC_SRIOV
	if ((efx->pci_dev->physfn) && (!efx->pci_dev->is_physfn)) {
		struct pci_dev *pci_dev_pf = efx->pci_dev->physfn;
		struct efx_nic *efx_pf = pci_get_drvdata(pci_dev_pf);
		efx_pf->type->get_mac_address(efx_pf, nic_data->port_id);
	} else
#endif
#endif /* HAVE_PHYSFN */
		ether_addr_copy(nic_data->port_id, efx->net_dev->perm_addr);
#endif
	/* If tx_coalesce_doorbell=Y disable tx_push, we do this here to avoid
	 * additional checks on the fast path */
	if (tx_coalesce_doorbell) {
		netif_info(efx, drv, efx->net_dev,
			   "Tx push disabled due to tx_coalesce_doorbell=Y\n");
		tx_push_max_fill=0;
	}

	INIT_LIST_HEAD(&nic_data->vlan_list);
	mutex_init(&nic_data->vlan_lock);

	/* Add unspecified VID to support VLAN filtering being disabled */
	rc = efx_ef10_add_vlan(efx, EFX_FILTER_VID_UNSPEC);
	if (rc)
		goto fail_add_vid_unspec;

	/* If VLAN filtering is enabled, we need VID 0 to get untagged
	 * traffic.  It is added automatically if 8021q module is loaded,
	 * but we can't rely on it since module may be not loaded.
	 */
	rc = efx_ef10_add_vlan(efx, 0);
	if (rc)
		goto fail_add_vid_0;

	return 0;

fail_add_vid_0:
	efx_ef10_cleanup_vlans(efx);
fail_add_vid_unspec:
	mutex_destroy(&nic_data->vlan_lock);
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_TSTAMP)
	efx_ptp_remove(efx);
#endif

#if defined(EFX_NOT_UPSTREAM) && defined(CONFIG_SFC_AOE)
        if ((aoe_enabled(efx->nic_data)) && efx->aoe_data) {
                efx_aoe_detach(efx);
        }
#endif

	efx_mcdi_mon_remove(efx);

#ifdef EFX_NOT_UPSTREAM
fail7:
	device_remove_file(&efx->pci_dev->dev, &dev_attr_physical_port);
#endif

fail6:
	if (nic_data->datapath_caps &
	    (1 << MC_CMD_GET_CAPABILITIES_OUT_RX_INCLUDE_FCS_LBN))
		device_remove_file(&efx->pci_dev->dev, &dev_attr_forward_fcs);
fail5:
	device_remove_file(&efx->pci_dev->dev, &dev_attr_primary_flag);
fail4:
	device_remove_file(&efx->pci_dev->dev, &dev_attr_link_control_flag);
fail3:
	efx_mcdi_detach(efx);

	mutex_lock(&nic_data->udp_tunnels_lock);
	memset(nic_data->udp_tunnels, 0, sizeof(nic_data->udp_tunnels));
	(void) efx_ef10_set_udp_tnl_ports(efx, true);
	mutex_unlock(&nic_data->udp_tunnels_lock);
	mutex_destroy(&nic_data->udp_tunnels_lock);

	efx_mcdi_fini(efx);
fail2:
	efx_nic_free_buffer(efx, &nic_data->mcdi_buf);
fail1:
	kfree(nic_data);
	efx->nic_data = NULL;
	return rc;
}

static int efx_ef10_probe_pf(struct efx_nic *efx)
{
#ifdef EFX_NOT_UPSTREAM
	unsigned int desired_bandwidth = EFX_BW_PCIE_GEN2_X8;

	if (efx->pci_dev->device == 0x0923) {
		/* Greenport devices can make use of 8 lanes of Gen3. */
		desired_bandwidth = EFX_BW_PCIE_GEN3_X8;
	} else if (efx->pci_dev->device == 0x0a03) {
		/* Medford devices want 16 lane, even though not all have that.
		 * We suppress this optimism in efx_nic_check_pcie_link().
		 */
		desired_bandwidth = EFX_BW_PCIE_GEN3_X16;
	}

	efx_nic_check_pcie_link(efx, desired_bandwidth, NULL, NULL);
#endif
	return efx_ef10_probe(efx);
}

int efx_ef10_vswitch_alloc(struct efx_nic *efx, unsigned int port_id,
			   unsigned int vswitch_type)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VSWITCH_ALLOC_IN_LEN);
	int rc;

	MCDI_SET_DWORD(inbuf, VSWITCH_ALLOC_IN_UPSTREAM_PORT_ID, port_id);
	MCDI_SET_DWORD(inbuf, VSWITCH_ALLOC_IN_TYPE, vswitch_type);
	MCDI_SET_DWORD(inbuf, VSWITCH_ALLOC_IN_NUM_VLAN_TAGS, 2);
	MCDI_POPULATE_DWORD_1(inbuf, VSWITCH_ALLOC_IN_FLAGS,
			      VSWITCH_ALLOC_IN_FLAG_AUTO_PORT, 0);

	/* Quietly try to allocate 2 VLAN tags */
	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_VSWITCH_ALLOC, inbuf, sizeof(inbuf),
				NULL, 0, NULL);

	/* If 2 VLAN tags is too many, revert to trying with 1 VLAN tags */
	if (rc == -EPROTO) {
		MCDI_SET_DWORD(inbuf, VSWITCH_ALLOC_IN_NUM_VLAN_TAGS, 1);
		rc = efx_mcdi_rpc(efx, MC_CMD_VSWITCH_ALLOC, inbuf,
				  sizeof(inbuf), NULL, 0, NULL);
	} else if (rc) {
		efx_mcdi_display_error(efx, MC_CMD_VSWITCH_ALLOC,
				       MC_CMD_VSWITCH_ALLOC_IN_LEN, NULL, 0, rc);
	}
	return rc;
}

int efx_ef10_vswitch_free(struct efx_nic *efx, unsigned int port_id)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VSWITCH_FREE_IN_LEN);

	MCDI_SET_DWORD(inbuf, VSWITCH_FREE_IN_UPSTREAM_PORT_ID, port_id);

	return efx_mcdi_rpc(efx, MC_CMD_VSWITCH_FREE, inbuf, sizeof(inbuf),
			    NULL, 0, NULL);
}

int efx_ef10_vport_alloc(struct efx_nic *efx, unsigned int port_id_in,
			 unsigned int vport_type, u16 vlan, bool vlan_restrict,
			 unsigned int *port_id_out)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VPORT_ALLOC_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_VPORT_ALLOC_OUT_LEN);
	size_t outlen;
	int rc;

	EFX_WARN_ON_PARANOID(!port_id_out);

	MCDI_SET_DWORD(inbuf, VPORT_ALLOC_IN_UPSTREAM_PORT_ID, port_id_in);
	MCDI_SET_DWORD(inbuf, VPORT_ALLOC_IN_TYPE, vport_type);
	MCDI_SET_DWORD(inbuf, VPORT_ALLOC_IN_NUM_VLAN_TAGS,
		       (vlan != EFX_FILTER_VID_UNSPEC));
	MCDI_POPULATE_DWORD_2(inbuf, VPORT_ALLOC_IN_FLAGS,
			      VPORT_ALLOC_IN_FLAG_AUTO_PORT, 0,
			      VPORT_ALLOC_IN_FLAG_VLAN_RESTRICT, vlan_restrict);
	if (vlan != EFX_FILTER_VID_UNSPEC)
		MCDI_POPULATE_DWORD_1(inbuf, VPORT_ALLOC_IN_VLAN_TAGS,
				      VPORT_ALLOC_IN_VLAN_TAG_0, vlan);

	rc = efx_mcdi_rpc(efx, MC_CMD_VPORT_ALLOC, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), &outlen);
	if (rc)
		return rc;
	if (outlen < MC_CMD_VPORT_ALLOC_OUT_LEN)
		return -EIO;

	*port_id_out = MCDI_DWORD(outbuf, VPORT_ALLOC_OUT_VPORT_ID);
	return 0;
}

int efx_ef10_vport_free(struct efx_nic *efx, unsigned int port_id)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VPORT_FREE_IN_LEN);

	MCDI_SET_DWORD(inbuf, VPORT_FREE_IN_VPORT_ID, port_id);

	return efx_mcdi_rpc(efx, MC_CMD_VPORT_FREE, inbuf, sizeof(inbuf),
			    NULL, 0, NULL);
}

int efx_ef10_vadaptor_query(struct efx_nic *efx, unsigned int port_id,
			    u32 *port_flags, u32 *vadaptor_flags,
			    unsigned int *vlan_tags)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VADAPTOR_QUERY_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_VADAPTOR_QUERY_OUT_LEN);
	size_t outlen;
	int rc;

	if (nic_data->datapath_caps &
	    (1 << MC_CMD_GET_CAPABILITIES_OUT_VADAPTOR_QUERY_LBN)) {
		MCDI_SET_DWORD(inbuf, VADAPTOR_QUERY_IN_UPSTREAM_PORT_ID,
			       port_id);

		rc = efx_mcdi_rpc(efx, MC_CMD_VADAPTOR_QUERY, inbuf, sizeof(inbuf),
				  outbuf, sizeof(outbuf), &outlen);
		if (rc)
			return rc;

		if (outlen < sizeof(outbuf)) {
			rc = -EIO;
			return rc;
		}
	}

	if (port_flags)
		*port_flags = MCDI_DWORD(outbuf, VADAPTOR_QUERY_OUT_PORT_FLAGS);
	if (vadaptor_flags)
		*vadaptor_flags =
			MCDI_DWORD(outbuf, VADAPTOR_QUERY_OUT_VADAPTOR_FLAGS);
	if (vlan_tags)
		*vlan_tags =
			MCDI_DWORD(outbuf,
				   VADAPTOR_QUERY_OUT_NUM_AVAILABLE_VLAN_TAGS);

	return 0;
}

int efx_ef10_vadaptor_alloc(struct efx_nic *efx, unsigned int port_id)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VADAPTOR_ALLOC_IN_LEN);

	MCDI_SET_DWORD(inbuf, VADAPTOR_ALLOC_IN_UPSTREAM_PORT_ID, port_id);
	MCDI_POPULATE_DWORD_1(inbuf, VADAPTOR_ALLOC_IN_FLAGS,
			      VADAPTOR_ALLOC_IN_FLAG_PERMIT_SET_MAC_WHEN_FILTERS_INSTALLED,
			      1);
	return efx_mcdi_rpc(efx, MC_CMD_VADAPTOR_ALLOC, inbuf, sizeof(inbuf),
			    NULL, 0, NULL);
}

int efx_ef10_vadaptor_free(struct efx_nic *efx, unsigned int port_id)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VADAPTOR_FREE_IN_LEN);

	MCDI_SET_DWORD(inbuf, VADAPTOR_FREE_IN_UPSTREAM_PORT_ID, port_id);
	return efx_mcdi_rpc(efx, MC_CMD_VADAPTOR_FREE, inbuf, sizeof(inbuf),
			    NULL, 0, NULL);
}

int efx_ef10_evb_port_assign(struct efx_nic *efx, unsigned int port_id,
			     unsigned int vf_fn)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_EVB_PORT_ASSIGN_IN_LEN);
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	MCDI_SET_DWORD(inbuf, EVB_PORT_ASSIGN_IN_PORT_ID, port_id);
	MCDI_POPULATE_DWORD_2(inbuf, EVB_PORT_ASSIGN_IN_FUNCTION,
			      EVB_PORT_ASSIGN_IN_PF, nic_data->pf_index,
			      EVB_PORT_ASSIGN_IN_VF, vf_fn);

	return efx_mcdi_rpc(efx, MC_CMD_EVB_PORT_ASSIGN, inbuf, sizeof(inbuf),
			    NULL, 0, NULL);
}

int efx_ef10_vport_add_mac(struct efx_nic *efx, unsigned int port_id, u8 *mac)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VPORT_ADD_MAC_ADDRESS_IN_LEN);

	MCDI_SET_DWORD(inbuf, VPORT_ADD_MAC_ADDRESS_IN_VPORT_ID, port_id);
	ether_addr_copy(MCDI_PTR(inbuf, VPORT_ADD_MAC_ADDRESS_IN_MACADDR), mac);

	return efx_mcdi_rpc(efx, MC_CMD_VPORT_ADD_MAC_ADDRESS, inbuf,
			    sizeof(inbuf), NULL, 0, NULL);
}

int efx_ef10_vport_del_mac(struct efx_nic *efx, unsigned int port_id, u8 *mac)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VPORT_DEL_MAC_ADDRESS_IN_LEN);

	MCDI_SET_DWORD(inbuf, VPORT_DEL_MAC_ADDRESS_IN_VPORT_ID, port_id);
	ether_addr_copy(MCDI_PTR(inbuf, VPORT_DEL_MAC_ADDRESS_IN_MACADDR), mac);

	return efx_mcdi_rpc(efx, MC_CMD_VPORT_DEL_MAC_ADDRESS, inbuf,
			    sizeof(inbuf), NULL, 0, NULL);
}

static int efx_ef10_free_vis(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF_ERR(outbuf);
	size_t outlen;
	int rc = efx_mcdi_rpc_quiet(efx, MC_CMD_FREE_VIS, NULL, 0,
				    outbuf, sizeof(outbuf), &outlen);

	/* -EALREADY means nothing to free, so ignore */
	if (rc == -EALREADY)
		rc = 0;
	if (rc)
		efx_mcdi_display_error(efx, MC_CMD_FREE_VIS, 0, outbuf, outlen,
				       rc);
	return rc;
}

#ifdef EFX_USE_PIO

static void efx_ef10_free_piobufs(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	MCDI_DECLARE_BUF(inbuf, MC_CMD_FREE_PIOBUF_IN_LEN);
	unsigned int i;
	int rc;

	BUILD_BUG_ON(MC_CMD_FREE_PIOBUF_OUT_LEN != 0);

	for (i = 0; i < nic_data->n_piobufs; i++) {
		MCDI_SET_DWORD(inbuf, FREE_PIOBUF_IN_PIOBUF_HANDLE,
			       nic_data->piobuf_handle[i]);
		rc = efx_mcdi_rpc(efx, MC_CMD_FREE_PIOBUF, inbuf, sizeof(inbuf),
				  NULL, 0, NULL);
		WARN_ON(rc && rc != -ENETDOWN && !efx_ef10_hw_unavailable(efx));
	}

	nic_data->n_piobufs = 0;
}

static int efx_ef10_alloc_piobufs(struct efx_nic *efx, unsigned int n)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	MCDI_DECLARE_BUF(outbuf, MC_CMD_ALLOC_PIOBUF_OUT_LEN);
	unsigned int i;
	size_t outlen;
	int rc = 0;

	BUILD_BUG_ON(MC_CMD_ALLOC_PIOBUF_IN_LEN != 0);

	for (i = 0; i < n; i++) {
		rc = efx_mcdi_rpc_quiet(efx, MC_CMD_ALLOC_PIOBUF, NULL, 0,
				  outbuf, sizeof(outbuf), &outlen);
		if (rc) {
			/* Don't display the MC error if we didn't have space
			 * for a VF.
			 */
			if (!(efx_ef10_is_vf(efx) && rc == -ENOSPC))
				efx_mcdi_display_error(efx, MC_CMD_ALLOC_PIOBUF,
						0, outbuf, outlen, rc);
			break;
		}
		if (outlen < MC_CMD_ALLOC_PIOBUF_OUT_LEN) {
			rc = -EIO;
			break;
		}
		nic_data->piobuf_handle[i] =
			MCDI_DWORD(outbuf, ALLOC_PIOBUF_OUT_PIOBUF_HANDLE);
		netif_dbg(efx, probe, efx->net_dev,
			  "allocated PIO buffer %u handle %x\n", i,
			  nic_data->piobuf_handle[i]);
	}

	nic_data->n_piobufs = i;
	if (rc)
		efx_ef10_free_piobufs(efx);
	return rc;
}

static int efx_ef10_link_piobufs(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	_MCDI_DECLARE_BUF(inbuf,
			 max(MC_CMD_LINK_PIOBUF_IN_LEN,
			     MC_CMD_UNLINK_PIOBUF_IN_LEN));
	struct efx_channel *channel;
	struct efx_tx_queue *tx_queue;
	unsigned int offset, index;
	int rc;

	BUILD_BUG_ON(MC_CMD_LINK_PIOBUF_OUT_LEN != 0);
	BUILD_BUG_ON(MC_CMD_UNLINK_PIOBUF_OUT_LEN != 0);

	memset(inbuf, 0, sizeof(inbuf));

	/* Link a buffer to each VI in the write-combining mapping */
	for (index = 0; index < nic_data->n_piobufs; ++index) {
		MCDI_SET_DWORD(inbuf, LINK_PIOBUF_IN_PIOBUF_HANDLE,
			       nic_data->piobuf_handle[index]);
		MCDI_SET_DWORD(inbuf, LINK_PIOBUF_IN_TXQ_INSTANCE,
			       nic_data->pio_write_vi_base + index);
		rc = efx_mcdi_rpc(efx, MC_CMD_LINK_PIOBUF,
				  inbuf, MC_CMD_LINK_PIOBUF_IN_LEN,
				  NULL, 0, NULL);
		if (rc) {
			netif_err(efx, drv, efx->net_dev,
				  "failed to link VI %u to PIO buffer %u (%d)\n",
				  nic_data->pio_write_vi_base + index, index,
				  rc);
			goto fail;
		}
		netif_dbg(efx, probe, efx->net_dev,
			  "linked VI %u to PIO buffer %u\n",
			  nic_data->pio_write_vi_base + index, index);
	}

	/* Link a buffer to each TX queue */
	efx_for_each_channel(channel, efx) {
		efx_for_each_channel_tx_queue(tx_queue, channel) {
			/* We assign the PIO buffers to queues in
			 * reverse order to allow for the following
			 * special case.
			 */
			offset = ((efx->tx_channel_offset + efx->n_tx_channels -
				   tx_queue->channel->channel - 1) *
				  efx_piobuf_size);
			index = offset / nic_data->piobuf_size;
			offset = offset % nic_data->piobuf_size;

			/* When the host page size is 4K, the first
			 * host page in the WC mapping may be within
			 * the same VI page as the last TX queue.  We
			 * can only link one buffer to each VI.
			 */
			if (tx_queue->queue == nic_data->pio_write_vi_base) {
				BUG_ON(index != 0);
				rc = 0;
			} else {
				MCDI_SET_DWORD(inbuf,
					       LINK_PIOBUF_IN_PIOBUF_HANDLE,
					       nic_data->piobuf_handle[index]);
				MCDI_SET_DWORD(inbuf,
					       LINK_PIOBUF_IN_TXQ_INSTANCE,
					       tx_queue->queue);
				rc = efx_mcdi_rpc(efx, MC_CMD_LINK_PIOBUF,
						  inbuf, MC_CMD_LINK_PIOBUF_IN_LEN,
						  NULL, 0, NULL);
			}

			if (rc) {
				/* This is non-fatal; the TX path just
				 * won't use PIO for this queue
				 */
				netif_err(efx, drv, efx->net_dev,
					  "failed to link VI %u to PIO buffer %u (%d)\n",
					  tx_queue->queue, index, rc);
				tx_queue->piobuf = NULL;

				if (rc == -ENETDOWN)
					goto fail;
			} else {
				tx_queue->piobuf =
					nic_data->pio_write_base +
					index * EFX_VI_PAGE_SIZE + offset;
				tx_queue->piobuf_offset = offset;
				netif_dbg(efx, probe, efx->net_dev,
					  "linked VI %u to PIO buffer %u offset %x addr %p\n",
					  tx_queue->queue, index,
					  tx_queue->piobuf_offset,
					  tx_queue->piobuf);
			}
		}
	}

	return 0;

fail:
	while (index--) {
		MCDI_SET_DWORD(inbuf, UNLINK_PIOBUF_IN_TXQ_INSTANCE,
			       nic_data->pio_write_vi_base + index);
		efx_mcdi_rpc(efx, MC_CMD_UNLINK_PIOBUF,
			     inbuf, MC_CMD_UNLINK_PIOBUF_IN_LEN,
			     NULL, 0, NULL);
	}
	return rc;
}

static void efx_ef10_forget_old_piobufs(struct efx_nic *efx)
{
	struct efx_channel *channel;
	struct efx_tx_queue *tx_queue;

	/* All our existing PIO buffers went away */
	efx_for_each_channel(channel, efx)
		efx_for_each_channel_tx_queue(tx_queue, channel)
			tx_queue->piobuf = NULL;
}

#else /* !EFX_USE_PIO */

static int efx_ef10_alloc_piobufs(struct efx_nic *efx, unsigned int n)
{
	return n == 0 ? 0 : -ENOBUFS;
}

static int efx_ef10_link_piobufs(struct efx_nic *efx)
{
	return 0;
}

static void efx_ef10_free_piobufs(struct efx_nic *efx)
{
}

static void efx_ef10_forget_old_piobufs(struct efx_nic *efx)
{
}

#endif /* EFX_USE_PIO */

static void efx_ef10_remove(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
#if defined(CONFIG_SFC_SRIOV) && (!defined(EFX_USE_KCOMPAT) || (defined(EFX_HAVE_NDO_SET_VF_MAC) && defined(EFX_HAVE_PHYSFN)))
	struct efx_ef10_nic_data *nic_data_pf;
	struct pci_dev *pci_dev_pf;
	struct efx_nic *efx_pf;
	struct ef10_vf *vf;

	if (efx->pci_dev->is_virtfn) {
		pci_dev_pf = efx->pci_dev->physfn;
		if (pci_dev_pf) {
			efx_pf = pci_get_drvdata(pci_dev_pf);
			nic_data_pf = efx_pf->nic_data;
			vf = nic_data_pf->vf + nic_data->vf_index;
			vf->efx = NULL;
		} else
			netif_info(efx, drv, efx->net_dev,
				   "Could not get the PF id from VF\n");
	}
#endif

	efx_ef10_cleanup_vlans(efx);
	mutex_destroy(&nic_data->vlan_lock);

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_TSTAMP)
	efx_ptp_remove(efx);
#endif

#if defined(EFX_NOT_UPSTREAM) && defined(CONFIG_SFC_AOE)
	if ((aoe_enabled(nic_data)) && efx->aoe_data) {
		efx_aoe_detach(efx);
	}
#endif

	efx_mcdi_mon_remove(efx);

	efx_ef10_rx_free_indir_table(efx);

	if (nic_data->wc_membase)
		iounmap(nic_data->wc_membase);

	(void) efx_ef10_free_vis(efx);

	if (!nic_data->must_restore_piobufs)
		efx_ef10_free_piobufs(efx);

#ifdef EFX_NOT_UPSTREAM
	device_remove_file(&efx->pci_dev->dev, &dev_attr_physical_port);
#endif

	if (nic_data->datapath_caps &
	    (1 << MC_CMD_GET_CAPABILITIES_OUT_RX_INCLUDE_FCS_LBN))
		device_remove_file(&efx->pci_dev->dev, &dev_attr_forward_fcs);

	device_remove_file(&efx->pci_dev->dev, &dev_attr_primary_flag);
	device_remove_file(&efx->pci_dev->dev, &dev_attr_link_control_flag);

	efx_mcdi_detach(efx);

	memset(nic_data->udp_tunnels, 0, sizeof(nic_data->udp_tunnels));
	mutex_lock(&nic_data->udp_tunnels_lock);
	(void) efx_ef10_set_udp_tnl_ports(efx, true);
	mutex_unlock(&nic_data->udp_tunnels_lock);

	mutex_destroy(&nic_data->udp_tunnels_lock);

	efx_mcdi_fini(efx);
	efx_nic_free_buffer(efx, &nic_data->mcdi_buf);
	kfree(nic_data);
	efx->nic_data = NULL;
}

#if !defined(CONFIG_SFC_SRIOV)
static int efx_ef10_probe_vf(struct efx_nic *efx __attribute__ ((unused)))
{
	return 0;
}
#else
static int efx_ef10_probe_vf(struct efx_nic *efx)
{
	int rc;
	struct efx_ef10_nic_data *nic_data;
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_PHYSFN)
	struct pci_dev *pci_dev_pf;

	/* If the parent PF has no VF data structure, it doesn't know about this
	 * VF so fail probe.  The VF needs to be re-created.  This can happen
	 * if the PF driver is unloaded while the VF is assigned to a guest.
	 */
	pci_dev_pf = efx->pci_dev->physfn;
	if (pci_dev_pf) {
		struct efx_nic *efx_pf = pci_get_drvdata(pci_dev_pf);
		struct efx_ef10_nic_data *nic_data_pf = efx_pf->nic_data;

		if (!nic_data_pf->vf) {
			netif_info(efx, drv, efx->net_dev,
				   "The VF cannot link to its parent PF; "
				   "please destroy and re-create the VF\n");
			return -EBUSY;
		}
	}
#endif

	rc = efx_ef10_probe(efx);
	if (rc)
		return rc;

	nic_data = efx->nic_data;

	rc = efx_ef10_get_vf_index(efx);
	if (rc)
		goto fail;

#if !defined(EFX_USE_KCOMPAT) || (defined(EFX_HAVE_NDO_SET_VF_MAC) && defined(EFX_HAVE_PHYSFN))
	if (efx->pci_dev->is_virtfn) {
		if (efx->pci_dev->physfn) {
			struct efx_nic *efx_pf = pci_get_drvdata(efx->pci_dev->physfn);
			struct efx_ef10_nic_data *nic_data_pf = efx_pf->nic_data;

			nic_data_pf->vf[nic_data->vf_index].efx = efx;
			nic_data_pf->vf[nic_data->vf_index].pci_dev =
				efx->pci_dev;
		} else
			netif_info(efx, drv, efx->net_dev,
				   "Could not get the PF id from VF\n");
	}
#endif

	INIT_DELAYED_WORK(&nic_data->vf_stats_work,
			  efx_ef10_vf_update_stats_work);

	return 0;

fail:
	efx_ef10_remove(efx);
	return rc;
}
#endif /* CONFIG_SFC_SRIOV */

static int efx_ef10_alloc_vis(struct efx_nic *efx,
			      unsigned int min_vis, unsigned int max_vis)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_ALLOC_VIS_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_ALLOC_VIS_EXT_OUT_LEN);
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	size_t outlen;
	int rc;

	MCDI_SET_DWORD(inbuf, ALLOC_VIS_IN_MIN_VI_COUNT, min_vis);
	MCDI_SET_DWORD(inbuf, ALLOC_VIS_IN_MAX_VI_COUNT, max_vis);
	rc = efx_mcdi_rpc(efx, MC_CMD_ALLOC_VIS, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), &outlen);
	if (rc != 0)
		return rc;

	if (outlen < MC_CMD_ALLOC_VIS_OUT_LEN)
		return -EIO;

	netif_dbg(efx, drv, efx->net_dev, "base VI is A0x%03x\n",
		  MCDI_DWORD(outbuf, ALLOC_VIS_OUT_VI_BASE));

	nic_data->vi_base = MCDI_DWORD(outbuf, ALLOC_VIS_OUT_VI_BASE);
	efx->ef10_resources.vi_base = nic_data->vi_base;
	if (outlen == MC_CMD_ALLOC_VIS_EXT_OUT_LEN)
		efx->ef10_resources.vi_shift = MCDI_DWORD(outbuf, ALLOC_VIS_EXT_OUT_VI_SHIFT);
	nic_data->n_allocated_vis = MCDI_DWORD(outbuf, ALLOC_VIS_OUT_VI_COUNT);
	return 0;
}

/* Note that the failure path of this function does not free
 * resources, as this will be done by efx_ef10_remove().
 */
static int efx_ef10_dimension_resources(struct efx_nic *efx)
{
	struct efx_dl_ef10_resources *res = &efx->ef10_resources;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	unsigned int min_vis = max_t(unsigned int, efx->tx_queues_per_channel,
				     separate_tx_channels ? 2 : 1);
	unsigned int uc_mem_map_size, wc_mem_map_size;
	unsigned int channel_vis, pio_write_vi_base, max_vis;
	void __iomem *membase;
	int rc;

	channel_vis = max(efx->n_channels,
			  efx->n_tx_channels * efx->tx_queues_per_channel);

#ifdef EFX_USE_PIO
	/* Try to allocate PIO buffers if wanted and if the full
	 * number of PIO buffers would be sufficient to allocate one
	 * copy-buffer per TX channel.  Failure is non-fatal, as there
	 * are only a small number of PIO buffers shared between all
	 * functions of the controller.
	 */
	if (efx_piobuf_size != 0 &&
	    nic_data->piobuf_size / efx_piobuf_size * EF10_TX_PIOBUF_COUNT >=
	    efx->n_tx_channels) {
		unsigned int n_piobufs =
			DIV_ROUND_UP(efx->n_tx_channels,
				     nic_data->piobuf_size / efx_piobuf_size);

		rc = efx_ef10_alloc_piobufs(efx, n_piobufs);
		if (rc == -ENOSPC)
			netif_dbg(efx, probe, efx->net_dev,
				  "out of PIO buffers; cannot allocate more\n");
		else if (rc == -EPERM)
			netif_dbg(efx, probe, efx->net_dev,
				  "not permitted to allocate PIO buffers\n");
		else if (rc)
			netif_err(efx, probe, efx->net_dev,
				  "failed to allocate PIO buffers (%d)\n", rc);
		else
			netif_dbg(efx, probe, efx->net_dev,
				  "allocated %u PIO buffers\n", n_piobufs);
	}
#else
	nic_data->n_piobufs = 0;
#endif

	/* PIO buffers should be mapped with write-combining enabled,
	 * and we want to make single UC and WC mappings rather than
	 * several of each (in fact that's the only option if host
	 * page size is >4K).  So we may allocate some extra VIs just
	 * for writing PIO buffers through.
	 *
	 * The UC mapping contains (channel_vis - 1) complete VIs and the
	 * first half of the next VI.  Then the WC mapping begins with
	 * the second half of this last VI.
	 */
	uc_mem_map_size = PAGE_ALIGN((channel_vis - 1) * EFX_VI_PAGE_SIZE +
				     ER_DZ_TX_PIOBUF);
	if (nic_data->n_piobufs) {
		/* pio_write_vi_base rounds down to give the number of complete
		 * VIs inside the UC mapping.
		 */
		pio_write_vi_base = uc_mem_map_size / EFX_VI_PAGE_SIZE;
		wc_mem_map_size = (PAGE_ALIGN((pio_write_vi_base +
					       nic_data->n_piobufs) *
					      EFX_VI_PAGE_SIZE) -
				   uc_mem_map_size);
		max_vis = pio_write_vi_base + nic_data->n_piobufs;
	} else {
		pio_write_vi_base = 0;
		wc_mem_map_size = 0;
		max_vis = channel_vis;
	}

#ifdef EFX_NOT_UPSTREAM
	max_vis += efx_target_num_vis >= 0 ?
			efx_target_num_vis :
			efx_ef10_is_vf(efx) ? EF10_ONLOAD_VF_VIS
					    : EF10_ONLOAD_PF_VIS;
#endif

	/* In case the last attached driver failed to free VIs, do it now */
	rc = efx_ef10_free_vis(efx);
	if (rc != 0)
		return rc;

	rc = efx_ef10_alloc_vis(efx, min_vis, max_vis);
	if (rc != 0)
		return rc;

	if (nic_data->n_allocated_vis < channel_vis) {
		netif_info(efx, drv, efx->net_dev,
			   "Could not allocate enough VIs to satisfy RSS "
			   "requirements. Performance may be impaired.\n");
		/* We didn't get the VIs to populate our channels. We could keep
		 * what we got but then we'd have more interrupts than we need.
		 * Instead calculate new max_channels and restart */
		efx->max_channels = nic_data->n_allocated_vis;
		efx->max_tx_channels =
			nic_data->n_allocated_vis / efx->tx_queues_per_channel;

		efx_ef10_free_vis(efx);
		return -EAGAIN;
	}

	/* If we didn't get enough VIs to map all the PIO buffers, free the
	 * PIO buffers
	 */
	if (nic_data->n_piobufs &&
	    nic_data->n_allocated_vis <
	    pio_write_vi_base + nic_data->n_piobufs) {
		netif_dbg(efx, probe, efx->net_dev,
			  "%u VIs are not sufficient to map %u PIO buffers\n",
			  nic_data->n_allocated_vis, nic_data->n_piobufs);
		efx_ef10_free_piobufs(efx);
	}

	/* Extend the original UC mapping of the memory BAR */
	membase = ioremap_nocache(efx->membase_phys, uc_mem_map_size);
	if (!membase) {
		netif_err(efx, probe, efx->net_dev,
			  "could not extend memory BAR to %x\n",
			  uc_mem_map_size);
		return -ENOMEM;
	}
	iounmap(efx->membase);
	efx->membase = membase;

	/* Set up the WC mapping if needed */
	if (wc_mem_map_size) {
		nic_data->wc_membase = ioremap_wc(efx->membase_phys +
						  uc_mem_map_size,
						  wc_mem_map_size);
		if (!nic_data->wc_membase) {
			netif_err(efx, probe, efx->net_dev,
				  "could not allocate WC mapping of size %x\n",
				  wc_mem_map_size);
			return -ENOMEM;
		}
		nic_data->pio_write_vi_base = pio_write_vi_base;
		nic_data->pio_write_base =
			nic_data->wc_membase +
			(pio_write_vi_base * EFX_VI_PAGE_SIZE + ER_DZ_TX_PIOBUF -
			 uc_mem_map_size);

		rc = efx_ef10_link_piobufs(efx);
		if (rc)
			efx_ef10_free_piobufs(efx);
		if (rc == -ENETDOWN)
			return rc;
	}

	netif_dbg(efx, probe, efx->net_dev,
		  "memory BAR at %llx (virtual %p+%x UC, %p+%x WC)\n",
		  (unsigned long long)efx->membase_phys, efx->membase,
		  uc_mem_map_size, nic_data->wc_membase, wc_mem_map_size);

	res->vi_min = DIV_ROUND_UP(uc_mem_map_size + wc_mem_map_size,
				   EFX_VI_PAGE_SIZE);
	res->vi_lim = nic_data->n_allocated_vis;
	res->timer_quantum_ns = efx->timer_quantum_ns;
	res->rss_channel_count = efx->rss_spread;
	res->rx_channel_count = efx->n_rx_channels;
	if (EFX_INT_MODE_USE_MSI(efx))
		res->flags |= EFX_DL_EF10_USE_MSI;

	efx->dl_info = &res->hdr;


#if defined(EFX_NOT_UPSTREAM) && defined(CONFIG_SFC_AOE)
	/* If the board is AOE enabled */
	if (efx->nic_data && (aoe_enabled(efx->nic_data))) {
		/* Number of MACs is hardcoded for now */
		efx->aoe_resources.internal_macs = 2;
		efx->aoe_resources.external_macs = 2;
		efx->aoe_resources.hdr.type = EFX_DL_AOE_RESOURCES;
		efx->aoe_resources.hdr.next = efx->ef10_resources.hdr.next;
		efx->ef10_resources.hdr.next = &efx->aoe_resources.hdr;
	}
#endif
	return 0;
}

#ifdef EFX_NOT_UPSTREAM
static struct efx_tx_queue *
efx_ef10_select_tx_queue_non_csum(struct efx_channel *channel,
				  struct sk_buff *skb)
{
	return &channel->tx_queue[skb->ip_summed ? EFX_TXQ_TYPE_CSUM_OFFLOAD :
						   EFX_TXQ_TYPE_NO_OFFLOAD];
}
#endif

static void efx_ef10_set_tx_queue_csum(struct efx_tx_queue *tx_queue,
				       unsigned int txq_type)
{
	if (tx_queue->csum_offload != txq_type) {
		bool outer_csum = txq_type & EFX_TXQ_TYPE_CSUM_OFFLOAD;
		bool inner_csum = txq_type & EFX_TXQ_TYPE_INNER_CSUM_OFFLOAD;
		bool tso_v2 = ((tx_queue->tso_version == 2) &&
			       !tx_queue->timestamping);
		struct efx_tx_buffer *buffer =
			efx_tx_queue_get_insert_buffer(tx_queue);

		buffer->flags = EFX_TX_BUF_OPTION;
		buffer->len = buffer->unmap_len = 0;
		EFX_POPULATE_QWORD_7(buffer->option,
				     ESF_DZ_TX_DESC_IS_OPT, true,
				     ESF_DZ_TX_OPTION_TYPE,
				     ESE_DZ_TX_OPTION_DESC_CRC_CSUM,
				     ESF_DZ_TX_OPTION_UDP_TCP_CSUM,
					outer_csum,
				     ESF_DZ_TX_OPTION_IP_CSUM,
					outer_csum && !tso_v2,
				     ESF_DZ_TX_OPTION_INNER_UDP_TCP_CSUM,
					inner_csum,
				     ESF_DZ_TX_OPTION_INNER_IP_CSUM,
					inner_csum && !tso_v2,
				     ESF_DZ_TX_TIMESTAMP,
					tx_queue->timestamping);

		++tx_queue->insert_count;
		tx_queue->csum_offload = txq_type;
	}
}

static struct efx_tx_queue *
efx_ef10_select_tx_queue(struct efx_channel *channel,
			 struct sk_buff *skb)
{
	unsigned int txq_type = skb->ip_summed == CHECKSUM_PARTIAL ?
				EFX_TXQ_TYPE_CSUM_OFFLOAD :
				EFX_TXQ_TYPE_NO_OFFLOAD;
	struct efx_tx_queue *tx_queue = channel->tx_queue;

	efx_ef10_set_tx_queue_csum(tx_queue, txq_type);

	return tx_queue;
}

#ifdef EFX_USE_OVERLAY_TX_CSUM
unsigned int efx_ef10_select_tx_queue_type(struct sk_buff *skb)
{
	unsigned int txq_type = EFX_TXQ_TYPE_NO_OFFLOAD;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return txq_type;

	if (skb->encapsulation &&
	    skb_checksum_start_offset(skb) ==
	      skb_inner_transport_offset(skb)) {
		/* we only advertise features for IPv4 and IPv6 checksums on
		 * encapsulated packets, so if the checksum is for the inner
		 * packet, it must be one of them; no further checking required.
		 */
		txq_type = EFX_TXQ_TYPE_INNER_CSUM_OFFLOAD;

#if !defined (EFX_USE_KCOMPAT) || defined (EFX_HAVE_GSO_UDP_TUNNEL_CSUM)
		/* Do we also need to offload the outer header checksum? */
		if (skb_shinfo(skb)->gso_segs > 1 &&
#if !defined (EFX_USE_KCOMPAT) || defined (EFX_HAVE_GSO_PARTIAL)
		    !(skb_shinfo(skb)->gso_type & SKB_GSO_PARTIAL) &&
#endif
		    (skb_shinfo(skb)->gso_type & SKB_GSO_UDP_TUNNEL_CSUM))
			txq_type |= EFX_TXQ_TYPE_CSUM_OFFLOAD;
#endif
		return txq_type;
	}

	/* similarly, we only advertise features for IPv4 and IPv6 checksums,
	 * so it must be one of them. No need for further checks.
	 */
	return EFX_TXQ_TYPE_CSUM_OFFLOAD;
}

#ifdef EFX_NOT_UPSTREAM
static struct efx_tx_queue *
efx_ef10_select_tx_queue_non_csum_overlay(struct efx_channel *channel,
					  struct sk_buff *skb)
{
	unsigned int txq_type = efx_ef10_select_tx_queue_type(skb);

	return &channel->tx_queue[txq_type];
}
#endif

static struct efx_tx_queue *
efx_ef10_select_tx_queue_overlay(struct efx_channel *channel,
				 struct sk_buff *skb)
{
	unsigned int txq_type = efx_ef10_select_tx_queue_type(skb);
	/* map no offload and overlay offload to second queue, which
	 * will be reconfigured with option descriptors if needed.
	 */
	static const unsigned int txq_map[] = {
		[EFX_TXQ_TYPE_CSUM_OFFLOAD] = EFX_TXQ_TYPE_CSUM_OFFLOAD,
		[EFX_TXQ_TYPE_NO_OFFLOAD] = EFX_TXQ_TYPE_NO_OFFLOAD,
		[EFX_TXQ_TYPE_INNER_CSUM_OFFLOAD] = EFX_TXQ_TYPE_NO_OFFLOAD,
		[EFX_TXQ_TYPE_BOTH_CSUM_OFFLOAD] = EFX_TXQ_TYPE_NO_OFFLOAD,
	};
	struct efx_tx_queue *tx_queue = &channel->tx_queue[txq_map[txq_type]];

	efx_ef10_set_tx_queue_csum(tx_queue, txq_type);

	return tx_queue;
}
#endif

static int efx_ef10_init_nic(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	u32 old_datapath_caps = nic_data->datapath_caps;
#if !defined(EFX_USE_KCOMPAT) || (defined(EFX_HAVE_HW_ENC_FEATURES) && \
				  defined(EFX_HAVE_NDO_FEATURES_CHECK))
#ifdef EFX_USE_OVERLAY_TX_CSUM
	netdev_features_t hw_enc_features;
#endif
#endif
	int rc;

	if (nic_data->must_check_datapath_caps) {
		rc = efx_ef10_init_datapath_caps(efx);
		if (rc)
			return rc;
		nic_data->must_check_datapath_caps = false;
	}

	if (nic_data->datapath_caps != old_datapath_caps) {
		down_write(&efx->filter_sem);
		if (efx->filter_state)
			efx_ef10_filter_table_probe_supported_filters(efx,
							efx->filter_state);
		up_write(&efx->filter_sem);
	}

	if (nic_data->must_realloc_vis) {
		/* We cannot let the number of VIs change now */
		rc = efx_ef10_alloc_vis(efx, nic_data->n_allocated_vis,
					nic_data->n_allocated_vis);
		if (rc)
			return rc;
		nic_data->must_realloc_vis = false;
	}

	if (nic_data->must_restore_piobufs && nic_data->n_piobufs) {
		rc = efx_ef10_alloc_piobufs(efx, nic_data->n_piobufs);
		if (rc == 0) {
			rc = efx_ef10_link_piobufs(efx);
			if (rc)
				efx_ef10_free_piobufs(efx);
		}

		/* Log an error on failure, but this is non-fatal.
		 * Permission errors are less important - we've presumably
		 * had the PIO buffer licence removed.
		 */
		if (rc == -EPERM)
			netif_dbg(efx, drv, efx->net_dev,
				  "not permitted to restore PIO buffers\n");
		else if (rc)
			netif_err(efx, drv, efx->net_dev,
				  "failed to restore PIO buffers (%d)\n", rc);
		nic_data->must_restore_piobufs = false;
	}

	/* don't fail init if RSS setup doesn't work */
	efx->type->rx_push_rss_config(efx, false, efx->rx_indir_table, NULL);

#if !defined(EFX_USE_KCOMPAT) || (defined(EFX_HAVE_HW_ENC_FEATURES) && \
				  defined(EFX_HAVE_NDO_FEATURES_CHECK))
#ifdef EFX_USE_OVERLAY_TX_CSUM
	hw_enc_features = 0;

	/* add encapsulated checksum offload features */
	if (nic_data->datapath_caps &
	    (1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN) &&
	    !efx_ef10_is_vf(efx))
		hw_enc_features |= NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
#ifdef EFX_CAN_SUPPORT_ENCAP_TSO
	/* add encapsulated TSO features */
	if (nic_data->datapath_caps2 &
		(1 << MC_CMD_GET_CAPABILITIES_V2_OUT_TX_TSO_V2_ENCAP_LBN)) {
		netdev_features_t encap_tso_features;

		encap_tso_features = NETIF_F_GSO_UDP_TUNNEL | NETIF_F_GSO_GRE |
			NETIF_F_GSO_UDP_TUNNEL_CSUM | NETIF_F_GSO_GRE_CSUM;

		hw_enc_features |= encap_tso_features | NETIF_F_TSO;
		efx->net_dev->features |= encap_tso_features;
	}
#endif

	efx->net_dev->hw_enc_features = hw_enc_features;
#endif
#endif

	return 0;
}

static void efx_ef10_reset_mc_allocations(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
#ifdef CONFIG_SFC_SRIOV
	unsigned int i;
#endif

	/* All our allocations have been reset */
	nic_data->must_realloc_vis = true;
	nic_data->must_restore_filters = true;
	nic_data->must_restore_piobufs = true;
	efx_ef10_forget_old_piobufs(efx);
	nic_data->rx_rss_context = EFX_EF10_RSS_CONTEXT_INVALID;

	/* Driver-created vswitches and vports must be re-created */
	nic_data->must_probe_vswitching = true;
	nic_data->vport_id = EVB_PORT_ID_ASSIGNED;
#ifdef CONFIG_SFC_SRIOV
	if (nic_data->vf)
		for (i = 0; i < efx->vf_count; i++)
			nic_data->vf[i].vport_id = 0;
#endif
	/* Reset the vport_id passed to driverlink clients in case vswitching
	 * is not re-created (due to a config change) after MC reboot.
	 */
	efx->ef10_resources.vport_id = EVB_PORT_ID_ASSIGNED;
}

static enum reset_type efx_ef10_map_reset_reason(enum reset_type reason)
{
	if (reason == RESET_TYPE_MC_FAILURE)
		return RESET_TYPE_DATAPATH;

	return efx_mcdi_map_reset_reason(reason);
}

static int efx_ef10_map_reset_flags(u32 *flags)
{
	enum {
		EF10_RESET_PORT = ((ETH_RESET_MAC | ETH_RESET_PHY) <<
				   ETH_RESET_SHARED_SHIFT),
		EF10_RESET_MC = ((ETH_RESET_DMA | ETH_RESET_FILTER |
				  ETH_RESET_OFFLOAD | ETH_RESET_MAC |
				  ETH_RESET_PHY | ETH_RESET_MGMT) <<
				 ETH_RESET_SHARED_SHIFT)
	};

	/* We assume for now that our PCI function is permitted to
	 * reset everything.
	 */

	if ((*flags & EF10_RESET_MC) == EF10_RESET_MC) {
		*flags &= ~EF10_RESET_MC;
		return RESET_TYPE_WORLD;
	}

	if ((*flags & EF10_RESET_PORT) == EF10_RESET_PORT) {
		*flags &= ~EF10_RESET_PORT;
		return RESET_TYPE_ALL;
	}

	/* no invisible reset implemented */

	return -EINVAL;
}

static int efx_ef10_reset(struct efx_nic *efx, enum reset_type reset_type)
{
	int rc = efx_mcdi_reset(efx, reset_type);

	efx->last_reset = jiffies;

	/* Unprivileged functions return -EPERM, but need to return success
	 * here so that the datapath is brought back up.
	 */
	if (reset_type == RESET_TYPE_WORLD && rc == -EPERM)
		rc = 0;

	/* If it was a port reset, trigger reallocation of MC resources.
	 * Note that on an MC reset nothing needs to be done now because we'll
	 * detect the MC reset later and handle it then.
	 * For an FLR, we never get an MC reset event, but the MC has reset all
	 * resources assigned to us, so we have to trigger reallocation now.
	 */
	if ((reset_type == RESET_TYPE_ALL ||
	     reset_type == RESET_TYPE_MCDI_TIMEOUT) && !rc)
		efx_ef10_reset_mc_allocations(efx);
	return rc;
}

static bool efx_ef10_hw_unavailable(struct efx_nic *efx)
{
	if ((efx->state == STATE_DISABLED) || (efx->state == STATE_RECOVERY))
		return true;

	if (_efx_readd(efx, ER_DZ_BIU_MC_SFT_STATUS) == 0xffffffff) {
		netif_err(efx, hw, efx->net_dev, "Hardware unavailable\n");
		efx->state = STATE_DISABLED;
		return true;
	}

	return false;
}

#ifdef EFX_NOT_UPSTREAM
static int efx_ef10_rx_defer_ring_rx_doorbell(struct efx_rx_queue *rx_queue)
{
	struct efx_channel *channel = efx_rx_queue_channel(rx_queue);
	MCDI_DECLARE_BUF(inbuf, MC_CMD_DRIVER_EVENT_IN_LEN);
	efx_qword_t event;
	size_t outlen;

	EFX_POPULATE_QWORD_2(event,
			     ESF_DZ_EV_CODE, EFX_EF10_DRVGEN_EV,
			     ESF_DZ_EV_DATA, EFX_EF10_RERING_RX_DOORBELL);

	MCDI_SET_DWORD(inbuf, DRIVER_EVENT_IN_EVQ, channel->channel);

	/* MCDI_SET_QWORD is not appropriate here since EFX_POPULATE_* has
	 * already swapped the data to little-endian order.
	 */
	memcpy(MCDI_PTR(inbuf, DRIVER_EVENT_IN_DATA), &event.u64[0],
	       sizeof(efx_qword_t));

	return efx_mcdi_rpc(channel->efx, MC_CMD_DRIVER_EVENT,
			    inbuf, sizeof(inbuf), NULL, 0, &outlen);
}
#endif

static void efx_ef10_monitor(struct efx_nic *efx)
{
#if defined(EFX_NOT_UPSTREAM)
	if (monitor_hw_available && efx_ef10_hw_unavailable(efx)) {
		if (efx->link_state.up) {
			efx->link_state.up = false;
			efx_link_status_changed(efx);
		}
	}
#endif
#if defined(CONFIG_EEH)
	{
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_EEH_DEV_CHECK_FAILURE)
	struct eeh_dev *eehdev =
		of_node_to_eeh_dev(pci_device_to_OF_node(efx->pci_dev));

	eeh_dev_check_failure(eehdev);
#else
	struct pci_dev *pcidev = efx->pci_dev;
	struct device_node *dn = pci_device_to_OF_node(pcidev);

	eeh_dn_check_failure(dn, pcidev);
#endif
	}
#endif

#ifdef EFX_NOT_UPSTREAM
	if (EFX_WORKAROUND_59975(efx)) {
		/* re-ring the RX doorbell. Should be harmless */
		struct efx_channel *channel;
		efx_for_each_channel(channel, efx) {
			struct efx_rx_queue *rxq;
			efx_for_each_channel_rx_queue(rxq, channel) {
				if (rxq->removed_count == 0) {
					efx_ef10_rx_defer_ring_rx_doorbell(rxq);
				}
			}
		}
	}
#endif
}

#define EF10_DMA_STAT(ext_name, mcdi_name)			\
	[EF10_STAT_ ## ext_name] =				\
	{ #ext_name, 64, 8 * MC_CMD_MAC_ ## mcdi_name }
#define EF10_DMA_INVIS_STAT(int_name, mcdi_name)		\
	[EF10_STAT_ ## int_name] =				\
	{ NULL, 64, 8 * MC_CMD_MAC_ ## mcdi_name }
#define EF10_OTHER_STAT(ext_name)				\
	[EF10_STAT_ ## ext_name] = { #ext_name, 0, 0 }
#define GENERIC_SW_STAT(ext_name)				\
	[GENERIC_STAT_ ## ext_name] = { #ext_name, 0, 0 }

static const struct efx_hw_stat_desc efx_ef10_stat_desc[EF10_STAT_COUNT] = {
	EF10_DMA_STAT(port_tx_bytes, TX_BYTES),
	EF10_DMA_STAT(port_tx_packets, TX_PKTS),
	EF10_DMA_STAT(port_tx_pause, TX_PAUSE_PKTS),
	EF10_DMA_STAT(port_tx_control, TX_CONTROL_PKTS),
	EF10_DMA_STAT(port_tx_unicast, TX_UNICAST_PKTS),
	EF10_DMA_STAT(port_tx_multicast, TX_MULTICAST_PKTS),
	EF10_DMA_STAT(port_tx_broadcast, TX_BROADCAST_PKTS),
	EF10_DMA_STAT(port_tx_lt64, TX_LT64_PKTS),
	EF10_DMA_STAT(port_tx_64, TX_64_PKTS),
	EF10_DMA_STAT(port_tx_65_to_127, TX_65_TO_127_PKTS),
	EF10_DMA_STAT(port_tx_128_to_255, TX_128_TO_255_PKTS),
	EF10_DMA_STAT(port_tx_256_to_511, TX_256_TO_511_PKTS),
	EF10_DMA_STAT(port_tx_512_to_1023, TX_512_TO_1023_PKTS),
	EF10_DMA_STAT(port_tx_1024_to_15xx, TX_1024_TO_15XX_PKTS),
	EF10_DMA_STAT(port_tx_15xx_to_jumbo, TX_15XX_TO_JUMBO_PKTS),
	EF10_DMA_STAT(port_rx_bytes, RX_BYTES),
	EF10_DMA_INVIS_STAT(port_rx_bytes_minus_good_bytes, RX_BAD_BYTES),
	EF10_OTHER_STAT(port_rx_good_bytes),
	EF10_OTHER_STAT(port_rx_bad_bytes),
	EF10_DMA_STAT(port_rx_packets, RX_PKTS),
	EF10_DMA_STAT(port_rx_good, RX_GOOD_PKTS),
	EF10_DMA_STAT(port_rx_bad, RX_BAD_FCS_PKTS),
	EF10_DMA_STAT(port_rx_pause, RX_PAUSE_PKTS),
	EF10_DMA_STAT(port_rx_control, RX_CONTROL_PKTS),
	EF10_DMA_STAT(port_rx_unicast, RX_UNICAST_PKTS),
	EF10_DMA_STAT(port_rx_multicast, RX_MULTICAST_PKTS),
	EF10_DMA_STAT(port_rx_broadcast, RX_BROADCAST_PKTS),
	EF10_DMA_STAT(port_rx_lt64, RX_UNDERSIZE_PKTS),
	EF10_DMA_STAT(port_rx_64, RX_64_PKTS),
	EF10_DMA_STAT(port_rx_65_to_127, RX_65_TO_127_PKTS),
	EF10_DMA_STAT(port_rx_128_to_255, RX_128_TO_255_PKTS),
	EF10_DMA_STAT(port_rx_256_to_511, RX_256_TO_511_PKTS),
	EF10_DMA_STAT(port_rx_512_to_1023, RX_512_TO_1023_PKTS),
	EF10_DMA_STAT(port_rx_1024_to_15xx, RX_1024_TO_15XX_PKTS),
	EF10_DMA_STAT(port_rx_15xx_to_jumbo, RX_15XX_TO_JUMBO_PKTS),
	EF10_DMA_STAT(port_rx_gtjumbo, RX_GTJUMBO_PKTS),
	EF10_DMA_STAT(port_rx_bad_gtjumbo, RX_JABBER_PKTS),
	EF10_DMA_STAT(port_rx_overflow, RX_OVERFLOW_PKTS),
	EF10_DMA_STAT(port_rx_align_error, RX_ALIGN_ERROR_PKTS),
	EF10_DMA_STAT(port_rx_length_error, RX_LENGTH_ERROR_PKTS),
	EF10_DMA_STAT(port_rx_nodesc_drops, RX_NODESC_DROPS),
	GENERIC_SW_STAT(rx_nodesc_trunc),
	GENERIC_SW_STAT(rx_noskb_drops),
	EF10_DMA_STAT(port_rx_pm_trunc_bb_overflow, PM_TRUNC_BB_OVERFLOW),
	EF10_DMA_STAT(port_rx_pm_discard_bb_overflow, PM_DISCARD_BB_OVERFLOW),
	EF10_DMA_STAT(port_rx_pm_trunc_vfifo_full, PM_TRUNC_VFIFO_FULL),
	EF10_DMA_STAT(port_rx_pm_discard_vfifo_full, PM_DISCARD_VFIFO_FULL),
	EF10_DMA_STAT(port_rx_pm_trunc_qbb, PM_TRUNC_QBB),
	EF10_DMA_STAT(port_rx_pm_discard_qbb, PM_DISCARD_QBB),
	EF10_DMA_STAT(port_rx_pm_discard_mapping, PM_DISCARD_MAPPING),
	EF10_DMA_STAT(port_rx_dp_q_disabled_packets, RXDP_Q_DISABLED_PKTS),
	EF10_DMA_STAT(port_rx_dp_di_dropped_packets, RXDP_DI_DROPPED_PKTS),
	EF10_DMA_STAT(port_rx_dp_streaming_packets, RXDP_STREAMING_PKTS),
	EF10_DMA_STAT(port_rx_dp_hlb_fetch, RXDP_HLB_FETCH_CONDITIONS),
	EF10_DMA_STAT(port_rx_dp_hlb_wait, RXDP_HLB_WAIT_CONDITIONS),
	EF10_DMA_STAT(rx_unicast, VADAPTER_RX_UNICAST_PACKETS),
	EF10_DMA_STAT(rx_unicast_bytes, VADAPTER_RX_UNICAST_BYTES),
	EF10_DMA_STAT(rx_multicast, VADAPTER_RX_MULTICAST_PACKETS),
	EF10_DMA_STAT(rx_multicast_bytes, VADAPTER_RX_MULTICAST_BYTES),
	EF10_DMA_STAT(rx_broadcast, VADAPTER_RX_BROADCAST_PACKETS),
	EF10_DMA_STAT(rx_broadcast_bytes, VADAPTER_RX_BROADCAST_BYTES),
	EF10_DMA_STAT(rx_bad, VADAPTER_RX_BAD_PACKETS),
	EF10_DMA_STAT(rx_bad_bytes, VADAPTER_RX_BAD_BYTES),
	EF10_DMA_STAT(rx_overflow, VADAPTER_RX_OVERFLOW),
	EF10_DMA_STAT(tx_unicast, VADAPTER_TX_UNICAST_PACKETS),
	EF10_DMA_STAT(tx_unicast_bytes, VADAPTER_TX_UNICAST_BYTES),
	EF10_DMA_STAT(tx_multicast, VADAPTER_TX_MULTICAST_PACKETS),
	EF10_DMA_STAT(tx_multicast_bytes, VADAPTER_TX_MULTICAST_BYTES),
	EF10_DMA_STAT(tx_broadcast, VADAPTER_TX_BROADCAST_PACKETS),
	EF10_DMA_STAT(tx_broadcast_bytes, VADAPTER_TX_BROADCAST_BYTES),
	EF10_DMA_STAT(tx_bad, VADAPTER_TX_BAD_PACKETS),
	EF10_DMA_STAT(tx_bad_bytes, VADAPTER_TX_BAD_BYTES),
	EF10_DMA_STAT(tx_overflow, VADAPTER_TX_OVERFLOW),
};

#define HUNT_COMMON_STAT_MASK ((1ULL << EF10_STAT_port_tx_bytes) |	\
			       (1ULL << EF10_STAT_port_tx_packets) |	\
			       (1ULL << EF10_STAT_port_tx_pause) |	\
			       (1ULL << EF10_STAT_port_tx_unicast) |	\
			       (1ULL << EF10_STAT_port_tx_multicast) |	\
			       (1ULL << EF10_STAT_port_tx_broadcast) |	\
			       (1ULL << EF10_STAT_port_rx_bytes) |	\
			       (1ULL << EF10_STAT_port_rx_bytes_minus_good_bytes) | \
			       (1ULL << EF10_STAT_port_rx_good_bytes) |	\
			       (1ULL << EF10_STAT_port_rx_bad_bytes) |	\
			       (1ULL << EF10_STAT_port_rx_packets) |	\
			       (1ULL << EF10_STAT_port_rx_good) |	\
			       (1ULL << EF10_STAT_port_rx_bad) |	\
			       (1ULL << EF10_STAT_port_rx_pause) |	\
			       (1ULL << EF10_STAT_port_rx_control) |	\
			       (1ULL << EF10_STAT_port_rx_unicast) |	\
			       (1ULL << EF10_STAT_port_rx_multicast) |	\
			       (1ULL << EF10_STAT_port_rx_broadcast) |	\
			       (1ULL << EF10_STAT_port_rx_lt64) |	\
			       (1ULL << EF10_STAT_port_rx_64) |		\
			       (1ULL << EF10_STAT_port_rx_65_to_127) |	\
			       (1ULL << EF10_STAT_port_rx_128_to_255) |	\
			       (1ULL << EF10_STAT_port_rx_256_to_511) |	\
			       (1ULL << EF10_STAT_port_rx_512_to_1023) |\
			       (1ULL << EF10_STAT_port_rx_1024_to_15xx) |\
			       (1ULL << EF10_STAT_port_rx_15xx_to_jumbo) |\
			       (1ULL << EF10_STAT_port_rx_gtjumbo) |	\
			       (1ULL << EF10_STAT_port_rx_bad_gtjumbo) |\
			       (1ULL << EF10_STAT_port_rx_overflow) |	\
			       (1ULL << EF10_STAT_port_rx_nodesc_drops) |\
			       (1ULL << GENERIC_STAT_rx_nodesc_trunc) |	\
			       (1ULL << GENERIC_STAT_rx_noskb_drops))

/* On 7000 series NICs, these statistics are only provided by the 10G MAC.
 * For a 10G/40G switchable port we do not expose these because they might
 * not include all the packets they should.
 * On 8000 series NICs these statistics are always provided.
 */
#define HUNT_10G_ONLY_STAT_MASK ((1ULL << EF10_STAT_port_tx_control) |	\
				 (1ULL << EF10_STAT_port_tx_lt64) |	\
				 (1ULL << EF10_STAT_port_tx_64) |	\
				 (1ULL << EF10_STAT_port_tx_65_to_127) |\
				 (1ULL << EF10_STAT_port_tx_128_to_255) |\
				 (1ULL << EF10_STAT_port_tx_256_to_511) |\
				 (1ULL << EF10_STAT_port_tx_512_to_1023) |\
				 (1ULL << EF10_STAT_port_tx_1024_to_15xx) |\
				 (1ULL << EF10_STAT_port_tx_15xx_to_jumbo))

/* These statistics are only provided by the 40G MAC.  For a 10G/40G
 * switchable port we do expose these because the errors will otherwise
 * be silent.
 */
#define HUNT_40G_EXTRA_STAT_MASK ((1ULL << EF10_STAT_port_rx_align_error) |\
				  (1ULL << EF10_STAT_port_rx_length_error))

/* These statistics are only provided if the firmware supports the
 * capability PM_AND_RXDP_COUNTERS.
 */
#define HUNT_PM_AND_RXDP_STAT_MASK (					\
	(1ULL << EF10_STAT_port_rx_pm_trunc_bb_overflow) |		\
	(1ULL << EF10_STAT_port_rx_pm_discard_bb_overflow) |		\
	(1ULL << EF10_STAT_port_rx_pm_trunc_vfifo_full) |		\
	(1ULL << EF10_STAT_port_rx_pm_discard_vfifo_full) |		\
	(1ULL << EF10_STAT_port_rx_pm_trunc_qbb) |			\
	(1ULL << EF10_STAT_port_rx_pm_discard_qbb) |			\
	(1ULL << EF10_STAT_port_rx_pm_discard_mapping) |		\
	(1ULL << EF10_STAT_port_rx_dp_q_disabled_packets) |		\
	(1ULL << EF10_STAT_port_rx_dp_di_dropped_packets) |		\
	(1ULL << EF10_STAT_port_rx_dp_streaming_packets) |		\
	(1ULL << EF10_STAT_port_rx_dp_hlb_fetch) |			\
	(1ULL << EF10_STAT_port_rx_dp_hlb_wait))

static u64 efx_ef10_raw_stat_mask(struct efx_nic *efx)
{
	u64 raw_mask = HUNT_COMMON_STAT_MASK;
	u32 port_caps = efx_mcdi_phy_get_caps(efx);
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	if (!(efx->mcdi->fn_flags &
	      1 << MC_CMD_DRV_ATTACH_EXT_OUT_FLAG_LINKCTRL))
		return 0;

	if (port_caps & (1 << MC_CMD_PHY_CAP_40000FDX_LBN)) {
		raw_mask |= HUNT_40G_EXTRA_STAT_MASK;
		/* 8000 series have everything even at 40G */
		if (nic_data->datapath_caps2 &
		    (1 << MC_CMD_GET_CAPABILITIES_V2_OUT_MAC_STATS_40G_TX_SIZE_BINS_LBN))
			raw_mask |= HUNT_10G_ONLY_STAT_MASK;
	} else {
		raw_mask |= HUNT_10G_ONLY_STAT_MASK;
	}

	if (nic_data->datapath_caps &
	    (1 << MC_CMD_GET_CAPABILITIES_OUT_PM_AND_RXDP_COUNTERS_LBN))
		raw_mask |= HUNT_PM_AND_RXDP_STAT_MASK;

	return raw_mask;
}

static void efx_ef10_get_stat_mask(struct efx_nic *efx, unsigned long *mask)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	u64 raw_mask[2];

	raw_mask[0] = efx_ef10_raw_stat_mask(efx);

	/* Only show vadaptor stats when EVB capability is present */
	if (nic_data->datapath_caps &
	    (1 << MC_CMD_GET_CAPABILITIES_OUT_EVB_LBN)) {
		raw_mask[0] |= ~((1ULL << EF10_STAT_rx_unicast) - 1);
		raw_mask[1] = (1ULL << (EF10_STAT_COUNT - 63)) - 1;
	} else {
		raw_mask[1] = 0;
	}

#if BITS_PER_LONG == 64
	BUILD_BUG_ON(BITS_TO_LONGS(EF10_STAT_COUNT) != 2);
	mask[0] = raw_mask[0];
	mask[1] = raw_mask[1];
#else
	BUILD_BUG_ON(BITS_TO_LONGS(EF10_STAT_COUNT) != 3);
	mask[0] = raw_mask[0] & 0xffffffff;
	mask[1] = raw_mask[0] >> 32;
	mask[2] = raw_mask[1] & 0xffffffff;
#endif
}

static size_t efx_ef10_describe_stats(struct efx_nic *efx, u8 *names)
{
	DECLARE_BITMAP(mask, EF10_STAT_COUNT);

	efx_ef10_get_stat_mask(efx, mask);
	return efx_nic_describe_stats(efx_ef10_stat_desc, EF10_STAT_COUNT,
				      mask, names);
}

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_USE_NETDEV_STATS64)
static size_t efx_ef10_update_stats_common(struct efx_nic *efx, u64 *full_stats,
					   struct rtnl_link_stats64 *core_stats)
#else
static size_t efx_ef10_update_stats_common(struct efx_nic *efx, u64 *full_stats,
					   struct net_device_stats *core_stats)
#endif
{
	DECLARE_BITMAP(mask, EF10_STAT_COUNT);
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	u64 *stats = nic_data->stats;
	size_t stats_count = 0, index;

	efx_ef10_get_stat_mask(efx, mask);

	if (full_stats) {
		for_each_set_bit(index, mask, EF10_STAT_COUNT) {
			if (efx_ef10_stat_desc[index].name) {
				*full_stats++ = stats[index];
				++stats_count;
			}
		}
	}

	if (!core_stats)
		return stats_count;

	if (nic_data->datapath_caps &
			1 << MC_CMD_GET_CAPABILITIES_OUT_EVB_LBN) {
		/* Use vadaptor stats. */
		core_stats->rx_packets = stats[EF10_STAT_rx_unicast] +
					 stats[EF10_STAT_rx_multicast] +
					 stats[EF10_STAT_rx_broadcast];
		core_stats->tx_packets = stats[EF10_STAT_tx_unicast] +
					 stats[EF10_STAT_tx_multicast] +
					 stats[EF10_STAT_tx_broadcast];
		core_stats->rx_bytes = stats[EF10_STAT_rx_unicast_bytes] +
				       stats[EF10_STAT_rx_multicast_bytes] +
				       stats[EF10_STAT_rx_broadcast_bytes];
		core_stats->tx_bytes = stats[EF10_STAT_tx_unicast_bytes] +
				       stats[EF10_STAT_tx_multicast_bytes] +
				       stats[EF10_STAT_tx_broadcast_bytes];
		core_stats->rx_dropped = stats[GENERIC_STAT_rx_nodesc_trunc] +
					 stats[GENERIC_STAT_rx_noskb_drops];
		core_stats->multicast = stats[EF10_STAT_rx_multicast];
		core_stats->rx_crc_errors = stats[EF10_STAT_rx_bad];
		core_stats->rx_fifo_errors = stats[EF10_STAT_rx_overflow];
		core_stats->rx_errors = core_stats->rx_crc_errors;
		core_stats->tx_errors = stats[EF10_STAT_tx_bad];
	} else {
		/* Use port stats. */
		core_stats->rx_packets = stats[EF10_STAT_port_rx_packets];
		core_stats->tx_packets = stats[EF10_STAT_port_tx_packets];
		core_stats->rx_bytes = stats[EF10_STAT_port_rx_bytes];
		core_stats->tx_bytes = stats[EF10_STAT_port_tx_bytes];
		core_stats->rx_dropped = stats[EF10_STAT_port_rx_nodesc_drops] +
					 stats[GENERIC_STAT_rx_nodesc_trunc] +
					 stats[GENERIC_STAT_rx_noskb_drops];
		core_stats->multicast = stats[EF10_STAT_port_rx_multicast];
		core_stats->rx_length_errors =
				stats[EF10_STAT_port_rx_gtjumbo] +
				stats[EF10_STAT_port_rx_length_error];
		core_stats->rx_crc_errors = stats[EF10_STAT_port_rx_bad];
		core_stats->rx_frame_errors =
				stats[EF10_STAT_port_rx_align_error];
		core_stats->rx_fifo_errors = stats[EF10_STAT_port_rx_overflow];
		core_stats->rx_errors = (core_stats->rx_length_errors +
					 core_stats->rx_crc_errors +
					 core_stats->rx_frame_errors);
	}

	return stats_count;
}

static int efx_ef10_try_update_nic_stats_pf(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	DECLARE_BITMAP(mask, EF10_STAT_COUNT);
	__le64 generation_start, generation_end;
	u64 *stats = nic_data->stats;
	__le64 *dma_stats;

	efx_ef10_get_stat_mask(efx, mask);

	dma_stats = efx->stats_buffer.addr;

	generation_end = dma_stats[MC_CMD_MAC_GENERATION_END];
	if (generation_end == EFX_MC_STATS_GENERATION_INVALID)
		return 0;
	rmb();
	efx_nic_update_stats(efx_ef10_stat_desc, EF10_STAT_COUNT, mask,
			     stats, efx->stats_buffer.addr, false);
	rmb();
	generation_start = dma_stats[MC_CMD_MAC_GENERATION_START];
	if (generation_end != generation_start)
		return -EAGAIN;

	/* Update derived statistics */
	efx_nic_fix_nodesc_drop_stat(efx,
				     &stats[EF10_STAT_port_rx_nodesc_drops]);
	/* MC Firmware reads RX_BYTES and RX_GOOD_BYTES from the MAC.
	 * It then calculates RX_BAD_BYTES and DMAs it to us with RX_BYTES.
	 * We report these as port_rx_ stats. We are not given RX_GOOD_BYTES.
	 * Here we calculate port_rx_good_bytes.
	 */
	stats[EF10_STAT_port_rx_good_bytes] =
		stats[EF10_STAT_port_rx_bytes] -
		stats[EF10_STAT_port_rx_bytes_minus_good_bytes];

	/* The asynchronous reads used to calculate RX_BAD_BYTES in
	 * MC Firmware are done such that we should not see an increase in
	 * RX_BAD_BYTES when a good packet has arrived. Unfortunately this
	 * does mean that the stat can decrease at times. Here we do not
	 * update the stat unless it has increased or has gone to zero
	 * (In the case of the NIC rebooting).
	 * Please see Bug 33781 for a discussion of why things work this way.
	 */
	efx_update_diff_stat(&stats[EF10_STAT_port_rx_bad_bytes],
			     stats[EF10_STAT_port_rx_bytes_minus_good_bytes]);
	efx_update_sw_stats(efx, stats);
	return 0;
}


#if !defined(EFX_USE_KCOMPAT) || defined(EFX_USE_NETDEV_STATS64)
static size_t efx_ef10_update_stats_pf(struct efx_nic *efx, u64 *full_stats,
				       struct rtnl_link_stats64 *core_stats)
#else
static size_t efx_ef10_update_stats_pf(struct efx_nic *efx, u64 *full_stats,
				       struct net_device_stats *core_stats)
#endif
{
	int retry;

	spin_lock_bh(&efx->stats_lock);

	/* If we're unlucky enough to read statistics during the DMA, wait
	 * up to 10ms for it to finish (typically takes <500us)
	 */
	for (retry = 0; retry < 100; ++retry) {
		if (efx_ef10_try_update_nic_stats_pf(efx) == 0)
			break;
		udelay(100);
	}

	return efx_ef10_update_stats_common(efx, full_stats, core_stats);
}

/* Get vPort statistics.
 * If lock is provided, it is obtained before stats update and held on
 * successful return.
 */
int efx_ef10_vport_get_stats(struct efx_nic *efx, unsigned int vport_id,
			     u64 *stats, spinlock_t *lock)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_MAC_STATS_IN_LEN);
	DECLARE_BITMAP(mask, EF10_STAT_COUNT);
	__le64 generation_start, generation_end;
	u32 dma_len = MC_CMD_MAC_NSTATS * sizeof(u64);
	struct efx_buffer stats_buf;
	__le64 *dma_stats;
	int rc;

	efx_ef10_get_stat_mask(efx, mask);

	rc = efx_nic_alloc_buffer(efx, &stats_buf, dma_len, GFP_ATOMIC);
	if (rc)
		return rc;

	dma_stats = stats_buf.addr;
	dma_stats[MC_CMD_MAC_GENERATION_END] = EFX_MC_STATS_GENERATION_INVALID;

	MCDI_SET_QWORD(inbuf, MAC_STATS_IN_DMA_ADDR, stats_buf.dma_addr);
	MCDI_POPULATE_DWORD_1(inbuf, MAC_STATS_IN_CMD,
			      MAC_STATS_IN_DMA, 1);
	MCDI_SET_DWORD(inbuf, MAC_STATS_IN_DMA_LEN, dma_len);
	MCDI_SET_DWORD(inbuf, MAC_STATS_IN_PORT_ID, vport_id);

	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_MAC_STATS, inbuf, sizeof(inbuf),
				NULL, 0, NULL);

	if (rc) {
		/* Expect ENOENT if DMA queues have not been set up */
		if (rc != -ENOENT || (vport_id == EVB_PORT_ID_ASSIGNED &&
				      atomic_read(&efx->active_queues)))
			efx_mcdi_display_error(efx, MC_CMD_MAC_STATS,
					       sizeof(inbuf), NULL, 0, rc);
		goto out;
	}

	generation_end = dma_stats[MC_CMD_MAC_GENERATION_END];
	if (generation_end == EFX_MC_STATS_GENERATION_INVALID) {
		WARN_ON_ONCE(1);
		rc = -EAGAIN;
		goto out;
	}
	/* Acquire lock back since stats should be updated under lock */
	if (lock)
		spin_lock_bh(lock);
	else
		rmb();
	efx_nic_update_stats(efx_ef10_stat_desc, EF10_STAT_COUNT, mask,
			     stats, stats_buf.addr, false);
	rmb();
	generation_start = dma_stats[MC_CMD_MAC_GENERATION_START];
	if (generation_end != generation_start) {
		if (lock)
			spin_unlock_bh(lock);
		rc = -EAGAIN;
		goto out;
	}

out:
	efx_nic_free_buffer(efx, &stats_buf);
	return rc;
}

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_USE_NETDEV_STATS64)
static size_t efx_ef10_update_stats_vf(struct efx_nic *efx, u64 *full_stats,
				       struct rtnl_link_stats64 *core_stats)
#else
static size_t efx_ef10_update_stats_vf(struct efx_nic *efx, u64 *full_stats,
				       struct net_device_stats *core_stats)
#endif
{
	/* MCDI is required to update statistics, but it can't be used
	 * here since read dev_base_lock rwlock may be held outside and
	 * EVQ/CPU0 may wait for the write lock with bottom-halves
	 * disabled (i.e. NAPI cannot run to process MCDI completion).
	 */

	/* Serialize access to nic_data->stats */
	spin_lock_bh(&efx->stats_lock);
	/* The lock should be held on return */
	return efx_ef10_update_stats_common(efx, full_stats, core_stats);
}

static void efx_ef10_vf_schedule_stats_work(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_USE_CANCEL_DELAYED_WORK_SYNC)
	schedule_delayed_work(&nic_data->vf_stats_work,
			      msecs_to_jiffies(efx->stats_period_ms));
#else
	spin_lock_bh(&efx->stats_lock);
	if (nic_data->vf_stats_enabled)
		queue_delayed_work(efx_workqueue, &nic_data->vf_stats_work,
				   msecs_to_jiffies(efx->stats_period_ms));
	spin_unlock_bh(&efx->stats_lock);
#endif
}

static void efx_ef10_start_stats_vf(struct efx_nic *efx)
{
#if defined(EFX_USE_KCOMPAT) && !defined(EFX_USE_CANCEL_DELAYED_WORK_SYNC)
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	spin_lock_bh(&efx->stats_lock);
	nic_data->vf_stats_enabled = true;
	spin_unlock_bh(&efx->stats_lock);
#endif
	efx_ef10_vf_schedule_stats_work(efx);
}

static void efx_ef10_stop_stats_vf(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_USE_CANCEL_DELAYED_WORK_SYNC)
	cancel_delayed_work_sync(&nic_data->vf_stats_work);
#else
	spin_lock_bh(&efx->stats_lock);
	nic_data->vf_stats_enabled = false;
	spin_unlock_bh(&efx->stats_lock);
	cancel_delayed_work(&nic_data->vf_stats_work);
	flush_workqueue(efx_workqueue);
#endif
}

static void efx_ef10_pull_stats_vf(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	u64 *stats = nic_data->stats;
	int rc;

	/* It is safe againt deadlock on dev_base_lock rwlock since:
	 *  i) called in MCDI polling mode on probe (i.e. does not
	 *     require EVQ/CPU0 to process MCDI completion)
	 *  ii) called from dedicated async work item which holds no locks
	 */

	rc = efx_ef10_vport_get_stats(efx, EVB_PORT_ID_ASSIGNED,
				      stats, &efx->stats_lock);
	if (rc)
		return;

	/* stats_lock is locked */
	efx_update_sw_stats(efx, stats);
	spin_unlock_bh(&efx->stats_lock);
}

#ifdef CONFIG_SFC_SRIOV
static void efx_ef10_vf_update_stats_work(struct work_struct *data)
{
#if !defined(EFX_USE_KCOMPAT) || !defined(EFX_NEED_WORK_API_WRAPPERS)
	struct efx_ef10_nic_data *nic_data =
		container_of(data, struct efx_ef10_nic_data, vf_stats_work.work);
#else
	struct efx_ef10_nic_data *nic_data =
		container_of(data, struct efx_ef10_nic_data, vf_stats_work);
#endif

	efx_ef10_pull_stats_vf(nic_data->efx);
	efx_ef10_vf_schedule_stats_work(nic_data->efx);
}
#endif

static void efx_ef10_push_irq_moderation(struct efx_channel *channel)
{
	struct efx_nic *efx = channel->efx;
	unsigned int mode, usecs;
	efx_dword_t timer_cmd;

	if (channel->irq_moderation_us) {
		mode = 3;
		usecs = channel->irq_moderation_us;
	} else {
		mode = 0;
		usecs = 0;
	}

	if (EFX_EF10_WORKAROUND_61265(efx)) {
		MCDI_DECLARE_BUF(inbuf, MC_CMD_SET_EVQ_TMR_IN_LEN);
		unsigned int ns = usecs * 1000;

		MCDI_SET_DWORD(inbuf, SET_EVQ_TMR_IN_INSTANCE,
			       channel->channel);
		MCDI_SET_DWORD(inbuf, SET_EVQ_TMR_IN_TMR_LOAD_REQ_NS, ns);
		MCDI_SET_DWORD(inbuf, SET_EVQ_TMR_IN_TMR_RELOAD_REQ_NS, ns);
		MCDI_SET_DWORD(inbuf, SET_EVQ_TMR_IN_TMR_MODE, mode);

		efx_mcdi_rpc_async(efx, MC_CMD_SET_EVQ_TMR,
				   inbuf, sizeof(inbuf), 0, NULL, 0);
	} else if (EFX_EF10_WORKAROUND_35388(efx)) {
		unsigned int ticks = efx_usecs_to_ticks(efx, usecs);

		EFX_POPULATE_DWORD_3(timer_cmd, ERF_DD_EVQ_IND_TIMER_FLAGS,
				     EFE_DD_EVQ_IND_TIMER_FLAGS,
				     ERF_DD_EVQ_IND_TIMER_MODE, mode,
				     ERF_DD_EVQ_IND_TIMER_VAL, ticks);
		efx_writed_page(efx, &timer_cmd, ER_DD_EVQ_INDIRECT,
				channel->channel);
	} else {
		unsigned int ticks = efx_usecs_to_ticks(efx, usecs);

		EFX_POPULATE_DWORD_2(timer_cmd, ERF_DZ_TC_TIMER_MODE, mode,
				     ERF_DZ_TC_TIMER_VAL, ticks);
		efx_writed_page(efx, &timer_cmd, ER_DZ_EVQ_TMR,
				channel->channel);
	}
}

static void efx_ef10_get_wol_vf(struct efx_nic *efx, struct ethtool_wolinfo *wol) {}

static int efx_ef10_set_wol_vf(struct efx_nic *efx, u32 type)
{
	return -EOPNOTSUPP;
}

static void efx_ef10_get_wol(struct efx_nic *efx, struct ethtool_wolinfo *wol)
{
	wol->supported = 0;
	wol->wolopts = 0;
	memset(&wol->sopass, 0, sizeof(wol->sopass));
}

static int efx_ef10_set_wol(struct efx_nic *efx, u32 type)
{
	if (type != 0)
		return -EINVAL;
	return 0;
}

static void efx_ef10_mcdi_request(struct efx_nic *efx,
				  const efx_dword_t *hdr, size_t hdr_len,
				  const efx_dword_t *sdu, size_t sdu_len)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	u8 *pdu = nic_data->mcdi_buf.addr;

	memcpy(pdu, hdr, hdr_len);
	memcpy(pdu + hdr_len, sdu, sdu_len);
	wmb();

	/* The hardware provides 'low' and 'high' (doorbell) registers
	 * for passing the 64-bit address of an MCDI request to
	 * firmware.  However the dwords are swapped by firmware.  The
	 * least significant bits of the doorbell are then 0 for all
	 * MCDI requests due to alignment.
	 */
	_efx_writed(efx, cpu_to_le32((u64)nic_data->mcdi_buf.dma_addr >> 32),
		    ER_DZ_MC_DB_LWRD);
	_efx_writed(efx, cpu_to_le32((u32)nic_data->mcdi_buf.dma_addr),
		    ER_DZ_MC_DB_HWRD);
}

static bool efx_ef10_mcdi_poll_response(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	const efx_dword_t hdr = *(const efx_dword_t *)nic_data->mcdi_buf.addr;

	rmb();
	return EFX_DWORD_FIELD(hdr, MCDI_HEADER_RESPONSE);
}

static void
efx_ef10_mcdi_read_response(struct efx_nic *efx, efx_dword_t *outbuf,
			    size_t offset, size_t outlen)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	const u8 *pdu = nic_data->mcdi_buf.addr;

	memcpy(outbuf, pdu + offset, outlen);
}

static void efx_ef10_mcdi_reboot_detected(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	efx->last_reset = jiffies;

	/* All our allocations have been reset */
	efx_ef10_reset_mc_allocations(efx);

	/* The datapath firmware might have been changed */
	nic_data->must_check_datapath_caps = true;

	/* MAC statistics have been cleared on the NIC; clear the local
	 * statistic that we update with efx_update_diff_stat().
	 */
	nic_data->stats[EF10_STAT_port_rx_bad_bytes] = 0;
}

static int efx_ef10_mcdi_poll_reboot(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	int rc;

	rc = efx_ef10_get_warm_boot_count(efx);
	if (rc < 0) {
		/* The firmware is presumably in the process of
		 * rebooting.  However, we are supposed to report each
		 * reboot just once, so we must only do that once we
		 * can read and store the updated warm boot count.
		 */
		return 0;
	}

	if (rc == nic_data->warm_boot_count)
		return 0;

	nic_data->warm_boot_count = rc;
	efx_ef10_mcdi_reboot_detected(efx);

	return -EIO;
}

/* Handle an MSI interrupt
 *
 * Handle an MSI hardware interrupt.  This routine schedules event
 * queue processing.  No interrupt acknowledgement cycle is necessary.
 * Also, we never need to check that the interrupt is for us, since
 * MSI interrupts cannot be shared.
 */
#if !defined(EFX_USE_KCOMPAT) || !defined(EFX_HAVE_IRQ_HANDLER_REGS)
static irqreturn_t efx_ef10_msi_interrupt(int irq, void *dev_id)
#else
static irqreturn_t efx_ef10_msi_interrupt(int irq, void *dev_id,
					  struct pt_regs *regs
					  __attribute__ ((unused)))
#endif
{
	struct efx_msi_context *context = dev_id;
	struct efx_nic *efx = context->efx;

	netif_vdbg(efx, intr, efx->net_dev,
		   "IRQ %d on CPU %d\n", irq, raw_smp_processor_id());

	if (likely(ACCESS_ONCE(efx->irq_soft_enabled))) {
		/* Note test interrupts */
		if (context->index == efx->irq_level)
			efx->last_irq_cpu = raw_smp_processor_id();

		/* Schedule processing of the channel */
		efx_schedule_channel_irq(efx->channel[context->index]);
	}

	return IRQ_HANDLED;
}

#if !defined(EFX_USE_KCOMPAT) || !defined(EFX_HAVE_IRQ_HANDLER_REGS)
static irqreturn_t efx_ef10_legacy_interrupt(int irq, void *dev_id)
#else
static irqreturn_t efx_ef10_legacy_interrupt(int irq, void *dev_id,
					     struct pt_regs *regs
					     __attribute__ ((unused)))
#endif
{
	struct efx_nic *efx = dev_id;
	bool soft_enabled = ACCESS_ONCE(efx->irq_soft_enabled);
	struct efx_channel *channel;
	efx_dword_t reg;
	u32 queues;

	/* Read the ISR which also ACKs the interrupts */
	efx_readd(efx, &reg, ER_DZ_BIU_INT_ISR);
	queues = EFX_DWORD_FIELD(reg, ERF_DZ_ISR_REG);

	if (queues == 0)
		return IRQ_NONE;

	if (likely(soft_enabled)) {
		/* Note test interrupts */
		if (queues & (1U << efx->irq_level))
			efx->last_irq_cpu = raw_smp_processor_id();

		efx_for_each_channel(channel, efx) {
			if (queues & 1)
				efx_schedule_channel_irq(channel);
			queues >>= 1;
		}
	}

	netif_vdbg(efx, intr, efx->net_dev,
		   "IRQ %d on CPU %d status " EFX_DWORD_FMT "\n",
		   irq, raw_smp_processor_id(), EFX_DWORD_VAL(reg));

	return IRQ_HANDLED;
}

static int efx_ef10_irq_test_generate(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_TRIGGER_INTERRUPT_IN_LEN);

	if (efx_mcdi_set_workaround(efx, MC_CMD_WORKAROUND_BUG41750, true,
				    NULL) == 0)
		return -ENOTSUPP;

	BUILD_BUG_ON(MC_CMD_TRIGGER_INTERRUPT_OUT_LEN != 0);

	MCDI_SET_DWORD(inbuf, TRIGGER_INTERRUPT_IN_INTR_LEVEL, efx->irq_level);
	return efx_mcdi_rpc_quiet(efx, MC_CMD_TRIGGER_INTERRUPT,
				  inbuf, sizeof(inbuf), NULL, 0, NULL);
}

static int efx_ef10_tx_probe(struct efx_tx_queue *tx_queue)
{
	return efx_nic_alloc_buffer(tx_queue->efx, &tx_queue->txd.buf,
				    (tx_queue->ptr_mask + 1) *
				    sizeof(efx_qword_t),
				    GFP_KERNEL);
}

/* This writes to the TX_DESC_WPTR and also pushes data.  If multiple
 * descriptors are queued, subsequent descriptors will then be DMA-fetched.
 */
static inline void efx_ef10_push_tx_desc(struct efx_tx_queue *tx_queue,
					 const efx_qword_t *txd)
{
	unsigned int write_ptr;
	efx_oword_t reg;

	write_ptr = tx_queue->write_count & tx_queue->ptr_mask;
	EFX_POPULATE_OWORD_1(reg, ERF_DZ_TX_DESC_WPTR, write_ptr);
	reg.qword[0] = *txd;
	efx_writeo_page(tx_queue->efx, &reg,
			ER_DZ_TX_DESC_UPD, tx_queue->queue);
	tx_queue->notify_count = tx_queue->write_count;
}

/* Add Firmware-Assisted TSO v2 option descriptors to a queue.
 */
static int efx_ef10_tx_tso_desc(struct efx_tx_queue *tx_queue,
				struct sk_buff *skb,
				bool *data_mapped)
{
	struct efx_tx_buffer *buffer;
	u16 inner_ipv4_id = 0;
	u16 outer_ipv4_id = 0;
	struct tcphdr *tcp;
	struct iphdr *ip;
	u16 ip_tot_len;
	u32 seqnum;
	u32 mss;

	EFX_BUG_ON_PARANOID(tx_queue->tso_version != 2);

	mss = skb_shinfo(skb)->gso_size;

	if (unlikely(mss < 4)) {
		WARN_ONCE(1, "MSS of %u is too small for TSO v2\n", mss);
		return -EINVAL;
	}

#ifdef EFX_NOT_UPSTREAM
	if (efx_skb_encapsulation(skb)) {
#else
	if (skb->encapsulation) {
#endif
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_CAN_SUPPORT_ENCAP_TSO)
		if (!tx_queue->tso_encap)
			return -EINVAL;
		ip = ip_hdr(skb);
		if (ip->version == 4)
			outer_ipv4_id = ntohs(ip->id);

		ip = inner_ip_hdr(skb);
		tcp = inner_tcp_hdr(skb);
#else
		/* Insufficient information in the SKB to support this */
		return -EINVAL;
#endif
	} else {
		ip = ip_hdr(skb);
		tcp = tcp_hdr(skb);
	}

	/* 8000-series EF10 hardware requires that IP Total Length be
	 * greater than or equal to the value it will have in each segment
	 * (which is at most mss + 208 + TCP header length), but also less
	 * than (0x10000 - inner_network_header).  Otherwise the TCP
	 * checksum calculation will be broken for encapsulated packets.
	 * We fill in ip->tot_len with 0xff30, which should satisfy the
	 * first requirement unless the MSS is ridiculously large (which
	 * should be impossible as the driver max MTU is 9216); it is
	 * guaranteed to satisfy the second as we only attempt TSO if
	 * inner_network_header <= 208.
	 */
	ip_tot_len = -EFX_TSO2_MAX_HDRLEN;
	EFX_BUG_ON_PARANOID(mss + EFX_TSO2_MAX_HDRLEN + (tcp->doff << 2u) >
			    ip_tot_len);

	if (ip->version == 4) {
		ip->tot_len = htons(ip_tot_len);
		ip->check = 0;
		inner_ipv4_id = ntohs(ip->id);
	} else {
		((struct ipv6hdr *)ip)->payload_len = htons(ip_tot_len);
	}

	seqnum = ntohl(tcp->seq);

	buffer = efx_tx_queue_get_insert_buffer(tx_queue);

	buffer->flags = EFX_TX_BUF_OPTION;
	buffer->len = 0;
	buffer->unmap_len = 0;
	EFX_POPULATE_QWORD_5(buffer->option,
			ESF_DZ_TX_DESC_IS_OPT, 1,
			ESF_DZ_TX_OPTION_TYPE, ESE_DZ_TX_OPTION_DESC_TSO,
			ESF_DZ_TX_TSO_OPTION_TYPE,
			ESE_DZ_TX_TSO_OPTION_DESC_FATSO2A,
			ESF_DZ_TX_TSO_IP_ID, inner_ipv4_id,
			ESF_DZ_TX_TSO_TCP_SEQNO, seqnum
			);
	++tx_queue->insert_count;

	buffer = efx_tx_queue_get_insert_buffer(tx_queue);

	buffer->flags = EFX_TX_BUF_OPTION;
	buffer->len = 0;
	buffer->unmap_len = 0;
	EFX_POPULATE_QWORD_5(buffer->option,
			ESF_DZ_TX_DESC_IS_OPT, 1,
			ESF_DZ_TX_OPTION_TYPE, ESE_DZ_TX_OPTION_DESC_TSO,
			ESF_DZ_TX_TSO_OPTION_TYPE,
			ESE_DZ_TX_TSO_OPTION_DESC_FATSO2B,
			ESF_DZ_TX_TSO_OUTER_IP_ID, outer_ipv4_id,
			ESF_DZ_TX_TSO_TCP_MSS, mss
			);
	++tx_queue->insert_count;

	return 0;
}

static int efx_ef10_tx_init(struct efx_tx_queue *tx_queue)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_INIT_TXQ_EXT_IN_LEN);
	size_t entries = tx_queue->txd.buf.len / EFX_BUF_SIZE;
	struct efx_channel *channel = tx_queue->channel;
	struct efx_nic *efx = tx_queue->efx;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	bool tso_v2 = false;
	dma_addr_t dma_addr;
	efx_qword_t *txd;
	int rc;
	int i;
	bool outer_csum_offload =
		tx_queue->csum_offload & EFX_TXQ_TYPE_CSUM_OFFLOAD;
	bool inner_csum_offload =
		tx_queue->csum_offload & EFX_TXQ_TYPE_INNER_CSUM_OFFLOAD;

	BUILD_BUG_ON(MC_CMD_INIT_TXQ_OUT_LEN != 0);
	EFX_BUG_ON_PARANOID(inner_csum_offload &&
		!(nic_data->datapath_caps &
			(1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN)));

	/* Only allow TX timestamping if we have the license for it. */
	if (!(nic_data->licensed_features &
	      (1 << LICENSED_V3_FEATURES_TX_TIMESTAMPS_LBN))) {
		tx_queue->timestamping = false;
#ifdef CONFIG_SFC_PTP
		/* Disable sync events on this channel. */
		if (efx->type->ptp_set_ts_sync_events)
			efx->type->ptp_set_ts_sync_events(efx, false, false);
#endif
	}

#ifdef EFX_NOT_UPSTREAM
	/* TSOv2 is a limited resource that can only be configured on a limited
	 * number of queues.  On a queue with no checksum offloads, TSO cannot
	 * possibly be a thing, so we don't enable it there.  However, normally
	 * we use the same queue for NO_OFFLOAD and for INNER_CSUM_OFFLOAD, and
	 * it's initialised as a NO_OFFLOAD.  So it's only when
	 * tx_non_csum_queue is set (meaning we have separate NO_OFFLOAD and
	 * INNER_CSUM_OFFLOAD queues) that the NO_OFFLOAD queue doesn't want a
	 * TSOv2 context.
	 */

	if (!(tx_non_csum_queue &&
	      tx_queue->csum_offload == EFX_TXQ_TYPE_NO_OFFLOAD))
#endif
	/* TSOv2 cannot be used with Hardware timestamping. */
	if (!tx_queue->timestamping &&
	    (nic_data->datapath_caps2 &
	     (1 << MC_CMD_GET_CAPABILITIES_V2_OUT_TX_TSO_V2_LBN))) {
		tso_v2 = true;
		netif_dbg(efx, hw, efx->net_dev, "Using TSOv2 for channel %u\n",
				channel->channel);
	}

	MCDI_SET_DWORD(inbuf, INIT_TXQ_EXT_IN_SIZE, tx_queue->ptr_mask + 1);
	MCDI_SET_DWORD(inbuf, INIT_TXQ_EXT_IN_TARGET_EVQ, channel->channel);
	MCDI_SET_DWORD(inbuf, INIT_TXQ_EXT_IN_LABEL, tx_queue->queue);
	MCDI_SET_DWORD(inbuf, INIT_TXQ_EXT_IN_INSTANCE, tx_queue->queue);
	/* TSOv2 implies IP header checksum offload for TSO frames, so we can
	 * safely disable IP header checksum offload for everything else.  If
	 * we don't have TSOv2, then we have to enable IP header checksum
	 * offload, which is strictly incorrect but better than breaking TSO.
	 */
	MCDI_POPULATE_DWORD_6(inbuf, INIT_TXQ_EXT_IN_FLAGS,
			      INIT_TXQ_EXT_IN_FLAG_TSOV2_EN,
					tso_v2,
			      INIT_TXQ_EXT_IN_FLAG_IP_CSUM_DIS,
					tso_v2 || !outer_csum_offload,
			      INIT_TXQ_EXT_IN_FLAG_TCP_CSUM_DIS,
					!outer_csum_offload,
			      INIT_TXQ_EXT_IN_FLAG_INNER_IP_CSUM_EN,
					inner_csum_offload && !tso_v2,
			      INIT_TXQ_EXT_IN_FLAG_INNER_TCP_CSUM_EN,
					inner_csum_offload,
			      INIT_TXQ_EXT_IN_FLAG_TIMESTAMP,
					tx_queue->timestamping);

	MCDI_SET_DWORD(inbuf, INIT_TXQ_EXT_IN_OWNER_ID, 0);
	MCDI_SET_DWORD(inbuf, INIT_TXQ_EXT_IN_PORT_ID, nic_data->vport_id);

	dma_addr = tx_queue->txd.buf.dma_addr;

	netif_dbg(efx, hw, efx->net_dev, "pushing TXQ %d. %zu entries (%llx)\n",
		  tx_queue->queue, entries, (u64)dma_addr);

	for (i = 0; i < entries; ++i) {
		MCDI_SET_ARRAY_QWORD(inbuf, INIT_TXQ_EXT_IN_DMA_ADDR, i,
				     dma_addr);
		dma_addr += EFX_BUF_SIZE;
	}

	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_INIT_TXQ, inbuf, sizeof(inbuf),
				NULL, 0, NULL);

	if (rc == -ENOSPC && tso_v2) {
		/* We may have run out of TSOv2 contexts - try without. */
		tso_v2 = false;

		MCDI_POPULATE_DWORD_5(inbuf, INIT_TXQ_EXT_IN_FLAGS,
				      INIT_TXQ_EXT_IN_FLAG_IP_CSUM_DIS,
						!outer_csum_offload,
				      INIT_TXQ_EXT_IN_FLAG_TCP_CSUM_DIS,
						!outer_csum_offload,
				      INIT_TXQ_EXT_IN_FLAG_INNER_IP_CSUM_EN,
						inner_csum_offload,
				      INIT_TXQ_EXT_IN_FLAG_INNER_TCP_CSUM_EN,
						inner_csum_offload,
				      INIT_TXQ_EXT_IN_FLAG_TIMESTAMP,
						tx_queue->timestamping);

		rc = efx_mcdi_rpc_quiet(efx, MC_CMD_INIT_TXQ, inbuf,
					sizeof(inbuf), NULL, 0, NULL);
		if (rc == 0)
			netif_warn(efx, probe, efx->net_dev,
				   "TSOv2 context not available to segment in hardware. TCP performance may be reduced.\n");
	}

	if (rc) {
		efx_mcdi_display_error(efx, MC_CMD_INIT_TXQ,
				       MC_CMD_INIT_TXQ_EXT_IN_LEN, NULL, 0, rc);
		goto fail;
	}

	channel->tx_coalesce_doorbell = tx_coalesce_doorbell;

	/* A previous user of this TX queue might have set us up the
	 * bomb by writing a descriptor to the TX push collector but
	 * not the doorbell.  (Each collector belongs to a port, not a
	 * queue or function, so cannot easily be reset.)  We must
	 * attempt to push a no-op descriptor in its place.
	 */
	tx_queue->buffer[0].flags = EFX_TX_BUF_OPTION;
	tx_queue->insert_count = 1;
	tx_queue->notify_count = 0;
	txd = efx_tx_desc(tx_queue, 0);
	EFX_POPULATE_QWORD_7(*txd,
			     ESF_DZ_TX_DESC_IS_OPT, true,
			     ESF_DZ_TX_OPTION_TYPE,
			     ESE_DZ_TX_OPTION_DESC_CRC_CSUM,
			     ESF_DZ_TX_OPTION_UDP_TCP_CSUM,
				outer_csum_offload,
			     ESF_DZ_TX_OPTION_IP_CSUM,
				outer_csum_offload && !tso_v2,
			     ESF_DZ_TX_OPTION_INNER_UDP_TCP_CSUM,
				inner_csum_offload,
			     ESF_DZ_TX_OPTION_INNER_IP_CSUM,
				inner_csum_offload && !tso_v2,
			     ESF_DZ_TX_TIMESTAMP,
				tx_queue->timestamping);
	tx_queue->write_count = 1;

	if (tso_v2) {
		tx_queue->handle_tso = efx_ef10_tx_tso_desc;
		tx_queue->tso_version = 2;

		if (nic_data->datapath_caps2 &
		    (1 << MC_CMD_GET_CAPABILITIES_V2_OUT_TX_TSO_V2_ENCAP_LBN))
			tx_queue->tso_encap = true;
	} else if (nic_data->datapath_caps &
		   (1 << MC_CMD_GET_CAPABILITIES_OUT_TX_TSO_LBN)) {
		tx_queue->tso_version = 1;
	}

	wmb();
	efx_ef10_push_tx_desc(tx_queue, txd);

	return rc;

fail:
	if (rc != -ENETDOWN && rc != -EAGAIN)
		netdev_WARN(efx->net_dev, "failed to initialise TXQ %d\n",
			    tx_queue->queue);

	return rc;
}

static void efx_ef10_tx_fini(struct efx_tx_queue *tx_queue, bool hw_unavailable)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_FINI_TXQ_IN_LEN);
	MCDI_DECLARE_BUF_ERR(outbuf);
	struct efx_nic *efx = tx_queue->efx;
	size_t outlen;
	int rc;

	if (hw_unavailable)
		return;

	MCDI_SET_DWORD(inbuf, FINI_TXQ_IN_INSTANCE,
		       tx_queue->queue);

	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_FINI_TXQ, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), &outlen);

	if (rc && rc != -EALREADY)
		goto fail;

	return;

fail:
	efx_mcdi_display_error(efx, MC_CMD_FINI_TXQ, MC_CMD_FINI_TXQ_IN_LEN,
			       outbuf, outlen, rc);
}

static void efx_ef10_tx_remove(struct efx_tx_queue *tx_queue)
{
	efx_nic_free_buffer(tx_queue->efx, &tx_queue->txd.buf);
}

static void efx_ef10_notify_tx_desc(struct efx_tx_queue *tx_queue)
{
	unsigned int write_ptr;
	efx_dword_t reg;

	write_ptr = tx_queue->write_count & tx_queue->ptr_mask;
	EFX_POPULATE_DWORD_1(reg, ERF_DZ_TX_DESC_WPTR_DWORD, write_ptr);
	efx_writed_page(tx_queue->efx, &reg,
			ER_DZ_TX_DESC_UPD_DWORD, tx_queue->queue);
	tx_queue->notify_count = tx_queue->write_count;
}

#define EFX_EF10_MAX_TX_DESCRIPTOR_LEN 0x3fff

static unsigned int efx_ef10_tx_limit_len(struct efx_tx_queue *tx_queue,
					  dma_addr_t dma_addr, unsigned int len)
{
	if (len > EFX_EF10_MAX_TX_DESCRIPTOR_LEN) {
		/* If we need to break across multiple descriptors we should
		 * stop at a page boundary. This assumes the length limit is
		 * greater than the page size.
		 */
		dma_addr_t end = dma_addr + EFX_EF10_MAX_TX_DESCRIPTOR_LEN;

		BUILD_BUG_ON(EFX_EF10_MAX_TX_DESCRIPTOR_LEN < EFX_PAGE_SIZE);
		len = (end & (~(EFX_PAGE_SIZE - 1))) - dma_addr;
	}

	return len;
}

static void efx_ef10_tx_write(struct efx_tx_queue *tx_queue)
{
	unsigned int old_write_count = tx_queue->write_count;
	unsigned int new_write_count = old_write_count;
	struct efx_tx_buffer *buffer;
	unsigned int write_ptr;
	efx_qword_t *txd;

	tx_queue->xmit_pending = false;
	if (unlikely(tx_queue->write_count == tx_queue->insert_count))
		return;

	do {
		write_ptr = new_write_count & tx_queue->ptr_mask;
		buffer = &tx_queue->buffer[write_ptr];
		txd = efx_tx_desc(tx_queue, write_ptr);
		++new_write_count;

		/* Create TX descriptor ring entry */
		if (buffer->flags & EFX_TX_BUF_OPTION) {
			*txd = buffer->option;
			if (EFX_QWORD_FIELD(*txd, ESF_DZ_TX_OPTION_TYPE) == 1) {
				/* PIO descriptor */
				tx_queue->packet_write_count = new_write_count;
			}
		} else {
			tx_queue->packet_write_count = new_write_count;
			BUILD_BUG_ON(EFX_TX_BUF_CONT != 1);
			EFX_POPULATE_QWORD_3(
				*txd,
				ESF_DZ_TX_KER_CONT,
				buffer->flags & EFX_TX_BUF_CONT,
				ESF_DZ_TX_KER_BYTE_CNT, buffer->len,
				ESF_DZ_TX_KER_BUF_ADDR, buffer->dma_addr);
		}
	} while (new_write_count != tx_queue->insert_count);

	wmb(); /* Ensure descriptors are written before they are fetched */

	tx_queue->write_count = new_write_count;

	/* Only the first descriptor can be pushed, so don't need to consider
	 * whether multiple descriptors are queued.
	 */
	if (likely(old_write_count - tx_queue->read_count < tx_push_max_fill)) {
		txd = efx_tx_desc(tx_queue,
				  old_write_count & tx_queue->ptr_mask);
		efx_ef10_push_tx_desc(tx_queue, txd);
		++tx_queue->pushes;
	} else {
		/* The write_count above must be updated before reading
		 * channel->holdoff_doorbell to avoid a race with the
		 * completion path, so ensure these operations are not
		 * re-ordered.  This also flushes the update of write_count
		 * back into the cache.
		 */
		smp_mb();

		/* If the completion path is running and module option
		 * to enable coalescing is set we let the completion path
		 * handle the doorbell ping.
		 */
		if (!tx_queue->channel->holdoff_doorbell) {
			/* Completion handler not running so send out */
			efx_ef10_notify_tx_desc(tx_queue);
			++tx_queue->doorbell_notify_tx;
		}
	}
}

#define RSS_MODE_HASH_ADDRS	(1 << RSS_MODE_HASH_SRC_ADDR_LBN |\
				 1 << RSS_MODE_HASH_DST_ADDR_LBN)
#define RSS_MODE_HASH_PORTS	(1 << RSS_MODE_HASH_SRC_PORT_LBN |\
				 1 << RSS_MODE_HASH_DST_PORT_LBN)
#define RSS_CONTEXT_FLAGS_DEFAULT	(1 << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_TOEPLITZ_IPV4_EN_LBN |\
					 1 << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_TOEPLITZ_TCPV4_EN_LBN |\
					 1 << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_TOEPLITZ_IPV6_EN_LBN |\
					 1 << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_TOEPLITZ_TCPV6_EN_LBN |\
					 (RSS_MODE_HASH_ADDRS | RSS_MODE_HASH_PORTS) << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_TCP_IPV4_RSS_MODE_LBN |\
					 RSS_MODE_HASH_ADDRS << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_UDP_IPV4_RSS_MODE_LBN |\
					 RSS_MODE_HASH_ADDRS << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_OTHER_IPV4_RSS_MODE_LBN |\
					 (RSS_MODE_HASH_ADDRS | RSS_MODE_HASH_PORTS) << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_TCP_IPV6_RSS_MODE_LBN |\
					 RSS_MODE_HASH_ADDRS << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_UDP_IPV6_RSS_MODE_LBN |\
					 RSS_MODE_HASH_ADDRS << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_OTHER_IPV6_RSS_MODE_LBN)

static int efx_ef10_get_rss_flags(struct efx_nic *efx, u32 context, u32 *flags)
{
	/* Firmware had a bug (sfc bug 61952) where it would not actually
	 * fill in the flags field in the response to MC_CMD_RSS_CONTEXT_GET_FLAGS.
	 * This meant that it would always contain whatever was previously
	 * in the MCDI buffer.  Fortunately, all firmware versions with
	 * this bug have the same default flags value for a newly-allocated
	 * RSS context, and the only time we want to get the flags is just
	 * after allocating.  Moreover, the response has a 32-bit hole
	 * where the context ID would be in the request, so we can use an
	 * overlength buffer in the request and pre-fill the flags field
	 * with what we believe the default to be.  Thus if the firmware
	 * has the bug, it will leave our pre-filled value in the flags
	 * field of the response, and we will get the right answer.
	 *
	 * However, this does mean that this function should NOT be used if
	 * the RSS context flags might not be their defaults - it is ONLY
	 * reliably correct for a newly-allocated RSS context.
	 */
	MCDI_DECLARE_BUF(inbuf, MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_LEN);
	size_t outlen;
	int rc;

	/* Check we have a hole for the context ID */
	BUILD_BUG_ON(MC_CMD_RSS_CONTEXT_GET_FLAGS_IN_LEN != MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_FLAGS_OFST);
	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_GET_FLAGS_IN_RSS_CONTEXT_ID, context);
	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_GET_FLAGS_OUT_FLAGS,
		       RSS_CONTEXT_FLAGS_DEFAULT);
	rc = efx_mcdi_rpc(efx, MC_CMD_RSS_CONTEXT_GET_FLAGS, inbuf,
			  sizeof(inbuf), outbuf, sizeof(outbuf), &outlen);
	if (rc == 0) {
		if (outlen < MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_LEN)
			rc = -EIO;
		else
			*flags = MCDI_DWORD(outbuf, RSS_CONTEXT_GET_FLAGS_OUT_FLAGS);
	}
	return rc;
}

/* Attempt to enable 4-tuple UDP hashing on the specified RSS context.
 * If we fail, we just leave the RSS context at its default hash settings,
 * which is safe but may slightly reduce performance.
 * Defaults are 4-tuple for TCP and 2-tuple for UDP and other-IP, so we
 * just need to set the UDP ports flags (for both IP versions).
 */
static void efx_ef10_set_rss_flags(struct efx_nic *efx, u32 context)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_RSS_CONTEXT_SET_FLAGS_IN_LEN);
	u32 flags = 0;

	BUILD_BUG_ON(MC_CMD_RSS_CONTEXT_SET_FLAGS_OUT_LEN != 0);

	if (efx_ef10_get_rss_flags(efx, context, &flags) != 0)
		return;
	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_SET_FLAGS_IN_RSS_CONTEXT_ID, context);
	flags |= RSS_MODE_HASH_PORTS << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_UDP_IPV4_RSS_MODE_LBN;
	flags |= RSS_MODE_HASH_PORTS << MC_CMD_RSS_CONTEXT_GET_FLAGS_OUT_UDP_IPV6_RSS_MODE_LBN;
	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_SET_FLAGS_IN_FLAGS, flags);
	if (!efx_mcdi_rpc(efx, MC_CMD_RSS_CONTEXT_SET_FLAGS, inbuf, sizeof(inbuf),
			  NULL, 0, NULL))
		/* Succeeded, so UDP 4-tuple is now enabled */
		efx->rx_hash_udp_4tuple = true;
}

static int efx_ef10_alloc_rss_context(struct efx_nic *efx, u32 *context,
				      bool exclusive,
				      unsigned int *context_size)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	MCDI_DECLARE_BUF(inbuf, MC_CMD_RSS_CONTEXT_ALLOC_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_RSS_CONTEXT_ALLOC_OUT_LEN);
	size_t outlen;
	int rc;
	u32 alloc_type = exclusive ?
				MC_CMD_RSS_CONTEXT_ALLOC_IN_TYPE_EXCLUSIVE :
				MC_CMD_RSS_CONTEXT_ALLOC_IN_TYPE_SHARED;
	unsigned int rss_spread = exclusive ?
				efx->rss_spread :
				min(rounddown_pow_of_two(efx->rss_spread),
				    EFX_EF10_MAX_SHARED_RSS_CONTEXT_SIZE);

	if (!exclusive && rss_spread == 1) {
		*context = EFX_EF10_RSS_CONTEXT_INVALID;
		if (context_size)
			*context_size = 1;
		return 0;
	}

	if (nic_data->datapath_caps &
	      1 << MC_CMD_GET_CAPABILITIES_OUT_RX_RSS_LIMITED_LBN)
		return -EOPNOTSUPP;

	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_ALLOC_IN_UPSTREAM_PORT_ID,
		       nic_data->vport_id);
	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_ALLOC_IN_TYPE, alloc_type);
	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_ALLOC_IN_NUM_QUEUES, rss_spread);

	rc = efx_mcdi_rpc(efx, MC_CMD_RSS_CONTEXT_ALLOC, inbuf, sizeof(inbuf),
		outbuf, sizeof(outbuf), &outlen);
	if (rc != 0)
		return rc;

	if (outlen < MC_CMD_RSS_CONTEXT_ALLOC_OUT_LEN)
		return -EIO;

	*context = MCDI_DWORD(outbuf, RSS_CONTEXT_ALLOC_OUT_RSS_CONTEXT_ID);

	if (context_size)
		*context_size = rss_spread;

	if (nic_data->datapath_caps &
	    1 << MC_CMD_GET_CAPABILITIES_OUT_ADDITIONAL_RSS_MODES_LBN)
		efx_ef10_set_rss_flags(efx, *context);

	return 0;
}

static void efx_ef10_free_rss_context(struct efx_nic *efx, u32 context)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_RSS_CONTEXT_FREE_IN_LEN);
	int rc;

	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_FREE_IN_RSS_CONTEXT_ID,
		       context);

	rc = efx_mcdi_rpc(efx, MC_CMD_RSS_CONTEXT_FREE, inbuf, sizeof(inbuf),
			  NULL, 0, NULL);
}

static int efx_ef10_populate_rss_table(struct efx_nic *efx, u32 context,
				       const u32 *rx_indir_table, const u8 *key)
{
	MCDI_DECLARE_BUF(tablebuf, MC_CMD_RSS_CONTEXT_SET_TABLE_IN_LEN);
	MCDI_DECLARE_BUF(keybuf, MC_CMD_RSS_CONTEXT_SET_KEY_IN_LEN);
	int i, rc;

	MCDI_SET_DWORD(tablebuf, RSS_CONTEXT_SET_TABLE_IN_RSS_CONTEXT_ID,
		       context);
	BUILD_BUG_ON(ARRAY_SIZE(efx->rx_indir_table) !=
		     MC_CMD_RSS_CONTEXT_SET_TABLE_IN_INDIRECTION_TABLE_LEN);

	/* This iterates over the length of efx->rx_indir_table, but copies
	 * bytes from rx_indir_table.  That's because the latter is a pointer
	 * rather than an array, but should have the same length.
	 * The efx->rx_hash_key loop below is similar.
	 */
	for (i = 0; i < ARRAY_SIZE(efx->rx_indir_table); ++i)
		MCDI_PTR(tablebuf,
			 RSS_CONTEXT_SET_TABLE_IN_INDIRECTION_TABLE)[i] =
				(u8) rx_indir_table[i];

	rc = efx_mcdi_rpc(efx, MC_CMD_RSS_CONTEXT_SET_TABLE, tablebuf,
			  sizeof(tablebuf), NULL, 0, NULL);
	if (rc != 0)
		return rc;

	MCDI_SET_DWORD(keybuf, RSS_CONTEXT_SET_KEY_IN_RSS_CONTEXT_ID,
		       context);
	BUILD_BUG_ON(ARRAY_SIZE(efx->rx_hash_key) !=
		     MC_CMD_RSS_CONTEXT_SET_KEY_IN_TOEPLITZ_KEY_LEN);
	for (i = 0; i < ARRAY_SIZE(efx->rx_hash_key); ++i)
		MCDI_PTR(keybuf, RSS_CONTEXT_SET_KEY_IN_TOEPLITZ_KEY)[i] = key[i];

	return efx_mcdi_rpc(efx, MC_CMD_RSS_CONTEXT_SET_KEY, keybuf,
			    sizeof(keybuf), NULL, 0, NULL);
}

static void efx_ef10_rx_free_indir_table(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	if (nic_data->rx_rss_context != EFX_EF10_RSS_CONTEXT_INVALID)
		efx_ef10_free_rss_context(efx, nic_data->rx_rss_context);
	nic_data->rx_rss_context = EFX_EF10_RSS_CONTEXT_INVALID;
}

static int efx_ef10_rx_push_shared_rss_config(struct efx_nic *efx,
					      unsigned int *context_size)
{
	u32 new_rx_rss_context;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	int rc = efx_ef10_alloc_rss_context(efx, &new_rx_rss_context,
					    false, context_size);

	if (rc != 0)
		return rc;

	nic_data->rx_rss_context = new_rx_rss_context;
	nic_data->rx_rss_context_exclusive = false;
	efx_set_default_rx_indir_table(efx);
	return 0;
}

static int efx_ef10_rx_push_exclusive_rss_config(struct efx_nic *efx,
						 const u32 *rx_indir_table,
						 const u8 *key)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	int rc;
	u32 new_rx_rss_context;

	if (nic_data->rx_rss_context == EFX_EF10_RSS_CONTEXT_INVALID ||
	    !nic_data->rx_rss_context_exclusive) {
		rc = efx_ef10_alloc_rss_context(efx, &new_rx_rss_context,
						true, NULL);
		if (rc == -EOPNOTSUPP)
			return rc;
		else if (rc != 0)
			goto fail1;
	} else {
		new_rx_rss_context = nic_data->rx_rss_context;
	}

	rc = efx_ef10_populate_rss_table(efx, new_rx_rss_context,
					 rx_indir_table, key);
	if (rc)
		goto fail2;
	if (nic_data->rx_rss_context != new_rx_rss_context)
		efx_ef10_rx_free_indir_table(efx);
	nic_data->rx_rss_context = new_rx_rss_context;
	nic_data->rx_rss_context_exclusive = true;
	if (rx_indir_table != efx->rx_indir_table)
		memcpy(efx->rx_indir_table, rx_indir_table,
				sizeof(efx->rx_indir_table));
	if (key != efx->rx_hash_key)
		memcpy(efx->rx_hash_key, key, efx->type->rx_hash_key_size);

	return 0;

fail2:
	if (new_rx_rss_context != nic_data->rx_rss_context)
		efx_ef10_free_rss_context(efx, new_rx_rss_context);
fail1:
	netif_err(efx, hw, efx->net_dev, "%s: failed rc=%d\n", __func__, rc);
	return rc;
}

static int efx_ef10_rx_pull_rss_config(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	MCDI_DECLARE_BUF(inbuf, MC_CMD_RSS_CONTEXT_GET_TABLE_IN_LEN);
	MCDI_DECLARE_BUF(tablebuf, MC_CMD_RSS_CONTEXT_GET_TABLE_OUT_LEN);
	MCDI_DECLARE_BUF(keybuf, MC_CMD_RSS_CONTEXT_GET_KEY_OUT_LEN);
	size_t outlen;
	int rc, i;

	BUILD_BUG_ON(MC_CMD_RSS_CONTEXT_GET_TABLE_IN_LEN !=
		     MC_CMD_RSS_CONTEXT_GET_KEY_IN_LEN);

	if (nic_data->rx_rss_context == EFX_EF10_RSS_CONTEXT_INVALID)
		return -ENOENT;

	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_GET_TABLE_IN_RSS_CONTEXT_ID,
		       nic_data->rx_rss_context);
	BUILD_BUG_ON(ARRAY_SIZE(efx->rx_indir_table) !=
		     MC_CMD_RSS_CONTEXT_GET_TABLE_OUT_INDIRECTION_TABLE_LEN);
	rc = efx_mcdi_rpc(efx, MC_CMD_RSS_CONTEXT_GET_TABLE, inbuf, sizeof(inbuf),
			  tablebuf, sizeof(tablebuf), &outlen);
	if (rc != 0)
		return rc;

	if (WARN_ON(outlen != MC_CMD_RSS_CONTEXT_GET_TABLE_OUT_LEN))
		return -EIO;

	for (i = 0; i < ARRAY_SIZE(efx->rx_indir_table); i++)
		efx->rx_indir_table[i] = MCDI_PTR(tablebuf,
				RSS_CONTEXT_GET_TABLE_OUT_INDIRECTION_TABLE)[i];

	MCDI_SET_DWORD(inbuf, RSS_CONTEXT_GET_KEY_IN_RSS_CONTEXT_ID,
		       nic_data->rx_rss_context);
	BUILD_BUG_ON(ARRAY_SIZE(efx->rx_hash_key) !=
		     MC_CMD_RSS_CONTEXT_SET_KEY_IN_TOEPLITZ_KEY_LEN);
	rc = efx_mcdi_rpc(efx, MC_CMD_RSS_CONTEXT_GET_KEY, inbuf, sizeof(inbuf),
			  keybuf, sizeof(keybuf), &outlen);
	if (rc != 0)
		return rc;

	if (WARN_ON(outlen != MC_CMD_RSS_CONTEXT_GET_KEY_OUT_LEN))
		return -EIO;

	for (i = 0; i < ARRAY_SIZE(efx->rx_hash_key); ++i)
		efx->rx_hash_key[i] = MCDI_PTR(
				keybuf, RSS_CONTEXT_GET_KEY_OUT_TOEPLITZ_KEY)[i];

	return 0;
}

static int efx_ef10_pf_rx_push_rss_config(struct efx_nic *efx, bool user,
					  const u32 *rx_indir_table,
					  const u8 *key)
{
	int rc;

	if (efx->rss_spread == 1)
		return 0;

	if (!key)
		key = efx->rx_hash_key;

	rc = efx_ef10_rx_push_exclusive_rss_config(efx, rx_indir_table, key);

	if (rc == -ENOBUFS && !user) {
		unsigned int context_size;
		bool mismatch = false;
		size_t i;

		for (i = 0; i < ARRAY_SIZE(efx->rx_indir_table) && !mismatch;
		     i++)
			mismatch = rx_indir_table[i] !=
				ethtool_rxfh_indir_default(i, efx->rss_spread);

		rc = efx_ef10_rx_push_shared_rss_config(efx, &context_size);
		if (rc == 0) {
			if (context_size != efx->rss_spread)
				netif_warn(efx, probe, efx->net_dev,
					   "Could not allocate an exclusive RSS "
					   "context; allocated a shared one of "
					   "different size. "
					   "Wanted %u, got %u.\n",
					   efx->rss_spread, context_size);
			else if (mismatch)
				netif_warn(efx, probe, efx->net_dev,
					   "Could not allocate an exclusive RSS "
					   "context; allocated a shared one but "
					   "could not apply custom "
					   "indirection.\n");
			else
				netif_info(efx, probe, efx->net_dev,
					   "Could not allocate an exclusive RSS "
					   "context; allocated a shared one.\n");
		}
	}
	return rc;
}

static int efx_ef10_vf_rx_push_rss_config(struct efx_nic *efx, bool user,
					  const u32 *rx_indir_table
					  __attribute__ ((unused)),
					  const u8 *key
					  __attribute__ ((unused)))
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	if (user)
		return -EOPNOTSUPP;
	if (nic_data->rx_rss_context != EFX_EF10_RSS_CONTEXT_INVALID)
		return 0;
	return efx_ef10_rx_push_shared_rss_config(efx, NULL);
}

static int efx_ef10_rx_probe(struct efx_rx_queue *rx_queue)
{
	return efx_nic_alloc_buffer(rx_queue->efx, &rx_queue->rxd.buf,
				    (rx_queue->ptr_mask + 1) *
				    sizeof(efx_qword_t),
				    GFP_KERNEL);
}

#ifdef CONFIG_SFC_DEBUGFS
static void efx_debugfs_read_dev_list(struct seq_file *file,
				      struct efx_ef10_dev_addr *dev_list,
				      size_t list_len)
{
	size_t i;
	static u8 zero[ETH_ALEN];

	for (i = 0; i < list_len; i++)
		if (ether_addr_equal(dev_list[i].addr, zero))
			seq_printf(file, "%02x:%02x:%02x:%02x:%02x:%02x\n",
				   dev_list[i].addr[0], dev_list[i].addr[1],
				   dev_list[i].addr[2], dev_list[i].addr[3],
				   dev_list[i].addr[4], dev_list[i].addr[5]);
}

static int efx_debugfs_read_dev_uc_list(struct seq_file *file, void *data)
{
	struct efx_ef10_filter_table **table = data;

	efx_debugfs_read_dev_list(file, (*table)->dev_uc_list,
				  EFX_EF10_FILTER_DEV_UC_MAX);
	return 0;
}

static int efx_debugfs_read_dev_mc_list(struct seq_file *file, void *data)
{
	struct efx_ef10_filter_table **table = data;

	efx_debugfs_read_dev_list(file, (*table)->dev_mc_list,
				  EFX_EF10_FILTER_DEV_MC_MAX);
	return 0;
}

static void print_filter(char *s, size_t l, struct efx_filter_spec *spec)
{
	int p =	snprintf(s, l, "match=0x%x,pri=%d,flags=0x%x,q=%d",
			 spec->match_flags, spec->priority, spec->flags,
			 spec->dmaq_id);

	if (spec->stack_id)
		p += snprintf(s + p, l - p, ",stack=0x%x",
			      spec->stack_id);
	if (spec->vport_id)
		p += snprintf(s + p, l - p, ",vport=0x%x",
			      spec->vport_id);

	if (spec->flags & EFX_FILTER_FLAG_RX_RSS) {
		if (spec->rss_context == -1)
			p += snprintf(s + p, l - p, ",rss=def");
		else
			p += snprintf(s + p, l - p, ",rss=0x%x",
				      spec->rss_context);
	}

	if (spec->match_flags & EFX_FILTER_MATCH_OUTER_VID)
		p += snprintf(s + p, l - p,
			      ",ovid=%d", spec->outer_vid);
	if (spec->match_flags & EFX_FILTER_MATCH_INNER_VID)
		p += snprintf(s + p, l - p,
			      ",ivid=%d", spec->inner_vid);
	if (spec->match_flags & EFX_FILTER_MATCH_ENCAP_TYPE)
		p += snprintf(s + p, l - p,
			      ",encap=%d", spec->encap_type);
	if (spec->match_flags & EFX_FILTER_MATCH_ENCAP_TNI)
		p += snprintf(s + p, l - p,
			      ",tni=0x%x", spec->tni);
	if (spec->match_flags & EFX_FILTER_MATCH_LOC_MAC)
		p += snprintf(s + p, l - p,
			      ",lmac=%02x:%02x:%02x:%02x:%02x:%02x",
			      spec->loc_mac[0], spec->loc_mac[1],
			      spec->loc_mac[2], spec->loc_mac[3],
			      spec->loc_mac[4], spec->loc_mac[5]);
	if (spec->match_flags & EFX_FILTER_MATCH_REM_MAC)
		p += snprintf(s + p, l - p,
			      ",rmac=%02x:%02x:%02x:%02x:%02x:%02x",
			      spec->rem_mac[0], spec->rem_mac[1],
			      spec->rem_mac[2], spec->rem_mac[3],
			      spec->rem_mac[4], spec->rem_mac[5]);
	if (spec->match_flags & EFX_FILTER_MATCH_OUTER_LOC_MAC)
		p += snprintf(s + p, l - p,
			      ",olmac=%02x:%02x:%02x:%02x:%02x:%02x",
			      spec->outer_loc_mac[0], spec->outer_loc_mac[1],
			      spec->outer_loc_mac[2], spec->outer_loc_mac[3],
			      spec->outer_loc_mac[4], spec->outer_loc_mac[5]);
	if (spec->match_flags & EFX_FILTER_MATCH_ETHER_TYPE)
		p += snprintf(s + p, l - p,
			      ",ether=0x%x", spec->ether_type);
	if (spec->match_flags & EFX_FILTER_MATCH_IP_PROTO)
		p += snprintf(s + p, l - p,
			      ",ippr=0x%x", spec->ip_proto);
	if (spec->match_flags & EFX_FILTER_MATCH_LOC_HOST) {
		if (spec->ether_type == ETH_P_IP)
			p += snprintf(s + p, l - p,
				      ",lip=%d.%d.%d.%d",
				      spec->loc_host[0] & 0xff,
				      (spec->loc_host[0] >> 8) & 0xff,
				      (spec->loc_host[0] >> 16) & 0xff,
				      (spec->loc_host[0] >> 24) & 0xff);
		else if (spec->ether_type == ETH_P_IPV6)
			p += snprintf(s + p, l - p,
				      ",lip=%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
				      spec->loc_host[0] & 0xffff,
				      (spec->loc_host[0] >> 16) & 0xffff,
				      spec->loc_host[1] & 0xffff,
				      (spec->loc_host[1] >> 16) & 0xffff,
				      spec->loc_host[2] & 0xffff,
				      (spec->loc_host[2] >> 16) & 0xffff,
				      spec->loc_host[3] & 0xffff,
				      (spec->loc_host[3] >> 16) & 0xffff);
		else
			p += snprintf(s + p, l - p, ",lip=?");
	}
	if (spec->match_flags & EFX_FILTER_MATCH_REM_HOST) {
		if (spec->ether_type == ETH_P_IP)
			p += snprintf(s + p, l - p,
				      ",rip=%d.%d.%d.%d",
				      spec->rem_host[0] & 0xff,
				      (spec->rem_host[0] >> 8) & 0xff,
				      (spec->rem_host[0] >> 16) & 0xff,
				      (spec->rem_host[0] >> 24) & 0xff);
		else if (spec->ether_type == ETH_P_IPV6)
			p += snprintf(s + p, l - p,
				      ",rip=%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
				      spec->rem_host[0] & 0xffff,
				      (spec->rem_host[0] >> 16) & 0xffff,
				      spec->rem_host[1] & 0xffff,
				      (spec->rem_host[1] >> 16) & 0xffff,
				      spec->rem_host[2] & 0xffff,
				      (spec->rem_host[2] >> 16) & 0xffff,
				      spec->rem_host[3] & 0xffff,
				      (spec->rem_host[3] >> 16) & 0xffff);
		else
			p += snprintf(s + p, l - p, ",rip=?");
	}
	if (spec->match_flags & EFX_FILTER_MATCH_LOC_PORT)
		p += snprintf(s + p, l - p,
			      ",lport=%d", spec->loc_port);
	if (spec->match_flags & EFX_FILTER_MATCH_REM_PORT)
		p += snprintf(s + p, l - p,
			      ",rport=%d", spec->rem_port);
	if (spec->match_flags & EFX_FILTER_MATCH_LOC_MAC_IG)
		p += snprintf(s + p, l - p, ",%s",
			spec->loc_mac[0] ? "mc" : "uc");
}

static int efx_debugfs_read_filter_list(struct seq_file *file, void *data)
{
	struct efx_nic *efx = container_of(data, struct efx_nic, filter_state);
	struct efx_ef10_filter_table *table = efx->filter_state;
	int i;

	/* deliberately don't lock the filter_lock, so that we can
	 * dump the table mid-operation if needed.
	 */
	down_read(&efx->filter_sem);

	for (i = 0; i < HUNT_FILTER_TBL_ROWS; ++i) {
		struct efx_filter_spec *spec =
			efx_ef10_filter_entry_spec(table, i);
		char filter[256];

		if (spec) {
			print_filter(filter, sizeof(filter), spec);

			seq_printf(file, "%d[0x%04llx],0x%x = %s\n",
				i, table->entry[i].handle & 0xffff,
				efx_ef10_filter_entry_flags(table, i),
				filter);
		}
	}

	up_read(&efx->filter_sem);

	return 0;
}

static int efx_debugfs_udp_tunnels(struct seq_file *file, void *data)
{
	struct efx_nic *efx = data;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	char typebuf[8];
	unsigned int i;

	mutex_lock(&nic_data->udp_tunnels_lock);

	for (i = 0; i < ARRAY_SIZE(nic_data->udp_tunnels); ++i) {
		efx_get_udp_tunnel_type_name(nic_data->udp_tunnels[i].type,
					     typebuf, sizeof(typebuf));
		seq_printf(file, "%d (%s)\n",
			   ntohs(nic_data->udp_tunnels[i].port),
			   typebuf);
	}
	mutex_unlock(&nic_data->udp_tunnels_lock);
	return 0;
}

#ifdef EFX_NOT_UPSTREAM
static int efx_debugfs_read_kernel_blocked(struct seq_file *file, void *data)
{
	unsigned int i;
	bool *kernel_blocked = data;

	for (i = 0; i < EFX_DL_FILTER_BLOCK_KERNEL_MAX; i++)
		seq_printf(file, "%d:%d\n", i, kernel_blocked[i]);
	return 0;
}
#endif

static int efx_debugfs_read_netdev_uc_addr(struct seq_file *file, void *data)
{
	struct net_device *net_dev = data;
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_DEVICE_UC)
	struct netdev_hw_addr *uc;
#else
	struct dev_addr_list *uc;
#endif
	unsigned int i = 0;

	netdev_for_each_uc_addr(uc, net_dev) {
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_DEVICE_UC)
		seq_printf(file, "%d - %pM\n", i, uc->addr);
#else
		seq_printf(file, "%d - %pM\n", i, uc->da_addr);
#endif
		i++;
	}
	return 0;
}

static int efx_debugfs_read_netdev_mc_addr(struct seq_file *file, void *data)
{
	struct net_device *net_dev = data;
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_DEVICE_MC)
	struct netdev_hw_addr *mc;
#else
	struct dev_mc_list *mc;
#endif
	unsigned int i = 0;

	netdev_for_each_mc_addr(mc, net_dev) {
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_DEVICE_MC)
		seq_printf(file, "%d - %pM\n", i, mc->addr);
#else
		seq_printf(file, "%d - %pM\n", i, mc->dmi_addr);
#endif
		i++;
	}
	return 0;
}

static int efx_debugfs_read_netdev_uc_count(struct seq_file *file, void *data)
{
	struct net_device *net_dev = data;

	seq_printf(file, "%d\n", netdev_uc_count(net_dev));
	return 0;
}

static int efx_debugfs_read_netdev_mc_count(struct seq_file *file, void *data)
{
	struct net_device *net_dev = data;

	seq_printf(file, "%d\n", netdev_mc_count(net_dev));
	return 0;
}

static int efx_debugfs_read_netdev_flags(struct seq_file *file, void *data)
{
	struct net_device *net_dev = data;

	seq_printf(file, "0x%x promisc=%d\n", net_dev->flags,
		   net_dev->flags & IFF_PROMISC);
	return 0;
}

static int efx_debugfs_read_netdev_dev_addr(struct seq_file *file, void *data)
{
	struct net_device *net_dev = data;

	seq_printf(file, "%pM\n", net_dev->dev_addr);
	return 0;
}

static struct efx_debugfs_parameter filter_debugfs[] = {
	EFX_INT_PARAMETER(struct efx_ef10_filter_table, dev_uc_count),
	EFX_INT_PARAMETER(struct efx_ef10_filter_table, dev_mc_count),
	EFX_NAMED_PARAMETER(dev_uc_list, struct efx_nic, filter_state,
			    void *, efx_debugfs_read_dev_uc_list),
	EFX_NAMED_PARAMETER(dev_mc_list, struct efx_nic, filter_state,
			    void *, efx_debugfs_read_dev_mc_list),
	EFX_NAMED_PARAMETER(filters, struct efx_nic, filter_state,
			    void *, efx_debugfs_read_filter_list),
	_EFX_RAW_PARAMETER(udp_tunnels, efx_debugfs_udp_tunnels),
#ifdef EFX_NOT_UPSTREAM
	_EFX_PARAMETER(struct efx_ef10_filter_table, kernel_blocked,
		       efx_debugfs_read_kernel_blocked),
#endif
	{NULL},
};

static struct efx_debugfs_parameter netdev_debugfs[] = {
	_EFX_RAW_PARAMETER(netdev_uc_addr, efx_debugfs_read_netdev_uc_addr),
	_EFX_RAW_PARAMETER(netdev_mc_addr, efx_debugfs_read_netdev_mc_addr),
	_EFX_RAW_PARAMETER(netdev_uc_count, efx_debugfs_read_netdev_uc_count),
	_EFX_RAW_PARAMETER(netdev_mc_count, efx_debugfs_read_netdev_mc_count),
	_EFX_RAW_PARAMETER(netdev_flags, efx_debugfs_read_netdev_flags),
	_EFX_RAW_PARAMETER(netdev_dev_addr, efx_debugfs_read_netdev_dev_addr),
	{NULL},
};
#endif /* CONFIG_SFC_DEBUGFS */

static int efx_ef10_rx_init(struct efx_rx_queue *rx_queue)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_INIT_RXQ_EXT_IN_LEN);
	struct efx_channel *channel = efx_rx_queue_channel(rx_queue);
	size_t entries = rx_queue->rxd.buf.len / EFX_BUF_SIZE;
	struct efx_nic *efx = rx_queue->efx;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	bool supports_overlays;
	dma_addr_t dma_addr;
	int rc;
	int i;
	BUILD_BUG_ON(MC_CMD_INIT_RXQ_EXT_OUT_LEN != 0);

	rx_queue->scatter_n = 0;
	rx_queue->scatter_len = 0;

	supports_overlays =
		!!(nic_data->datapath_caps &
			(1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN));

	MCDI_SET_DWORD(inbuf, INIT_RXQ_IN_SIZE, rx_queue->ptr_mask + 1);
	MCDI_SET_DWORD(inbuf, INIT_RXQ_IN_TARGET_EVQ, channel->channel);
	MCDI_SET_DWORD(inbuf, INIT_RXQ_IN_LABEL, efx_rx_queue_index(rx_queue));
	MCDI_SET_DWORD(inbuf, INIT_RXQ_IN_INSTANCE,
		       efx_rx_queue_index(rx_queue));
	/* Outer classes (ETH_TAG_CLASS to be more precise) are required
	 * for Rx VLAN stripping offload.
	 */
	MCDI_POPULATE_DWORD_3(inbuf, INIT_RXQ_IN_FLAGS,
			      INIT_RXQ_IN_FLAG_PREFIX, 1,
			      INIT_RXQ_IN_FLAG_TIMESTAMP, 1,
			      INIT_RXQ_EXT_IN_FLAG_WANT_OUTER_CLASSES,
			      supports_overlays);
	MCDI_SET_DWORD(inbuf, INIT_RXQ_IN_OWNER_ID, 0);
	MCDI_SET_DWORD(inbuf, INIT_RXQ_IN_PORT_ID, nic_data->vport_id);

	dma_addr = rx_queue->rxd.buf.dma_addr;

	netif_dbg(efx, hw, efx->net_dev, "pushing RXQ %d. %zu entries (%llx)\n",
		  efx_rx_queue_index(rx_queue), entries, (u64)dma_addr);

	for (i = 0; i < entries; ++i) {
		MCDI_SET_ARRAY_QWORD(inbuf, INIT_RXQ_IN_DMA_ADDR, i, dma_addr);
		dma_addr += EFX_BUF_SIZE;
	}

	rc = efx_mcdi_rpc(efx, MC_CMD_INIT_RXQ, inbuf,
			  MC_CMD_INIT_RXQ_EXT_IN_LEN, NULL, 0, NULL);
	if (rc && rc != -ENETDOWN && rc != -EAGAIN)
		netdev_WARN(efx->net_dev, "failed to initialise RXQ %d\n",
			    efx_rx_queue_index(rx_queue));
	return rc;
}



static void efx_ef10_rx_fini(struct efx_rx_queue *rx_queue, bool hw_unavailable)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_FINI_RXQ_IN_LEN);
	MCDI_DECLARE_BUF_ERR(outbuf);
	struct efx_nic *efx = rx_queue->efx;
	size_t outlen;
	int rc;

	if (hw_unavailable)
		return;

	MCDI_SET_DWORD(inbuf, FINI_RXQ_IN_INSTANCE,
		       efx_rx_queue_index(rx_queue));

	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_FINI_RXQ, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), &outlen);

	if (rc && rc != -EALREADY)
		goto fail;

	return;

fail:
	efx_mcdi_display_error(efx, MC_CMD_FINI_RXQ, MC_CMD_FINI_RXQ_IN_LEN,
			       outbuf, outlen, rc);
}

static void efx_ef10_rx_remove(struct efx_rx_queue *rx_queue)
{
	efx_nic_free_buffer(rx_queue->efx, &rx_queue->rxd.buf);
}

/* This creates an entry in the RX descriptor queue */
static inline void
efx_ef10_build_rx_desc(struct efx_rx_queue *rx_queue, unsigned int index)
{
	struct efx_rx_buffer *rx_buf;
	efx_qword_t *rxd;

	rxd = efx_rx_desc(rx_queue, index);
	rx_buf = efx_rx_buffer(rx_queue, index);
	EFX_POPULATE_QWORD_2(*rxd,
			     ESF_DZ_RX_KER_BYTE_CNT, rx_buf->len,
			     ESF_DZ_RX_KER_BUF_ADDR, rx_buf->dma_addr);
}

void _efx_ef10_rx_write(struct efx_rx_queue *rx_queue)
{
	struct efx_nic *efx = rx_queue->efx;
	unsigned int write_count;
	efx_dword_t reg;

	/* Firmware requires that RX_DESC_WPTR be a multiple of 8 */
	write_count = rx_queue->added_count & ~7;
	while (rx_queue->notified_count != write_count) {
		efx_ef10_build_rx_desc(
			rx_queue,
			rx_queue->notified_count & rx_queue->ptr_mask);
		++rx_queue->notified_count;
	}

	wmb();
	EFX_POPULATE_DWORD_1(reg, ERF_DZ_RX_DESC_WPTR,
			     write_count & rx_queue->ptr_mask);
	efx_writed_page(efx, &reg, ER_DZ_RX_DESC_UPD,
			efx_rx_queue_index(rx_queue));
}

static void efx_ef10_rx_write(struct efx_rx_queue *rx_queue)
{
	unsigned int write_count;

	/* Firmware requires that RX_DESC_WPTR be a multiple of 8 */
	write_count = rx_queue->added_count & ~7;
	if (rx_queue->notified_count == write_count)
		return;

	_efx_ef10_rx_write(rx_queue);
}

static int efx_ef10_rx_defer_refill(struct efx_rx_queue *rx_queue)
{
	struct efx_channel *channel = efx_rx_queue_channel(rx_queue);
	MCDI_DECLARE_BUF(inbuf, MC_CMD_DRIVER_EVENT_IN_LEN);
	efx_qword_t event;
	size_t outlen;

	EFX_POPULATE_QWORD_2(event,
			     ESF_DZ_EV_CODE, EFX_EF10_DRVGEN_EV,
			     ESF_DZ_EV_DATA, EFX_EF10_REFILL);

	MCDI_SET_DWORD(inbuf, DRIVER_EVENT_IN_EVQ, channel->channel);

	/* MCDI_SET_QWORD is not appropriate here since EFX_POPULATE_* has
	 * already swapped the data to little-endian order.
	 */
	memcpy(MCDI_PTR(inbuf, DRIVER_EVENT_IN_DATA), &event.u64[0],
	       sizeof(efx_qword_t));

	return efx_mcdi_rpc(channel->efx, MC_CMD_DRIVER_EVENT,
			    inbuf, sizeof(inbuf), NULL, 0, &outlen);
}

static int efx_ef10_ev_probe(struct efx_channel *channel)
{
	return efx_nic_alloc_buffer(channel->efx, &channel->eventq.buf,
				    (channel->eventq_mask + 1) *
				    sizeof(efx_qword_t),
				    GFP_KERNEL);
}

static void efx_ef10_ev_fini(struct efx_channel *channel)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_FINI_EVQ_IN_LEN);
	MCDI_DECLARE_BUF_ERR(outbuf);
	struct efx_nic *efx = channel->efx;
	size_t outlen;
	int rc;

	MCDI_SET_DWORD(inbuf, FINI_EVQ_IN_INSTANCE, channel->channel);

	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_FINI_EVQ, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), &outlen);

	if (rc && rc != -EALREADY)
		goto fail;

	return;

fail:
	efx_mcdi_display_error(efx, MC_CMD_FINI_EVQ, MC_CMD_FINI_EVQ_IN_LEN,
			       outbuf, outlen, rc);
}

static int efx_ef10_ev_init(struct efx_channel *channel)
{
	MCDI_DECLARE_BUF(inbuf,
			 MC_CMD_INIT_EVQ_V2_IN_LEN(EFX_MAX_EVQ_SIZE * 8 /
						   EFX_BUF_SIZE));
	MCDI_DECLARE_BUF(outbuf, MC_CMD_INIT_EVQ_V2_OUT_LEN);
	size_t entries = channel->eventq.buf.len / EFX_BUF_SIZE;
	struct efx_nic *efx = channel->efx;
	struct efx_ef10_nic_data *nic_data;
	size_t inlen, outlen;
	unsigned int enabled, implemented;
	dma_addr_t dma_addr;
	int rc;
	int i;

	nic_data = efx->nic_data;

	/* Fill event queue with all ones (i.e. empty events) */
	memset(channel->eventq.buf.addr, 0xff, channel->eventq.buf.len);

	MCDI_SET_DWORD(inbuf, INIT_EVQ_IN_SIZE, channel->eventq_mask + 1);
	MCDI_SET_DWORD(inbuf, INIT_EVQ_IN_INSTANCE, channel->channel);
	/* INIT_EVQ expects index in vector table, not absolute */
	MCDI_SET_DWORD(inbuf, INIT_EVQ_IN_IRQ_NUM, channel->channel);
	MCDI_SET_DWORD(inbuf, INIT_EVQ_IN_TMR_MODE,
		       MC_CMD_INIT_EVQ_IN_TMR_MODE_DIS);
	MCDI_SET_DWORD(inbuf, INIT_EVQ_IN_TMR_LOAD, 0);
	MCDI_SET_DWORD(inbuf, INIT_EVQ_IN_TMR_RELOAD, 0);
	MCDI_SET_DWORD(inbuf, INIT_EVQ_IN_COUNT_MODE,
		       MC_CMD_INIT_EVQ_IN_COUNT_MODE_DIS);
	MCDI_SET_DWORD(inbuf, INIT_EVQ_IN_COUNT_THRSHLD, 0);

	if (nic_data->datapath_caps2 &
	    1 << MC_CMD_GET_CAPABILITIES_V2_OUT_INIT_EVQ_V2_LBN) {
		/* Use the new generic approach to specifying event queue
		 * configuration, requesting lower latency or higher throughput.
		 * The options that actually get used appear in the output.
		 */
		int unsigned perf_mode = MC_CMD_INIT_EVQ_V2_IN_FLAG_TYPE_AUTO;

#ifdef EFX_NOT_UPSTREAM
		switch (efx->performance_profile) {
		case EFX_PERFORMANCE_PROFILE_THROUGHPUT:
			perf_mode = MC_CMD_INIT_EVQ_V2_IN_FLAG_TYPE_THROUGHPUT;
			break;
		case EFX_PERFORMANCE_PROFILE_LATENCY:
			perf_mode = MC_CMD_INIT_EVQ_V2_IN_FLAG_TYPE_LOW_LATENCY;
			break;
		default:
			break;
		}
#endif

		MCDI_POPULATE_DWORD_2(inbuf, INIT_EVQ_V2_IN_FLAGS,
				      INIT_EVQ_V2_IN_FLAG_INTERRUPTING, 1,
				      INIT_EVQ_V2_IN_FLAG_TYPE, perf_mode);
	} else {
		bool cut_thru = !(nic_data->datapath_caps &
			1 << MC_CMD_GET_CAPABILITIES_OUT_RX_BATCHING_LBN);

		MCDI_POPULATE_DWORD_4(inbuf, INIT_EVQ_IN_FLAGS,
				      INIT_EVQ_IN_FLAG_INTERRUPTING, 1,
				      INIT_EVQ_IN_FLAG_RX_MERGE, 1,
				      INIT_EVQ_IN_FLAG_TX_MERGE, 1,
				      INIT_EVQ_IN_FLAG_CUT_THRU, cut_thru);
	}


	dma_addr = channel->eventq.buf.dma_addr;
	for (i = 0; i < entries; ++i) {
		MCDI_SET_ARRAY_QWORD(inbuf, INIT_EVQ_IN_DMA_ADDR, i, dma_addr);
		dma_addr += EFX_BUF_SIZE;
	}

	inlen = MC_CMD_INIT_EVQ_IN_LEN(entries);

	rc = efx_mcdi_rpc(efx, MC_CMD_INIT_EVQ, inbuf, inlen,
			  outbuf, sizeof(outbuf), &outlen);

	if (outlen >= MC_CMD_INIT_EVQ_V2_OUT_LEN)
		netif_dbg(efx, drv, efx->net_dev,
			   "Channel %d using event queue flags %08x\n",
			   channel->channel,
			   MCDI_DWORD(outbuf, INIT_EVQ_V2_OUT_FLAGS));

	/* IRQ return is ignored */
	if (channel->channel || rc)
		return rc;

	/* Successfully created event queue on channel 0 */
	rc = efx_mcdi_get_workarounds(efx, &implemented, &enabled);
	if (rc == -ENOSYS) {
		/* GET_WORKAROUNDS was implemented before this workaround,
		 * thus it must be unavailable in this firmware.
		 */
		nic_data->workaround_26807 = false;
		rc = 0;
	} else if (rc) {
		goto fail;
	} else {
		bool want_workaround_26807 = multicast_chaining && \
			(implemented & MC_CMD_GET_WORKAROUNDS_OUT_BUG26807);
		nic_data->workaround_26807 =
			!!(enabled & MC_CMD_GET_WORKAROUNDS_OUT_BUG26807);

		if (want_workaround_26807 && !nic_data->workaround_26807) {
			unsigned int flags;

			rc = efx_mcdi_set_workaround(efx,
						     MC_CMD_WORKAROUND_BUG26807,
						     true, &flags);

			if (!rc) {
				if (flags &
				    1 << MC_CMD_WORKAROUND_EXT_OUT_FLR_DONE_LBN) {
					netif_info(efx, drv, efx->net_dev,
						   "other functions on NIC have been reset\n");

					/* With MCFW v4.6.x and earlier, the
					 * boot count will have incremented,
					 * so re-read the warm_boot_count
					 * value now to ensure this function
					 * doesn't think it has changed next
					 * time it checks.
					 */
					rc = efx_ef10_get_warm_boot_count(efx);
					if (rc >= 0) {
						nic_data->warm_boot_count = rc;
						rc = 0;
					}
				}
				nic_data->workaround_26807 = true;
			} else if (rc == -EPERM) {
				rc = 0;
			}
		}
	}

	if (!rc)
		return 0;

fail:
	efx_ef10_ev_fini(channel);
	return rc;
}

static void efx_ef10_ev_remove(struct efx_channel *channel)
{
	efx_nic_free_buffer(channel->efx, &channel->eventq.buf);
}

static void efx_ef10_handle_rx_wrong_queue(struct efx_rx_queue *rx_queue,
					   unsigned int rx_queue_label)
{
	struct efx_nic *efx = rx_queue->efx;

	netif_info(efx, hw, efx->net_dev,
		   "rx event arrived on queue %d labeled as queue %u\n",
		   efx_rx_queue_index(rx_queue), rx_queue_label);

	efx_schedule_reset(efx, RESET_TYPE_DISABLE);
}

static void
efx_ef10_handle_rx_bad_lbits(struct efx_rx_queue *rx_queue,
			     unsigned int actual, unsigned int expected)
{
	unsigned int dropped = (actual - expected) & rx_queue->ptr_mask;
	struct efx_nic *efx = rx_queue->efx;

	netif_info(efx, hw, efx->net_dev,
		   "dropped %d events (index=%d expected=%d)\n",
		   dropped, actual, expected);

	atomic_inc(&efx->errors.missing_event);
	efx_schedule_reset(efx, RESET_TYPE_DATAPATH);
}

/* partially received RX was aborted. clean up. */
static void efx_ef10_handle_rx_abort(struct efx_rx_queue *rx_queue)
{
	unsigned int rx_desc_ptr;

	netif_dbg(rx_queue->efx, hw, rx_queue->efx->net_dev,
		  "scattered RX aborted (dropping %u buffers)\n",
		  rx_queue->scatter_n);

	rx_desc_ptr = rx_queue->removed_count & rx_queue->ptr_mask;

	efx_rx_packet(rx_queue, rx_desc_ptr, rx_queue->scatter_n,
		      0, EFX_RX_PKT_DISCARD);

	rx_queue->removed_count += rx_queue->scatter_n;
	rx_queue->scatter_n = 0;
	rx_queue->scatter_len = 0;
	++efx_rx_queue_channel(rx_queue)->n_rx_nodesc_trunc;
}

static u16
efx_ef10_handle_rx_event_errors(struct efx_channel *channel,
				unsigned int n_packets,
				unsigned int rx_encap_hdr,
				unsigned int rx_l3_class,
				unsigned int rx_l4_class,
				const efx_qword_t *event)
{
	struct efx_nic *efx = channel->efx;
	bool handled = false;

	if (EFX_QWORD_FIELD(*event, ESF_DZ_RX_ECRC_ERR)) {
		if (!efx->forward_fcs) {
			if (!efx->loopback_selftest)
				channel->n_rx_eth_crc_err += n_packets;
			return EFX_RX_PKT_DISCARD;
		}
		handled = true;
	}
	if (EFX_QWORD_FIELD(*event, ESF_DZ_RX_IPCKSUM_ERR)) {
		if (unlikely(rx_encap_hdr != ESE_EZ_ENCAP_HDR_VXLAN &&
			     rx_l3_class != ESE_DZ_L3_CLASS_IP4 &&
			     rx_l3_class != ESE_DZ_L3_CLASS_IP4_FRAG &&
			     rx_l3_class != ESE_DZ_L3_CLASS_IP6 &&
			     rx_l3_class != ESE_DZ_L3_CLASS_IP6_FRAG))
			netdev_WARN(efx->net_dev,
				    "invalid class for RX_IPCKSUM_ERR: event="
				    EFX_QWORD_FMT "\n",
				    EFX_QWORD_VAL(*event));
		if (!efx->loopback_selftest)
			*(rx_encap_hdr ?
				&channel->n_rx_outer_ip_hdr_chksum_err :
				&channel->n_rx_ip_hdr_chksum_err) += n_packets;
		return 0;
	}
	if (EFX_QWORD_FIELD(*event, ESF_DZ_RX_TCPUDP_CKSUM_ERR)) {
		if (unlikely(rx_encap_hdr != ESE_EZ_ENCAP_HDR_VXLAN &&
			     ((rx_l3_class != ESE_DZ_L3_CLASS_IP4 &&
			       rx_l3_class != ESE_DZ_L3_CLASS_IP6) ||
			      (rx_l4_class != ESE_DZ_L4_CLASS_TCP &&
			       rx_l4_class != ESE_DZ_L4_CLASS_UDP))))
			netdev_WARN(efx->net_dev,
				    "invalid class for RX_TCPUDP_CKSUM_ERR: event="
				    EFX_QWORD_FMT "\n",
				    EFX_QWORD_VAL(*event));
		if (!efx->loopback_selftest)
			*(rx_encap_hdr ?
				&channel->n_rx_outer_tcp_udp_chksum_err :
				&channel->n_rx_tcp_udp_chksum_err) += n_packets;
		return 0;
	}
	if (EFX_QWORD_FIELD(*event, ESF_EZ_RX_IP_INNER_CHKSUM_ERR)) {
		if (unlikely(!rx_encap_hdr))
			netdev_WARN(efx->net_dev,
				    "invalid encapsulation type for RX_IP_INNER_CHKSUM_ERR: event="
				    EFX_QWORD_FMT "\n",
				    EFX_QWORD_VAL(*event));
		else if (unlikely(rx_l3_class != ESE_DZ_L3_CLASS_IP4 &&
				  rx_l3_class != ESE_DZ_L3_CLASS_IP4_FRAG &&
				  rx_l3_class != ESE_DZ_L3_CLASS_IP6 &&
				  rx_l3_class != ESE_DZ_L3_CLASS_IP6_FRAG))
			netdev_WARN(efx->net_dev,
				    "invalid class for RX_IP_INNER_CHKSUM_ERR: event="
				    EFX_QWORD_FMT "\n",
				    EFX_QWORD_VAL(*event));
		if (!efx->loopback_selftest)
			channel->n_rx_inner_ip_hdr_chksum_err += n_packets;
		return 0;
	}
	if (EFX_QWORD_FIELD(*event, ESF_EZ_RX_TCP_UDP_INNER_CHKSUM_ERR)) {
		if (unlikely(!rx_encap_hdr))
			netdev_WARN(efx->net_dev, 
				    "invalid encapsulation type for RX_TCP_UDP_INNER_CHKSUM_ERR: event="
				    EFX_QWORD_FMT "\n",
				    EFX_QWORD_VAL(*event));
		else if (unlikely((rx_l3_class != ESE_DZ_L3_CLASS_IP4 &&
				   rx_l3_class != ESE_DZ_L3_CLASS_IP6) ||
				  (rx_l4_class != ESE_DZ_L4_CLASS_TCP &&
				   rx_l4_class != ESE_DZ_L4_CLASS_UDP)))
			netdev_WARN(efx->net_dev,
				    "invalid class for RX_TCP_UDP_INNER_CHKSUM_ERR: event="
				    EFX_QWORD_FMT "\n",
				    EFX_QWORD_VAL(*event));
		if (!efx->loopback_selftest)
			channel->n_rx_inner_tcp_udp_chksum_err += n_packets;
		return 0;
	}

	WARN_ON(!handled); /* No error bits were recognised */
	return 0;
}

static int efx_ef10_handle_rx_event(struct efx_channel *channel,
				    const efx_qword_t *event)
{
	unsigned int rx_bytes, next_ptr_lbits, rx_queue_label, rx_l4_class;
	unsigned int rx_eth_tag_class, rx_l3_class, rx_encap_hdr;
	unsigned int n_descs, n_packets, i;
	struct efx_nic *efx = channel->efx;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_rx_queue *rx_queue;
	efx_qword_t errors;
	bool rx_cont;
	u16 flags = 0;

	if (unlikely(ACCESS_ONCE(efx->reset_pending)))
		return 0;

	/* Basic packet information */
	rx_bytes = EFX_QWORD_FIELD(*event, ESF_DZ_RX_BYTES);
	next_ptr_lbits = EFX_QWORD_FIELD(*event, ESF_DZ_RX_DSC_PTR_LBITS);
	rx_queue_label = EFX_QWORD_FIELD(*event, ESF_DZ_RX_QLABEL);
	rx_eth_tag_class = EFX_QWORD_FIELD(*event, ESF_DZ_RX_ETH_TAG_CLASS);
	rx_l3_class = EFX_QWORD_FIELD(*event, ESF_DZ_RX_L3_CLASS);
	rx_l4_class = EFX_QWORD_FIELD(*event, ESF_DZ_RX_L4_CLASS);
	rx_cont = EFX_QWORD_FIELD(*event, ESF_DZ_RX_CONT);
	rx_encap_hdr =
		nic_data->datapath_caps &
			(1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN) ?
		EFX_QWORD_FIELD(*event, ESF_EZ_RX_ENCAP_HDR) :
		ESE_EZ_ENCAP_HDR_NONE;

	if (EFX_QWORD_FIELD(*event, ESF_DZ_RX_DROP_EVENT))
		netdev_WARN(efx->net_dev, "saw RX_DROP_EVENT: event="
			    EFX_QWORD_FMT "\n",
			    EFX_QWORD_VAL(*event));

	rx_queue = efx_channel_get_rx_queue(channel);

	if (unlikely(rx_queue_label != efx_rx_queue_index(rx_queue)))
		efx_ef10_handle_rx_wrong_queue(rx_queue, rx_queue_label);

	n_descs = ((next_ptr_lbits - rx_queue->removed_count) &
		   ((1 << ESF_DZ_RX_DSC_PTR_LBITS_WIDTH) - 1));

	if (n_descs != rx_queue->scatter_n + 1) {
		struct efx_ef10_nic_data *nic_data = efx->nic_data;

		/* detect rx abort */
		if (unlikely(n_descs == rx_queue->scatter_n)) {
			if (rx_queue->scatter_n == 0 || rx_bytes != 0)
				netdev_WARN(efx->net_dev,
					    "invalid RX abort: scatter_n=%u event="
					    EFX_QWORD_FMT "\n",
					    rx_queue->scatter_n,
					    EFX_QWORD_VAL(*event));
			efx_ef10_handle_rx_abort(rx_queue);
			return 0;
		}

		/* Check that RX completion merging is valid, i.e.
		 * the current firmware supports it and this is a
		 * non-scattered packet.
		 */
		if (!(nic_data->datapath_caps &
		      (1 << MC_CMD_GET_CAPABILITIES_OUT_RX_BATCHING_LBN)) ||
		    rx_queue->scatter_n != 0 || rx_cont) {
			efx_ef10_handle_rx_bad_lbits(
				rx_queue, next_ptr_lbits,
				(rx_queue->removed_count +
				 rx_queue->scatter_n + 1) &
				((1 << ESF_DZ_RX_DSC_PTR_LBITS_WIDTH) - 1));
			return 0;
		}

		/* Merged completion for multiple non-scattered packets */
		rx_queue->scatter_n = 1;
		rx_queue->scatter_len = 0;
		n_packets = n_descs;
		++channel->n_rx_merge_events;
		channel->n_rx_merge_packets += n_packets;
		flags |= EFX_RX_PKT_PREFIX_LEN;
	} else {
		++rx_queue->scatter_n;
		rx_queue->scatter_len += rx_bytes;
		if (rx_cont)
			return 0;
		n_packets = 1;
	}

	EFX_POPULATE_QWORD_5(errors, ESF_DZ_RX_ECRC_ERR, 1,
				     ESF_DZ_RX_IPCKSUM_ERR, 1,
				     ESF_DZ_RX_TCPUDP_CKSUM_ERR, 1,
				     ESF_EZ_RX_IP_INNER_CHKSUM_ERR, 1,
				     ESF_EZ_RX_TCP_UDP_INNER_CHKSUM_ERR, 1);
	EFX_AND_QWORD(errors, *event, errors);
	if (unlikely(!EFX_QWORD_IS_ZERO(errors))) {
		flags |= efx_ef10_handle_rx_event_errors(channel, n_packets,
							 rx_encap_hdr,
							 rx_l3_class, rx_l4_class,
							 event);
	} else {
		if (rx_l3_class == ESE_DZ_L3_CLASS_IP4)
			flags |= EFX_RX_PKT_IPV4;
		else if (rx_l3_class == ESE_DZ_L3_CLASS_IP6)
			flags |= EFX_RX_PKT_IPV6;

		switch (rx_encap_hdr) {
		case ESE_EZ_ENCAP_HDR_VXLAN: /* VxLAN or GENEVE */
			flags |= EFX_RX_PKT_CSUMMED | EFX_RX_PKT_CSUM_LEVEL;
			break;
		case ESE_EZ_ENCAP_HDR_GRE:
			if (rx_l4_class == ESE_DZ_L4_CLASS_TCP ||
			    rx_l4_class == ESE_DZ_L4_CLASS_UDP)
				flags |= EFX_RX_PKT_CSUMMED;
			break;
		case ESE_EZ_ENCAP_HDR_NONE:
			if (rx_l4_class == ESE_DZ_L4_CLASS_TCP)
				flags |= EFX_RX_PKT_TCP | EFX_RX_PKT_CSUMMED;
			else if (rx_l4_class == ESE_DZ_L4_CLASS_UDP)
				flags |= EFX_RX_PKT_CSUMMED;
			break;
		default:
			netdev_WARN(efx->net_dev,
				   "unknown encapsulation type: event="
				   EFX_QWORD_FMT "\n",
				   EFX_QWORD_VAL(*event));
		}
	}

	if (rx_eth_tag_class == ESE_DZ_ETH_TAG_CLASS_VLAN1 ||
	    rx_eth_tag_class == ESE_DZ_ETH_TAG_CLASS_VLAN2)
		flags |= EFX_RX_PKT_VLAN;

	channel->irq_mod_score += 2 * n_packets;

	/* XXX for bug 37073 */
	if (rx_queue->added_count - rx_queue->removed_count < n_descs)
		netdev_WARN(efx->net_dev,
			    "invalid completion index: added_count=%#x removed_count=%#x n_packets=%u scatter_n=%u\n",
			    rx_queue->added_count, rx_queue->removed_count,
			    n_packets, rx_queue->scatter_n);

	/* Handle received packet(s) */
	for (i = 0; i < n_packets; i++) {
		efx_rx_packet(rx_queue,
			      rx_queue->removed_count & rx_queue->ptr_mask,
			      rx_queue->scatter_n, rx_queue->scatter_len,
			      flags);
		rx_queue->removed_count += rx_queue->scatter_n;
	}

	rx_queue->scatter_n = 0;
	rx_queue->scatter_len = 0;

	return n_packets;
}

static u32 efx_ef10_extract_event_ts(efx_qword_t *event)
{
	u32 tstamp;

	tstamp = EFX_QWORD_FIELD(*event, TX_TIMESTAMP_EVENT_TSTAMP_DATA_HI);
	tstamp <<= 16;
	tstamp |= EFX_QWORD_FIELD(*event, TX_TIMESTAMP_EVENT_TSTAMP_DATA_LO);

	return tstamp;
}

static void
efx_ef10_handle_tx_event(struct efx_channel *channel, efx_qword_t *event)
{
	struct efx_nic *efx = channel->efx;
	struct efx_tx_queue *tx_queue;
	unsigned int tx_ev_desc_ptr;
	unsigned int tx_ev_q_label;
	unsigned int tx_ev_type;
	u64 ts_part;

	if (unlikely(ACCESS_ONCE(efx->reset_pending)))
		return;

	if (unlikely(EFX_QWORD_FIELD(*event, ESF_DZ_TX_DROP_EVENT)))
		return;

	/* Get the transmit queue */
	tx_ev_q_label = EFX_QWORD_FIELD(*event, ESF_DZ_TX_QLABEL);
	tx_queue = efx_channel_get_tx_queue(channel, tx_ev_q_label);

	if (!tx_queue->timestamping) {
		tx_ev_desc_ptr = EFX_QWORD_FIELD(*event, ESF_DZ_TX_DESCR_INDX);
		efx_xmit_done(tx_queue, tx_ev_desc_ptr & tx_queue->ptr_mask);
		return;
	}

	/* Transmit timestamps are only available for 8XXX series. They result
	 * in up to three events per packet. These occur in order, and are:
	 *  - the normal completion event (may be omitted)
	 *  - the low part of the timestamp
	 *  - the high part of the timestamp
	 *
	 * It's possible for multiple completion events to appear before the
	 * corresponding timestamps. So we can for example get:
	 *  COMP N
	 *  COMP N+1
	 *  TS_LO N
	 *  TS_HI N
	 *  TS_LO N+1
	 *  TS_HI N+1
	 *
	 * In addition it's also possible for the adjacent completions to be
	 * merged, so we may not see COMP N above. As such, the completion
	 * events are not very useful here.
	 *
	 * Each part of the timestamp is itself split across two 16 bit
	 * fields in the event.
	 */
	tx_ev_type = EFX_QWORD_FIELD(*event, ESF_EZ_TX_SOFT1);

	switch (tx_ev_type) {
	case TX_TIMESTAMP_EVENT_TX_EV_COMPLETION:
		/* Ignore this event - see above. */
		break;

	case TX_TIMESTAMP_EVENT_TX_EV_TSTAMP_LO:
		ts_part = efx_ef10_extract_event_ts(event);
		tx_queue->completed_timestamp_minor = ts_part;
		break;

	case TX_TIMESTAMP_EVENT_TX_EV_TSTAMP_HI:
		ts_part = efx_ef10_extract_event_ts(event);
		tx_queue->completed_timestamp_major = ts_part;
		efx_xmit_done_single(tx_queue);
		break;

	default:
		netif_err(efx, hw, efx->net_dev,
			  "channel %d unknown tx event type %d (data "
			  EFX_QWORD_FMT ")\n",
			  channel->channel, tx_ev_type,
			  EFX_QWORD_VAL(*event));
		break;
	}
}

static int
efx_ef10_handle_driver_event(struct efx_channel *channel,
			     efx_qword_t *event, int budget)
{
	struct efx_nic *efx = channel->efx;
	int subcode;

	subcode = EFX_QWORD_FIELD(*event, ESF_DZ_DRV_SUB_CODE);

	switch (subcode) {
	case ESE_DZ_DRV_TIMER_EV:
	case ESE_DZ_DRV_WAKE_UP_EV:
		return efx_dl_handle_event(efx, event, budget);
	case ESE_DZ_DRV_START_UP_EV:
		/* event queue init complete. ok. */
		break;
	default:
		netif_err(efx, hw, efx->net_dev,
			  "channel %d unknown driver event type %d"
			  " (data " EFX_QWORD_FMT ")\n",
			  channel->channel, subcode,
			  EFX_QWORD_VAL(*event));
		return -EINVAL;
	}
	return 0;
}



static void efx_ef10_handle_driver_generated_event(struct efx_channel *channel,
						   efx_qword_t *event)
{
	struct efx_nic *efx = channel->efx;
	u32 subcode;

	subcode = EFX_QWORD_FIELD(*event, EFX_DWORD_0);

	switch (subcode) {
	case EFX_EF10_TEST:
		channel->event_test_cpu = raw_smp_processor_id();
		break;
	case EFX_EF10_REFILL:
		/* The queue must be empty, so we won't receive any rx
		 * events, so efx_process_channel() won't refill the
		 * queue. Refill it here
		 */
		efx_fast_push_rx_descriptors(&channel->rx_queue, true);
		break;
#ifdef EFX_NOT_UPSTREAM
	case EFX_EF10_RERING_RX_DOORBELL:
		/* For workaround 59975 */
		_efx_ef10_rx_write(&channel->rx_queue);
		break;
#endif
	default:
		netif_err(efx, hw, efx->net_dev,
			  "channel %d unknown driver event type %u"
			  " (data " EFX_QWORD_FMT ")\n",
			  channel->channel, (unsigned int) subcode,
			  EFX_QWORD_VAL(*event));
	}
}

static int efx_ef10_ev_process(struct efx_channel *channel, int quota)
{
	struct efx_nic *efx = channel->efx;
	efx_qword_t cacheline[L1_CACHE_BYTES / sizeof(efx_qword_t)];
	efx_qword_t event, *p_event, *cl_base, *old_cl_base = NULL;
	bool fresh = false;
	unsigned int read_ptr, cr_ptr;
	int ev_code;
	int spent = 0;
	int rc;

	read_ptr = channel->eventq_read_ptr;

	EFX_BUG_ON_PARANOID(!IS_ALIGNED((uintptr_t)channel->eventq.buf.addr,
					L1_CACHE_BYTES));

	for (;;) {
		p_event = efx_event(channel, read_ptr);
		/* We read a whole cacheline at a time, to minimise cache
		 * eviction when hardware and software are in the same
		 * cacheline and DDIO is not present
		 */
		cl_base = (efx_qword_t *)round_down((uintptr_t)p_event, L1_CACHE_BYTES);
		if (cl_base != old_cl_base) {
			/* memcpy could conceivably copy in bytes, which could
			 * lead to holes if the NIC is writing at the same time.
			 * So use open-coded qword copy
			 */
			for (cr_ptr = p_event - cl_base;
			     cr_ptr < ARRAY_SIZE(cacheline); cr_ptr++)
				cacheline[cr_ptr] = cl_base[cr_ptr];
			old_cl_base = cl_base;
			fresh = true;
		}
		event = cacheline[p_event - cl_base];

		if (!efx_event_present(&event)) {
			if (fresh)
				break;
			/* re-read the cacheline, in case the NIC wrote more
			 * events while we were handling the ones we read before
			 */
			old_cl_base = NULL;
			continue;
		}
		fresh = false;

		EFX_SET_QWORD(*p_event);

		++read_ptr;

		ev_code = EFX_QWORD_FIELD(event, ESF_DZ_EV_CODE);

		netif_vdbg(efx, drv, efx->net_dev,
			   "processing event on %d " EFX_QWORD_FMT "\n",
			   channel->channel, EFX_QWORD_VAL(event));

		switch (ev_code) {
		case ESE_DZ_EV_CODE_RX_EV:
			spent += efx_ef10_handle_rx_event(channel, &event);
			if (spent >= quota) {
				/* XXX can we split a merged event to
				 * avoid going over-quota?
				 */
				spent = quota;
				goto out;
			}
			break;
		case ESE_DZ_EV_CODE_TX_EV:
			efx_ef10_handle_tx_event(channel, &event);
			break;
		case ESE_DZ_EV_CODE_MCDI_EV:
			rc = efx_mcdi_process_event(channel, &event,
						    quota - spent);
			if (rc > 0)
				spent += rc;
			else if (rc < 0)
				spent++;
			if (spent >= quota)
				goto out;
			break;
		case ESE_DZ_EV_CODE_DRIVER_EV:
			rc = efx_ef10_handle_driver_event(channel, &event,
							  quota - spent);
			if (rc > 0)
				spent += rc;
			else if (rc < 0)
				spent++;
			if (spent >= quota)
				goto out;
			break;
		case EFX_EF10_DRVGEN_EV:
			efx_ef10_handle_driver_generated_event(channel, &event);
			break;
		default:
			netif_err(efx, hw, efx->net_dev,
				  "channel %d unknown event type %d"
				  " (data " EFX_QWORD_FMT ")\n",
				  channel->channel, ev_code,
				  EFX_QWORD_VAL(event));
		}
	}

out:
	channel->eventq_read_ptr = read_ptr;
	return spent;
}

static void efx_ef10_ev_read_ack(struct efx_channel *channel)
{
	struct efx_nic *efx = channel->efx;
	efx_dword_t rptr;

	if (EFX_EF10_WORKAROUND_35388(efx)) {
		BUILD_BUG_ON(EFX_MIN_EVQ_SIZE <
			     (1 << ERF_DD_EVQ_IND_RPTR_WIDTH));
		BUILD_BUG_ON(EFX_MAX_EVQ_SIZE >
			     (1 << 2 * ERF_DD_EVQ_IND_RPTR_WIDTH));

		EFX_POPULATE_DWORD_2(rptr, ERF_DD_EVQ_IND_RPTR_FLAGS,
				     EFE_DD_EVQ_IND_RPTR_FLAGS_HIGH,
				     ERF_DD_EVQ_IND_RPTR,
				     (channel->eventq_read_ptr &
				      channel->eventq_mask) >>
				     ERF_DD_EVQ_IND_RPTR_WIDTH);
		efx_writed_page(efx, &rptr, ER_DD_EVQ_INDIRECT,
				channel->channel);
		EFX_POPULATE_DWORD_2(rptr, ERF_DD_EVQ_IND_RPTR_FLAGS,
				     EFE_DD_EVQ_IND_RPTR_FLAGS_LOW,
				     ERF_DD_EVQ_IND_RPTR,
				     channel->eventq_read_ptr &
				     ((1 << ERF_DD_EVQ_IND_RPTR_WIDTH) - 1));
		efx_writed_page(efx, &rptr, ER_DD_EVQ_INDIRECT,
				channel->channel);
	} else {
		EFX_POPULATE_DWORD_1(rptr, ERF_DZ_EVQ_RPTR,
				     channel->eventq_read_ptr &
				     channel->eventq_mask);
		efx_writed_page(efx, &rptr, ER_DZ_EVQ_RPTR, channel->channel);
	}
}

static void efx_ef10_ev_test_generate(struct efx_channel *channel)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_DRIVER_EVENT_IN_LEN);
	struct efx_nic *efx = channel->efx;
	efx_qword_t event;
	int rc;

	EFX_POPULATE_QWORD_2(event,
			     ESF_DZ_EV_CODE, EFX_EF10_DRVGEN_EV,
			     ESF_DZ_EV_DATA, EFX_EF10_TEST);

	MCDI_SET_DWORD(inbuf, DRIVER_EVENT_IN_EVQ, channel->channel);

	/* MCDI_SET_QWORD is not appropriate here since EFX_POPULATE_* has
	 * already swapped the data to little-endian order.
	 */
	memcpy(MCDI_PTR(inbuf, DRIVER_EVENT_IN_DATA), &event.u64[0],
	       sizeof(efx_qword_t));

	rc = efx_mcdi_rpc(efx, MC_CMD_DRIVER_EVENT, inbuf, sizeof(inbuf),
			  NULL, 0, NULL);
	if (rc && (rc != -ENETDOWN))
		goto fail;

	return;

fail:
	WARN_ON(true);
	netif_err(efx, hw, efx->net_dev, "%s: failed rc=%d\n", __func__, rc);
}

void efx_ef10_handle_drain_event(struct efx_nic *efx)
{
	if (atomic_dec_and_test(&efx->active_queues))
		wake_up(&efx->flush_wq);

	WARN_ON(atomic_read(&efx->active_queues) < 0);
}

static int efx_ef10_fini_dmaq(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_channel *channel;
	struct efx_tx_queue *tx_queue;
	struct efx_rx_queue *rx_queue;
	int pending;
	bool hw_unavailable;

	/* If the MC has just rebooted, the TX/RX queues will have already been
	 * torn down, but efx->active_queues needs to be set to zero.
	 */
	if (nic_data->must_realloc_vis) {
		atomic_set(&efx->active_queues, 0);
		return 0;
	}

	/* Do not attempt to write to the NIC whilst unavailable */
	hw_unavailable = efx_ef10_hw_unavailable(efx);
	efx_for_each_channel(channel, efx) {
		efx_for_each_channel_rx_queue(rx_queue, channel)
			efx_ef10_rx_fini(rx_queue, hw_unavailable);
		efx_for_each_channel_tx_queue(tx_queue, channel)
			efx_ef10_tx_fini(tx_queue, hw_unavailable);
	}

	if (!hw_unavailable) {
		wait_event_timeout(efx->flush_wq,
				   atomic_read(&efx->active_queues) == 0,
				   msecs_to_jiffies(EFX_MAX_FLUSH_TIME));
		pending = atomic_read(&efx->active_queues);
		if (pending) {
			netif_err(efx, hw, efx->net_dev, "failed to flush %d queues\n",
				  pending);
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static void efx_ef10_prepare_flr(struct efx_nic *efx)
{
	atomic_set(&efx->active_queues, 0);
}

static bool efx_ef10_filter_equal(const struct efx_filter_spec *left,
				  const struct efx_filter_spec *right)
{
	if ((left->match_flags ^ right->match_flags))
		return false;

	return memcmp(&left->outer_vid, &right->outer_vid,
		      sizeof(struct efx_filter_spec) -
		      offsetof(struct efx_filter_spec, outer_vid)) == 0;
}

static unsigned int efx_ef10_filter_hash(const struct efx_filter_spec *spec)
{
	BUILD_BUG_ON(offsetof(struct efx_filter_spec, outer_vid) & 3);
	return jhash2((const u32 *)&spec->outer_vid,
		      (sizeof(struct efx_filter_spec) -
		       offsetof(struct efx_filter_spec, outer_vid)) / 4,
		      0);
	/* XXX should we randomise the initval? */
}

/* Decide whether a filter should be exclusive or else should allow
 * delivery to additional recipients.  Currently we decide that
 * filters for specific local unicast MAC and IP addresses are
 * exclusive.
 */
static bool efx_ef10_filter_is_exclusive(const struct efx_filter_spec *spec)
{
#ifdef EFX_NOT_UPSTREAM
	/* special case ether type and ip proto filters for onload */
	if (spec->match_flags == EFX_FILTER_MATCH_ETHER_TYPE ||
	    spec->match_flags == EFX_FILTER_MATCH_IP_PROTO ||
	    spec->match_flags ==
		     (EFX_FILTER_MATCH_ETHER_TYPE | EFX_FILTER_MATCH_IP_PROTO))
		return true;
#endif
	if (spec->match_flags & EFX_FILTER_MATCH_LOC_MAC &&
	    !is_multicast_ether_addr(spec->loc_mac))
		return true;

	if ((spec->match_flags &
	     (EFX_FILTER_MATCH_ETHER_TYPE | EFX_FILTER_MATCH_LOC_HOST)) ==
	    (EFX_FILTER_MATCH_ETHER_TYPE | EFX_FILTER_MATCH_LOC_HOST)) {
		if (spec->ether_type == htons(ETH_P_IP) &&
		    !(ipv4_is_multicast(spec->loc_host[0]) ||
		      ipv4_is_lbcast(spec->loc_host[0])))
			return true;
		if (spec->ether_type == htons(ETH_P_IPV6) &&
		    ((const u8 *)spec->loc_host)[0] != 0xff)
			return true;
	}

	return false;
}

static void
efx_ef10_filter_set_entry(struct efx_ef10_filter_table *table,
			  unsigned int filter_idx,
			  const struct efx_filter_spec *spec,
			  unsigned int flags)
{
	table->entry[filter_idx].spec =	(unsigned long)spec | flags;
}

static void
efx_ef10_filter_push_prep_set_match_fields(struct efx_nic *efx,
					   const struct efx_filter_spec *spec,
					   efx_dword_t *inbuf)
{
	u32 match_fields = 0;
	enum efx_encap_type encap_type =
		efx_filter_get_encap_type(spec);
	unsigned uc_match, mc_match;

	MCDI_SET_DWORD(inbuf, FILTER_OP_IN_OP,
			efx_ef10_filter_is_exclusive(spec) ?
			MC_CMD_FILTER_OP_IN_OP_INSERT :
			MC_CMD_FILTER_OP_IN_OP_SUBSCRIBE);

	/* Convert match flags and values.  Unlike almost
	 * everything else in MCDI, these fields are in
	 * network byte order.
	 */
#define COPY_FIELD(encap, gen_flag, gen_field, mcdi_field)                   \
	do {                                                         \
		if ((spec)->match_flags &                            \
				EFX_FILTER_MATCH_ ## gen_flag)       \
		COPY_VALUE((encap), (spec)->gen_field,       \
				mcdi_field);                      \
	} while (0)

#define COPY_VALUE(encap, value, mcdi_field)                                 \
	do {                                                         \
		match_fields |= (encap) ?                            \
		1 << MC_CMD_FILTER_OP_EXT_IN_MATCH_IFRM_ ##  \
		mcdi_field ## _LBN :                         \
		1 << MC_CMD_FILTER_OP_EXT_IN_MATCH_ ##       \
		mcdi_field ## _LBN;                          \
		BUILD_BUG_ON(MC_CMD_FILTER_OP_EXT_IN_ ##             \
				mcdi_field ## _LEN <                 \
				sizeof(value));                         \
		memcpy((encap) ?                                     \
				MCDI_PTR(inbuf, FILTER_OP_EXT_IN_IFRM_ ##     \
					mcdi_field) :                 \
				MCDI_PTR(inbuf, FILTER_OP_EXT_IN_ ##          \
					mcdi_field),                  \
				&(value), sizeof(value));                     \
	} while (0)

	/* first handle encapsulation type */
	if (encap_type) {
		__be16 ether_type =
			htons(encap_type & EFX_ENCAP_FLAG_IPV6 ?
			      ETH_P_IPV6 : ETH_P_IP);
		u8 outer_ip_proto; /* needs to be a variable for macro magic */
		u8 tni[4] = {spec->tni >> 16,
			(spec->tni >> 8) & 0xFF,
			spec->tni & 0xFF,
			0};
		bool vxlan = false;

		switch (encap_type & EFX_ENCAP_TYPES_MASK) {
		case EFX_ENCAP_TYPE_VXLAN:
			vxlan = true;
			/* fallthrough */
		case EFX_ENCAP_TYPE_GENEVE:
			COPY_VALUE(false, ether_type, ETHER_TYPE);
			outer_ip_proto = IPPROTO_UDP;
			COPY_VALUE(false, outer_ip_proto, IP_PROTO);
			if (spec->match_flags &
			    EFX_FILTER_MATCH_ENCAP_TNI) {
				match_fields |= 1 <<
					MC_CMD_FILTER_OP_EXT_IN_MATCH_VNI_OR_VSID_LBN;
			}
			/* We always need to set the type field, even if we're
			 * not matching on the TNI.
			 */
			MCDI_POPULATE_DWORD_2(inbuf,
				FILTER_OP_EXT_IN_VNI_OR_VSID,
				FILTER_OP_EXT_IN_VNI_TYPE,
				vxlan ? MC_CMD_FILTER_OP_EXT_IN_VNI_TYPE_VXLAN :
					MC_CMD_FILTER_OP_EXT_IN_VNI_TYPE_GENEVE,
				FILTER_OP_EXT_IN_VNI_VALUE,
				*(__be32 *)&tni);
			break;
		case EFX_ENCAP_TYPE_NVGRE:
			COPY_VALUE(false, ether_type, ETHER_TYPE);
			outer_ip_proto = IPPROTO_GRE;
			COPY_VALUE(false, outer_ip_proto, IP_PROTO);
			if (spec->match_flags &
			    EFX_FILTER_MATCH_ENCAP_TNI) {
				match_fields |= 1 <<
					MC_CMD_FILTER_OP_EXT_IN_MATCH_VNI_OR_VSID_LBN;
				MCDI_POPULATE_DWORD_2(inbuf,
					FILTER_OP_EXT_IN_VNI_OR_VSID,
					FILTER_OP_EXT_IN_VSID_VALUE,
					*(__be32 *)&tni,
					FILTER_OP_EXT_IN_VSID_TYPE,
					MC_CMD_FILTER_OP_EXT_IN_VSID_TYPE_NVGRE);
			}
			break;
		default:
			WARN_ON(1);
		}
		COPY_FIELD(false, OUTER_LOC_MAC, outer_loc_mac, DST_MAC);

		uc_match =
		  MC_CMD_FILTER_OP_EXT_IN_MATCH_IFRM_UNKNOWN_UCAST_DST_LBN;
		mc_match =
		  MC_CMD_FILTER_OP_EXT_IN_MATCH_IFRM_UNKNOWN_MCAST_DST_LBN;
	} else {
		uc_match = MC_CMD_FILTER_OP_EXT_IN_MATCH_UNKNOWN_UCAST_DST_LBN;
		mc_match = MC_CMD_FILTER_OP_EXT_IN_MATCH_UNKNOWN_MCAST_DST_LBN;
	}

	/* special case for mismatch */
	if (spec->match_flags & EFX_FILTER_MATCH_LOC_MAC_IG)
		match_fields |=
			is_multicast_ether_addr(spec->loc_mac) ?
			1 << mc_match :
			1 << uc_match;

	/* VLAN is always outer */
	COPY_FIELD(false, INNER_VID, inner_vid, INNER_VLAN);
	COPY_FIELD(false, OUTER_VID, outer_vid, OUTER_VLAN);
	/* outer MAC only applies if encap */
	if (encap_type)
		COPY_FIELD(false, OUTER_LOC_MAC, outer_loc_mac, DST_MAC);
	/* everything else is inner or outer based on encap type */
	COPY_FIELD(encap_type, REM_HOST, rem_host, SRC_IP);
	COPY_FIELD(encap_type, LOC_HOST, loc_host, DST_IP);
	COPY_FIELD(encap_type, REM_MAC, rem_mac, SRC_MAC);
	COPY_FIELD(encap_type, REM_PORT, rem_port, SRC_PORT);
	COPY_FIELD(encap_type, LOC_MAC, loc_mac, DST_MAC);
	COPY_FIELD(encap_type, LOC_PORT, loc_port, DST_PORT);
	COPY_FIELD(encap_type, ETHER_TYPE, ether_type, ETHER_TYPE);
	COPY_FIELD(encap_type, IP_PROTO, ip_proto, IP_PROTO);
#undef COPY_FIELD
#undef COPY_VALUE
	MCDI_SET_DWORD(inbuf, FILTER_OP_IN_MATCH_FIELDS,
			match_fields);
}

static void efx_ef10_filter_push_prep(struct efx_nic *efx,
				      const struct efx_filter_spec *spec,
				      efx_dword_t *inbuf, u64 handle,
				      bool replacing)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	unsigned int port_id;
	u32 flags = spec->flags;

	if (flags & EFX_FILTER_FLAG_RX_RSS &&
	    spec->rss_context == EFX_FILTER_RSS_CONTEXT_DEFAULT &&
	    nic_data->rx_rss_context == EFX_EF10_RSS_CONTEXT_INVALID)
		flags &= ~EFX_FILTER_FLAG_RX_RSS;

	memset(inbuf, 0, MC_CMD_FILTER_OP_EXT_IN_LEN);

	if (replacing) {
		MCDI_SET_DWORD(inbuf, FILTER_OP_IN_OP,
			       MC_CMD_FILTER_OP_IN_OP_REPLACE);
		MCDI_SET_QWORD(inbuf, FILTER_OP_IN_HANDLE, handle);
	} else {
		efx_ef10_filter_push_prep_set_match_fields(efx, spec, inbuf);
	}

	port_id = nic_data->vport_id;
	if (flags & EFX_FILTER_FLAG_VPORT_ID)
		port_id = spec->vport_id;
	if (flags & EFX_FILTER_FLAG_STACK_ID)
		port_id |= EVB_STACK_ID(spec->stack_id);
	MCDI_SET_DWORD(inbuf, FILTER_OP_IN_PORT_ID, port_id );
	MCDI_SET_DWORD(inbuf, FILTER_OP_IN_RX_DEST,
		       spec->dmaq_id == EFX_FILTER_RX_DMAQ_ID_DROP ?
		       MC_CMD_FILTER_OP_IN_RX_DEST_DROP :
		       MC_CMD_FILTER_OP_IN_RX_DEST_HOST);

#ifdef EFX_NOT_UPSTREAM
	/* Onload might set this flag to request  hardware multicast loopback */
#endif
	if (spec->flags & EFX_FILTER_FLAG_TX)
		MCDI_POPULATE_DWORD_2(inbuf, FILTER_OP_IN_TX_DEST,
				      FILTER_OP_IN_TX_DEST_MAC, 1,
				      FILTER_OP_IN_TX_DEST_PM, 1);
	else if (spec->flags & EFX_FILTER_FLAG_LOOPBACK)
		MCDI_POPULATE_DWORD_1(inbuf, FILTER_OP_IN_TX_DEST,
				      FILTER_OP_IN_TX_DEST_MAC, 1);
	else
		MCDI_SET_DWORD(inbuf, FILTER_OP_IN_TX_DEST,
			       MC_CMD_FILTER_OP_IN_TX_DEST_DEFAULT);
	MCDI_SET_DWORD(inbuf, FILTER_OP_IN_TX_DOMAIN, 0);

	MCDI_SET_DWORD(inbuf, FILTER_OP_IN_RX_QUEUE,
		       spec->dmaq_id == EFX_FILTER_RX_DMAQ_ID_DROP ?
		       0 : spec->dmaq_id);
	MCDI_SET_DWORD(inbuf, FILTER_OP_IN_RX_MODE,
		       (flags & EFX_FILTER_FLAG_RX_RSS) ?
		       MC_CMD_FILTER_OP_IN_RX_MODE_RSS :
		       MC_CMD_FILTER_OP_IN_RX_MODE_SIMPLE);
	if (flags & EFX_FILTER_FLAG_RX_RSS)
		MCDI_SET_DWORD(inbuf, FILTER_OP_IN_RX_CONTEXT,
			       spec->rss_context !=
			       EFX_FILTER_RSS_CONTEXT_DEFAULT ?
			       spec->rss_context : nic_data->rx_rss_context);
}

static int efx_ef10_filter_push(struct efx_nic *efx,
				const struct efx_filter_spec *spec,
				u64 *handle, bool replacing)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_FILTER_OP_EXT_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_FILTER_OP_EXT_OUT_LEN);
	int rc;

	efx_ef10_filter_push_prep(efx, spec, inbuf, *handle, replacing);
	rc = efx_mcdi_rpc(efx, MC_CMD_FILTER_OP, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), NULL);
	if (rc == 0)
		*handle = MCDI_QWORD(outbuf, FILTER_OP_OUT_HANDLE);
	if (rc == -ENOSPC)
		rc = -EBUSY; /* to match efx_farch_filter_insert() */
	return rc;
}

static u32 efx_ef10_filter_mcdi_flags_from_spec(const struct efx_filter_spec *spec)
{
	unsigned int match_flags = spec->match_flags;
	u32 mcdi_flags = 0;
	enum efx_encap_type encap_type =
		efx_filter_get_encap_type(spec);
	unsigned uc_match, mc_match;

#define MAP_FILTER_TO_MCDI_FLAG(gen_flag, mcdi_field, encap) {		\
		unsigned int  old_match_flags = match_flags;		\
		match_flags &= ~EFX_FILTER_MATCH_ ## gen_flag;		\
		if (match_flags != old_match_flags)			\
			mcdi_flags |=					\
				(1 << ((encap) ?			\
				       MC_CMD_FILTER_OP_EXT_IN_MATCH_IFRM_ ## \
				       mcdi_field ## _LBN :		\
				       MC_CMD_FILTER_OP_EXT_IN_MATCH_ ##\
				       mcdi_field ## _LBN));		\
	}
	/* inner or outer based on encap type */
	MAP_FILTER_TO_MCDI_FLAG(REM_HOST, SRC_IP, encap_type);
	MAP_FILTER_TO_MCDI_FLAG(LOC_HOST, DST_IP, encap_type);
	MAP_FILTER_TO_MCDI_FLAG(REM_MAC, SRC_MAC, encap_type);
	MAP_FILTER_TO_MCDI_FLAG(REM_PORT, SRC_PORT, encap_type);
	MAP_FILTER_TO_MCDI_FLAG(LOC_MAC, DST_MAC, encap_type);
	MAP_FILTER_TO_MCDI_FLAG(LOC_PORT, DST_PORT, encap_type);
	MAP_FILTER_TO_MCDI_FLAG(ETHER_TYPE, ETHER_TYPE, encap_type);
	MAP_FILTER_TO_MCDI_FLAG(IP_PROTO, IP_PROTO, encap_type);
	/* always outer */
	MAP_FILTER_TO_MCDI_FLAG(INNER_VID, INNER_VLAN, false);
	MAP_FILTER_TO_MCDI_FLAG(OUTER_VID, OUTER_VLAN, false);
#undef MAP_FILTER_TO_MCDI_FLAG
	/* special handling for encap type/tni, and mismatch */
	if (encap_type) {
		match_flags &= ~EFX_FILTER_MATCH_ENCAP_TYPE;
		match_flags &= ~EFX_FILTER_MATCH_ENCAP_TNI;
		mcdi_flags |=
			(1 << MC_CMD_FILTER_OP_EXT_IN_MATCH_VNI_OR_VSID_LBN);
		mcdi_flags |=
			(1 << MC_CMD_FILTER_OP_EXT_IN_MATCH_ETHER_TYPE_LBN);
		mcdi_flags |= (1 << MC_CMD_FILTER_OP_EXT_IN_MATCH_IP_PROTO_LBN);

		if (match_flags & EFX_FILTER_MATCH_OUTER_LOC_MAC) {
			match_flags &= ~EFX_FILTER_MATCH_OUTER_LOC_MAC;
			mcdi_flags |= (1 <<
				MC_CMD_FILTER_OP_EXT_IN_MATCH_DST_MAC_LBN);
		}

		uc_match = MC_CMD_FILTER_OP_EXT_IN_MATCH_IFRM_UNKNOWN_MCAST_DST_LBN;
		mc_match = MC_CMD_FILTER_OP_EXT_IN_MATCH_IFRM_UNKNOWN_UCAST_DST_LBN;
	} else {
		uc_match = MC_CMD_FILTER_OP_EXT_IN_MATCH_UNKNOWN_UCAST_DST_LBN;
		mc_match = MC_CMD_FILTER_OP_EXT_IN_MATCH_UNKNOWN_MCAST_DST_LBN;
	}

	if (match_flags & EFX_FILTER_MATCH_LOC_MAC_IG) {
		match_flags &= ~EFX_FILTER_MATCH_LOC_MAC_IG;
		mcdi_flags |=
			is_multicast_ether_addr(spec->loc_mac) ?
			1 << mc_match :
			1 << uc_match;
	}

	/* Did we map them all? */
	BUG_ON(match_flags);

	return mcdi_flags;
}

static int efx_ef10_filter_pri(struct efx_ef10_filter_table *table,
			       const struct efx_filter_spec *spec)
{
	u32 mcdi_flags = efx_ef10_filter_mcdi_flags_from_spec(spec);
	unsigned int match_pri;

	for (match_pri = 0;
	     match_pri < table->rx_match_count;
	     match_pri++)
		if (table->rx_match_mcdi_flags[match_pri] == mcdi_flags)
			return match_pri;

	return -EPROTONOSUPPORT;
}

static int efx_filter_sanity_check(const struct efx_filter_spec *spec)
{
	/* if ipproto or hosts were specified then only allow the supported
	 * ether types
	 */
	if (((spec->match_flags & EFX_FILTER_MATCH_IP_PROTO) ||
	     (spec->match_flags & EFX_FILTER_MATCH_REM_HOST) ||
	     (spec->match_flags & EFX_FILTER_MATCH_LOC_HOST)) &&
	    (!(spec->match_flags & EFX_FILTER_MATCH_ETHER_TYPE) ||
	     ((spec->ether_type != htons(ETH_P_IP) &&
	       spec->ether_type != htons(ETH_P_IPV6)))))
		return -EINVAL;

	/* if ports were specified then only allow the supported ip protos */
	if (((spec->match_flags & EFX_FILTER_MATCH_LOC_PORT) ||
	     (spec->match_flags & EFX_FILTER_MATCH_REM_PORT)) &&
	    (!(spec->match_flags & EFX_FILTER_MATCH_IP_PROTO) ||
	     (spec->ip_proto != IPPROTO_TCP && spec->ip_proto != IPPROTO_UDP)))
		return -EINVAL;

	return 0;
}

static s32 efx_ef10_filter_insert(struct efx_nic *efx,
				  const struct efx_filter_spec *spec,
				  bool replace_equal)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	DECLARE_BITMAP(mc_rem_map, EFX_EF10_FILTER_SEARCH_LIMIT);
	struct efx_filter_spec *saved_spec;
	unsigned int match_pri, hash;
	unsigned int priv_flags;
	bool replacing = false;
	int ins_index = -1;
	DEFINE_WAIT(wait);
	bool is_mc_recip;
	s32 rc;

	/* Support only RX or RX+TX filters. */
	if ((spec->flags & EFX_FILTER_FLAG_RX) == 0)
		return -EINVAL;
	/* TX and loopback are mutually exclusive */
	if ((spec->flags & EFX_FILTER_FLAG_TX) &&
	    (spec->flags & EFX_FILTER_FLAG_LOOPBACK))
		return -EINVAL;

	rc = efx_filter_sanity_check(spec);
	if (rc)
		return rc;

	rc = efx_ef10_filter_pri(table, spec);
	if (rc < 0)
		return rc;
	match_pri = rc;

	hash = efx_ef10_filter_hash(spec);
	is_mc_recip = efx_filter_is_mc_recipient(spec);
	if (is_mc_recip)
		bitmap_zero(mc_rem_map, EFX_EF10_FILTER_SEARCH_LIMIT);

	/* Find any existing filters with the same match tuple or
	 * else a free slot to insert at.  If any of them are busy,
	 * we have to wait and retry.
	 */
	for (;;) {
		unsigned int depth = 1;
		unsigned int i;

		spin_lock_bh(&efx->filter_lock);

#ifdef EFX_NOT_UPSTREAM
		if (spec->priority <= EFX_FILTER_PRI_AUTO &&
		    table->kernel_blocked[is_mc_recip ?
					  EFX_DL_FILTER_BLOCK_KERNEL_MCAST :
					  EFX_DL_FILTER_BLOCK_KERNEL_UCAST]) {
			rc = -EPERM;
			goto out_unlock;
		}
#endif

		for (;;) {
			i = (hash + depth) & (HUNT_FILTER_TBL_ROWS - 1);
			saved_spec = efx_ef10_filter_entry_spec(table, i);

			if (!saved_spec) {
				if (ins_index < 0)
					ins_index = i;
			} else if (efx_ef10_filter_equal(spec, saved_spec)) {
				if (table->entry[i].spec &
				    EFX_EF10_FILTER_FLAG_BUSY)
					break;
				if (spec->priority < saved_spec->priority &&
				    spec->priority != EFX_FILTER_PRI_AUTO) {
					rc = -EPERM;
					goto out_unlock;
				}
				if (!is_mc_recip) {
					/* This is the only one */
					if (spec->priority ==
					    saved_spec->priority &&
					    !replace_equal) {
						rc = -EEXIST;
						goto out_unlock;
					}
					ins_index = i;
					goto found;
				} else if (spec->priority >
					   saved_spec->priority ||
					   (spec->priority ==
					    saved_spec->priority &&
					    replace_equal) ||
					   spec->priority ==
						EFX_FILTER_PRI_AUTO) {
					if (ins_index < 0)
						ins_index = i;
					else
						__set_bit(depth, mc_rem_map);
				}
			}

			/* Once we reach the maximum search depth, use
			 * the first suitable slot or return -EBUSY if
			 * there was none
			 */
			if (depth == EFX_EF10_FILTER_SEARCH_LIMIT) {
				if (ins_index < 0) {
					rc = -EBUSY;
					goto out_unlock;
				}
				goto found;
			}

			++depth;
		}

		prepare_to_wait(&table->waitq, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_bh(&efx->filter_lock);
		schedule();
	}

found:
	/* Create a software table entry if necessary, and mark it
	 * busy.  We might yet fail to insert, but any attempt to
	 * insert a conflicting filter while we're waiting for the
	 * firmware must find the busy entry.
	 */
	saved_spec = efx_ef10_filter_entry_spec(table, ins_index);
	if (saved_spec) {
		if (spec->priority == EFX_FILTER_PRI_AUTO &&
		    saved_spec->priority >= EFX_FILTER_PRI_AUTO) {
			/* Just make sure it won't be removed */
			if (saved_spec->priority > EFX_FILTER_PRI_AUTO)
				saved_spec->flags |= EFX_FILTER_FLAG_RX_OVER_AUTO;
			table->entry[ins_index].spec &=
				~EFX_EF10_FILTER_FLAG_AUTO_OLD;
			rc = ins_index;
			goto out_unlock;
		}
		replacing = true;
		priv_flags = efx_ef10_filter_entry_flags(table, ins_index);
	} else {
		saved_spec = kmalloc(sizeof(*spec), GFP_ATOMIC);
		if (!saved_spec) {
			rc = -ENOMEM;
			goto out_unlock;
		}
		*saved_spec = *spec;
		priv_flags = 0;
	}
	efx_ef10_filter_set_entry(table, ins_index, saved_spec,
				  priv_flags | EFX_EF10_FILTER_FLAG_BUSY);

	/* Mark lower-priority multicast recipients busy prior to removal */
	if (is_mc_recip) {
		unsigned int depth, i;

		for (depth = 0; depth < EFX_EF10_FILTER_SEARCH_LIMIT; depth++) {
			i = (hash + depth) & (HUNT_FILTER_TBL_ROWS - 1);
			if (test_bit(depth, mc_rem_map))
				table->entry[i].spec |=
					EFX_EF10_FILTER_FLAG_BUSY;
		}
	}

	spin_unlock_bh(&efx->filter_lock);

	rc = efx_ef10_filter_push(efx, spec, &table->entry[ins_index].handle,
				  replacing);

	/* Finalise the software table entry */
	spin_lock_bh(&efx->filter_lock);
	if (rc == 0) {
		if (replacing) {
			/* Update the fields that may differ */
			if (saved_spec->priority == EFX_FILTER_PRI_AUTO)
				saved_spec->flags |=
					EFX_FILTER_FLAG_RX_OVER_AUTO;
			saved_spec->priority = spec->priority;
			saved_spec->flags &= EFX_FILTER_FLAG_RX_OVER_AUTO;
			saved_spec->flags |= spec->flags;
			saved_spec->rss_context = spec->rss_context;
			saved_spec->dmaq_id = spec->dmaq_id;
		}
	} else if (!replacing) {
		kfree(saved_spec);
		saved_spec = NULL;
	}
	efx_ef10_filter_set_entry(table, ins_index, saved_spec, priv_flags);

	/* Remove and finalise entries for lower-priority multicast
	 * recipients
	 */
	if (is_mc_recip) {
		MCDI_DECLARE_BUF(inbuf, MC_CMD_FILTER_OP_EXT_IN_LEN);
		unsigned int depth, i;

		memset(inbuf, 0, sizeof(inbuf));

		for (depth = 0; depth < EFX_EF10_FILTER_SEARCH_LIMIT; depth++) {
			if (!test_bit(depth, mc_rem_map))
				continue;

			i = (hash + depth) & (HUNT_FILTER_TBL_ROWS - 1);
			saved_spec = efx_ef10_filter_entry_spec(table, i);
			priv_flags = efx_ef10_filter_entry_flags(table, i);

			if (rc == 0) {
				spin_unlock_bh(&efx->filter_lock);
				MCDI_SET_DWORD(inbuf, FILTER_OP_IN_OP,
					       MC_CMD_FILTER_OP_IN_OP_UNSUBSCRIBE);
				MCDI_SET_QWORD(inbuf, FILTER_OP_IN_HANDLE,
					       table->entry[i].handle);
				rc = efx_mcdi_rpc(efx, MC_CMD_FILTER_OP,
						  inbuf, sizeof(inbuf),
						  NULL, 0, NULL);
				spin_lock_bh(&efx->filter_lock);
			}

			if (rc == 0) {
				kfree(saved_spec);
				saved_spec = NULL;
				priv_flags = 0;
			} else {
				priv_flags &= ~EFX_EF10_FILTER_FLAG_BUSY;
			}
			efx_ef10_filter_set_entry(table, i, saved_spec,
						  priv_flags);
		}
	}

	/* If successful, return the inserted filter ID */
	if (rc == 0)
		rc = efx_ef10_make_filter_id(match_pri, ins_index);

	wake_up_all(&table->waitq);
out_unlock:
	spin_unlock_bh(&efx->filter_lock);
	finish_wait(&table->waitq, &wait);
	return rc;
}

#ifdef EFX_NOT_UPSTREAM
static s32 efx_ef10_filter_insert_locked(struct efx_nic *efx,
					 const struct efx_filter_spec *spec,
					 bool replace_equal)
{
	s32 rc;

	down_read(&efx->filter_sem);
	rc = efx_ef10_filter_insert(efx, spec, replace_equal);
	up_read(&efx->filter_sem);
	return rc;
}
#endif

static void efx_ef10_filter_update_rx_scatter(struct efx_nic *efx)
{
	/* no need to do anything here on EF10 */
}

/* Remove a filter.
 * If !by_index, remove by ID
 * If by_index, remove by index
 * Filter ID may come from userland and must be range-checked.
 */
static int efx_ef10_filter_remove_internal(struct efx_nic *efx,
					   unsigned int priority_mask,
					   u32 filter_id, bool by_index)
{
	unsigned int filter_idx = efx_ef10_filter_get_unsafe_id(efx, filter_id);
	struct efx_ef10_filter_table *table = efx->filter_state;
	MCDI_DECLARE_BUF(inbuf, MC_CMD_FILTER_OP_IN_LEN);
	struct efx_filter_spec *spec;
	DEFINE_WAIT(wait);
	int rc;

	/* Find the software table entry and mark it busy.  Don't
	 * remove it yet; any attempt to update while we're waiting
	 * for the firmware must find the busy entry.
	 */
	for (;;) {
		spin_lock_bh(&efx->filter_lock);
		if (!(table->entry[filter_idx].spec &
		      EFX_EF10_FILTER_FLAG_BUSY))
			break;
		prepare_to_wait(&table->waitq, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_bh(&efx->filter_lock);
		schedule();
	}

	spec = efx_ef10_filter_entry_spec(table, filter_idx);
	if (!spec ||
	    (!by_index &&
	     efx_ef10_filter_pri(table, spec) !=
	     efx_ef10_filter_get_unsafe_pri(filter_id))) {
		rc = -ENOENT;
		goto out_unlock;
	}

	if (spec->flags & EFX_FILTER_FLAG_RX_OVER_AUTO &&
	    priority_mask == (1U << EFX_FILTER_PRI_AUTO)) {
		/* Just remove flags */
		spec->flags &= ~EFX_FILTER_FLAG_RX_OVER_AUTO;
		table->entry[filter_idx].spec &= ~EFX_EF10_FILTER_FLAG_AUTO_OLD;
		rc = 0;
		goto out_unlock;
	}

	if (!(priority_mask & (1U << spec->priority))) {
		rc = -ENOENT;
		goto out_unlock;
	}

	table->entry[filter_idx].spec |= EFX_EF10_FILTER_FLAG_BUSY;
	spin_unlock_bh(&efx->filter_lock);

	if (spec->flags & EFX_FILTER_FLAG_RX_OVER_AUTO) {
		/* Reset to an automatic filter */

		struct efx_filter_spec new_spec = *spec;

		new_spec.priority = EFX_FILTER_PRI_AUTO;
		new_spec.flags = (EFX_FILTER_FLAG_RX |
			(efx_rss_enabled(efx) ? EFX_FILTER_FLAG_RX_RSS : 0));
		new_spec.dmaq_id = 0;
		new_spec.rss_context = EFX_FILTER_RSS_CONTEXT_DEFAULT;
		rc = efx_ef10_filter_push(efx, &new_spec,
					  &table->entry[filter_idx].handle,
					  true);

		spin_lock_bh(&efx->filter_lock);
		if (rc == 0)
			*spec = new_spec;
	} else {
		/* Really remove the filter */

		MCDI_SET_DWORD(inbuf, FILTER_OP_IN_OP,
			       efx_ef10_filter_is_exclusive(spec) ?
			       MC_CMD_FILTER_OP_IN_OP_REMOVE :
			       MC_CMD_FILTER_OP_IN_OP_UNSUBSCRIBE);
		MCDI_SET_QWORD(inbuf, FILTER_OP_IN_HANDLE,
			       table->entry[filter_idx].handle);
		rc = efx_mcdi_rpc_quiet(efx, MC_CMD_FILTER_OP,
				  inbuf, sizeof(inbuf), NULL, 0, NULL);

		spin_lock_bh(&efx->filter_lock);
		if ((rc == 0) || (rc == -ENOENT)) {
			/* Filter removed OK or didn't actually exist. */
			kfree(spec);
			efx_ef10_filter_set_entry(table, filter_idx, NULL, 0);
		} else {
			efx_mcdi_display_error(efx, MC_CMD_FILTER_OP,
					MC_CMD_FILTER_OP_IN_LEN, NULL, 0, rc);
		}
	}

	table->entry[filter_idx].spec &= ~EFX_EF10_FILTER_FLAG_BUSY;
	wake_up_all(&table->waitq);
out_unlock:
	spin_unlock_bh(&efx->filter_lock);
	finish_wait(&table->waitq, &wait);
	return rc;
}

static int efx_ef10_filter_remove_safe(struct efx_nic *efx,
				       enum efx_filter_priority priority,
				       u32 filter_id)
{
	return efx_ef10_filter_remove_internal(efx, 1U << priority,
					       filter_id, false);
}

#ifdef EFX_NOT_UPSTREAM
static int efx_ef10_filter_remove_safe_locked(struct efx_nic *efx,
					      enum efx_filter_priority priority,
					      u32 filter_id)
{
	int rc;

	down_read(&efx->filter_sem);
	rc = efx_ef10_filter_remove_safe(efx, priority, filter_id);
	up_read(&efx->filter_sem);
	return rc;
}
#endif

static int efx_ef10_filter_remove_unsafe(struct efx_nic *efx,
					 enum efx_filter_priority priority,
					 u32 filter_id)
{
	return efx_ef10_filter_remove_internal(efx, 1U << priority,
					       filter_id, true);
}

static int efx_ef10_filter_get_safe(struct efx_nic *efx,
				    enum efx_filter_priority priority,
				    u32 filter_id, struct efx_filter_spec *spec)
{
	unsigned int filter_idx = efx_ef10_filter_get_unsafe_id(efx, filter_id);
	struct efx_ef10_filter_table *table;
	const struct efx_filter_spec *saved_spec;
	int rc;

#ifdef EFX_NOT_UPSTREAM
	down_read(&efx->filter_sem); /* only required for driverlink */
#endif
	table = efx->filter_state;
	spin_lock_bh(&efx->filter_lock);
	saved_spec = efx_ef10_filter_entry_spec(table, filter_idx);
	if (saved_spec && saved_spec->priority == priority &&
	    efx_ef10_filter_pri(table, saved_spec) ==
	    efx_ef10_filter_get_unsafe_pri(filter_id)) {
		*spec = *saved_spec;
		rc = 0;
	} else {
		rc = -ENOENT;
	}
	spin_unlock_bh(&efx->filter_lock);
#ifdef EFX_NOT_UPSTREAM
	up_read(&efx->filter_sem); /* only required for driverlink */
#endif
	return rc;
}

static int efx_ef10_filter_clear_rx(struct efx_nic *efx,
				     enum efx_filter_priority priority)
{
	unsigned int priority_mask;
	unsigned int i;
	int rc;

	priority_mask = (((1U << (priority + 1)) - 1) &
			 ~(1U << EFX_FILTER_PRI_AUTO));

	for (i = 0; i < HUNT_FILTER_TBL_ROWS; i++) {
		rc = efx_ef10_filter_remove_internal(efx, priority_mask,
						     i, true);
		if (rc && rc != -ENOENT)
			return rc;
	}

	return 0;
}

static int efx_ef10_filter_redirect(struct efx_nic *efx, u32 filter_id,
				    int rxq_i, int stack_id)
{
	struct efx_ef10_filter_table *table;
	struct efx_filter_spec *spec, new_spec;
	unsigned int filter_idx = efx_ef10_filter_get_unsafe_id(efx, filter_id);
	DEFINE_WAIT(wait);
	int rc;

#ifdef EFX_NOT_UPSTREAM
	down_read(&efx->filter_sem); /* only required for driverlink */
#endif
	table = efx->filter_state;
	/* Find the software table entry and mark it busy */
	for (;;) {
		spin_lock_bh(&efx->filter_lock);
		if (!(table->entry[filter_idx].spec &
		      EFX_EF10_FILTER_FLAG_BUSY))
			break;
		prepare_to_wait(&table->waitq, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_bh(&efx->filter_lock);
		schedule();
	}
	spec = efx_ef10_filter_entry_spec(table, filter_idx);
	if (!spec) {
		rc = -ENOENT;
		netdev_WARN(efx->net_dev,
			    "invalid filter_id: filter_id=%#x rxq_i=%u\n",
			    filter_id, rxq_i);
		goto out_unlock;
	}
	table->entry[filter_idx].spec |= EFX_EF10_FILTER_FLAG_BUSY;
	spin_unlock_bh(&efx->filter_lock);

	/* Try to redirect */
	new_spec = *spec;
	new_spec.dmaq_id = rxq_i;
	new_spec.stack_id = stack_id;
	rc = efx_ef10_filter_push(efx, &new_spec,
				  &table->entry[filter_idx].handle, true);
	if (rc && (rc != -ENETDOWN))
		netdev_WARN(efx->net_dev,
			    "failed to update filter: filter_id=%#x "
			    "filter_flags = %#x rxq_i=%u "
			    "stack_id = %d rc=%d \n",
			    filter_id, spec->flags, rxq_i,
			    new_spec.stack_id, rc);
	spin_lock_bh(&efx->filter_lock);
	if (!rc)
		*spec = new_spec;
	table->entry[filter_idx].spec &= ~EFX_EF10_FILTER_FLAG_BUSY;
	wake_up_all(&table->waitq);
out_unlock:
	spin_unlock_bh(&efx->filter_lock);
	finish_wait(&table->waitq, &wait);
#ifdef EFX_NOT_UPSTREAM
	up_read(&efx->filter_sem); /* only required for driverlink */
#endif
	return rc;
}

static u32 efx_ef10_filter_count_rx_used(struct efx_nic *efx,
					 enum efx_filter_priority priority)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_filter_spec *spec;
	unsigned int filter_idx;
	s32 count = 0;

	spin_lock_bh(&efx->filter_lock);
	for (filter_idx = 0; filter_idx < HUNT_FILTER_TBL_ROWS; filter_idx++) {
		spec = efx_ef10_filter_entry_spec(table, filter_idx);

		if (spec && spec->priority == priority)
			++count;
	}
	spin_unlock_bh(&efx->filter_lock);
	return count;
}

static u32 efx_ef10_filter_get_rx_id_limit(struct efx_nic *efx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;

	return table->rx_match_count * HUNT_FILTER_TBL_ROWS * 2;
}

static s32 efx_ef10_filter_get_rx_ids(struct efx_nic *efx,
				      enum efx_filter_priority priority,
				      u32 *buf, u32 size)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_filter_spec *spec;
	unsigned int filter_idx;
	s32 count = 0;

	spin_lock_bh(&efx->filter_lock);
	for (filter_idx = 0; filter_idx < HUNT_FILTER_TBL_ROWS; filter_idx++) {
		spec = efx_ef10_filter_entry_spec(table, filter_idx);
		if (spec && spec->priority == priority) {
			if (count == size) {
				count = -EMSGSIZE;
				break;
			}
			buf[count++] =
				efx_ef10_make_filter_id(
					efx_ef10_filter_pri(table, spec),
					filter_idx);
		}
	}
	spin_unlock_bh(&efx->filter_lock);
	return count;
}

#if (defined(EFX_NOT_UPSTREAM) && defined(EFX_USE_SARFS)) || defined(CONFIG_RFS_ACCEL)

static efx_mcdi_async_completer efx_ef10_filter_async_insert_complete;

static s32 efx_ef10_filter_async_insert(struct efx_nic *efx,
					const struct efx_filter_spec *spec)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	MCDI_DECLARE_BUF(inbuf, MC_CMD_FILTER_OP_EXT_IN_LEN);
	struct efx_filter_spec *saved_spec;
	unsigned int hash, i, depth = 1;
	bool replacing = false;
	int ins_index = -1;
	u64 cookie;
	s32 rc;

	/* Must be an RX filter without RSS and not for a multicast
	 * destination address (RFS only works for connected sockets).
	 * These restrictions allow us to pass only a tiny amount of
	 * data through to the completion function.
	 */
	EFX_WARN_ON_PARANOID(spec->flags !=
			     (EFX_FILTER_FLAG_RX | EFX_FILTER_FLAG_RX_SCATTER));
	EFX_WARN_ON_PARANOID(spec->priority > EFX_FILTER_PRI_HINT);
	EFX_WARN_ON_PARANOID(efx_filter_is_mc_recipient(spec));

	rc = efx_ef10_filter_pri(efx->filter_state, spec);
	if (rc < 0)
		return rc;

	hash = efx_ef10_filter_hash(spec);

	spin_lock_bh(&efx->filter_lock);

#ifdef EFX_NOT_UPSTREAM
	if (table->kernel_blocked[EFX_DL_FILTER_BLOCK_KERNEL_UCAST]) {
		rc = -EPERM;
		goto fail_unlock;
	}
#endif

	/* Find any existing filter with the same match tuple or else
	 * a free slot to insert at.  If an existing filter is busy,
	 * we have to give up.
	 */
	for (;;) {
		i = (hash + depth) & (HUNT_FILTER_TBL_ROWS - 1);
		saved_spec = efx_ef10_filter_entry_spec(table, i);

		if (!saved_spec) {
			if (ins_index < 0)
				ins_index = i;
		} else if (efx_ef10_filter_equal(spec, saved_spec)) {
			if (table->entry[i].spec & EFX_EF10_FILTER_FLAG_BUSY) {
				rc = -EBUSY;
				goto fail_unlock;
			}
			if (spec->priority < saved_spec->priority) {
				rc = -EPERM;
				goto fail_unlock;
			}
			ins_index = i;
			break;
		}

		/* Once we reach the maximum search depth, use the
		 * first suitable slot or return -EBUSY if there was
		 * none
		 */
		if (depth == EFX_EF10_FILTER_SEARCH_LIMIT) {
			if (ins_index < 0) {
				rc = -EBUSY;
				goto fail_unlock;
			}
			break;
		}

		++depth;
	}

	/* Create a software table entry if necessary, and mark it
	 * busy.  We might yet fail to insert, but any attempt to
	 * insert a conflicting filter while we're waiting for the
	 * firmware must find the busy entry.
	 */
	saved_spec = efx_ef10_filter_entry_spec(table, ins_index);
	if (saved_spec) {
		replacing = true;
	} else {
		saved_spec = kmalloc(sizeof(*spec), GFP_ATOMIC);
		if (!saved_spec) {
			rc = -ENOMEM;
			goto fail_unlock;
		}
		*saved_spec = *spec;
	}
	efx_ef10_filter_set_entry(table, ins_index, saved_spec,
				  EFX_EF10_FILTER_FLAG_BUSY);

	spin_unlock_bh(&efx->filter_lock);

	/* Pack up the variables needed on completion */
	BUILD_BUG_ON(EFX_FILTER_PRI_HINT > 1);
	BUILD_BUG_ON(HUNT_FILTER_TBL_ROWS > 0x2000);
	cookie = replacing << 31 | spec->priority << 30 | ins_index << 16 |
		 spec->dmaq_id;

	efx_ef10_filter_push_prep(efx, spec, inbuf,
				  table->entry[ins_index].handle, replacing);
	efx_mcdi_rpc_async(efx, MC_CMD_FILTER_OP, inbuf, sizeof(inbuf),
			   MC_CMD_FILTER_OP_OUT_LEN,
			   efx_ef10_filter_async_insert_complete, cookie);

	return ins_index;

fail_unlock:
	spin_unlock_bh(&efx->filter_lock);
	return rc;
}

static void
efx_ef10_filter_async_insert_complete(struct efx_nic *efx, unsigned long cookie,
				      int rc, efx_dword_t *outbuf,
				      size_t outlen_actual)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	unsigned int ins_index, dmaq_id;
	struct efx_filter_spec *spec;
	bool replacing;
	enum efx_filter_priority priority;

	/* Unpack the cookie */
	replacing = cookie >> 31;
	priority = (cookie >> 30) & 1;
	ins_index = (cookie >> 16) & (HUNT_FILTER_TBL_ROWS - 1);
	dmaq_id = cookie & 0xffff;

	spin_lock_bh(&efx->filter_lock);
	spec = efx_ef10_filter_entry_spec(table, ins_index);
	if (rc == 0) {
		table->entry[ins_index].handle =
			MCDI_QWORD(outbuf, FILTER_OP_OUT_HANDLE);
		if (replacing) {
			spec->priority = priority;
			spec->dmaq_id = dmaq_id;
		}
	} else if (!replacing) {
		kfree(spec);
		spec = NULL;
	}
	efx_ef10_filter_set_entry(table, ins_index, spec, 0);
	spin_unlock_bh(&efx->filter_lock);

	wake_up_all(&table->waitq);
}

static void
efx_ef10_filter_async_remove_complete(struct efx_nic *efx,
				      unsigned long filter_idx,
				      int rc, efx_dword_t *outbuf,
				      size_t outlen_actual);

/* Asynchronously remove a filter by index.
 * Will only remove filters or HINT priority or lower, since those are the
 * only ones we asynchronously insert.
 */
static int efx_ef10_filter_async_remove(struct efx_nic *efx,
					unsigned int filter_idx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_filter_spec *spec =
		efx_ef10_filter_entry_spec(table, filter_idx);
	int rc;

	MCDI_DECLARE_BUF(inbuf, MC_CMD_FILTER_OP_IN_LEN);

	if (!spec || spec->priority > EFX_FILTER_PRI_HINT)
		return -ENOENT;

	if (table->entry[filter_idx].spec & EFX_EF10_FILTER_FLAG_BUSY)
		return -EBUSY;

	MCDI_SET_DWORD(inbuf, FILTER_OP_IN_OP,
			MC_CMD_FILTER_OP_IN_OP_REMOVE);
	MCDI_SET_QWORD(inbuf, FILTER_OP_IN_HANDLE,
			table->entry[filter_idx].handle);
	rc = efx_mcdi_rpc_async(efx, MC_CMD_FILTER_OP, inbuf, sizeof(inbuf), 0,
				efx_ef10_filter_async_remove_complete,
				filter_idx);
	if (rc)
		return rc;

	table->entry[filter_idx].spec |= EFX_EF10_FILTER_FLAG_BUSY;
	return 0;
}

#ifdef CONFIG_RFS_ACCEL
static bool efx_ef10_filter_rfs_expire_one(struct efx_nic *efx, u32 flow_id,
					   unsigned int filter_idx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_filter_spec *spec =
		efx_ef10_filter_entry_spec(table, filter_idx);

	if (!spec)
		return false;

	WARN_ON(spec->priority != EFX_FILTER_PRI_HINT);

	if ((table->entry[filter_idx].spec & EFX_EF10_FILTER_FLAG_BUSY) ||
	    spec->priority != EFX_FILTER_PRI_HINT ||
	    !rps_may_expire_flow(efx->net_dev, spec->dmaq_id,
				 flow_id, filter_idx))
		return false;

	return efx_ef10_filter_async_remove(efx, filter_idx) == 0;
}
#endif /* CONFIG_RFS_ACCEL */

static void
efx_ef10_filter_async_remove_complete(struct efx_nic *efx,
				      unsigned long filter_idx,
				      int rc, efx_dword_t *outbuf,
				      size_t outlen_actual)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_filter_spec *spec =
		efx_ef10_filter_entry_spec(table, filter_idx);

	spin_lock_bh(&efx->filter_lock);
	if (rc == 0) {
		kfree(spec);
		efx_ef10_filter_set_entry(table, filter_idx, NULL, 0);
	}
	table->entry[filter_idx].spec &= ~EFX_EF10_FILTER_FLAG_BUSY;
	wake_up_all(&table->waitq);
	spin_unlock_bh(&efx->filter_lock);
}

#endif /* CONFIG_RFS_ACCEL || EFX_USE_SARFS */

static int efx_ef10_filter_match_flags_from_mcdi(bool encap, u32 mcdi_flags)
{
	int match_flags = 0;

#define MAP_FLAG(gen_flag, mcdi_field) do {				\
		u32 old_mcdi_flags = mcdi_flags;			\
		mcdi_flags &= ~(1 << MC_CMD_FILTER_OP_EXT_IN_MATCH_ ##	\
				     mcdi_field ## _LBN);		\
		if (mcdi_flags != old_mcdi_flags)			\
			match_flags |= EFX_FILTER_MATCH_ ## gen_flag;	\
	} while (0)

	if (encap) {
		/* encap filters must specify encap type */
		match_flags |= EFX_FILTER_MATCH_ENCAP_TYPE;
		/* and imply ethertype and ip proto */
		mcdi_flags &=
			~(1 << MC_CMD_FILTER_OP_EXT_IN_MATCH_IP_PROTO_LBN);
		mcdi_flags &=
			~(1 << MC_CMD_FILTER_OP_EXT_IN_MATCH_ETHER_TYPE_LBN);
		/* VNI/VSID, VLAN and outer MAC refer to the outer packet */
		MAP_FLAG(ENCAP_TNI, VNI_OR_VSID);
		MAP_FLAG(INNER_VID, INNER_VLAN);
		MAP_FLAG(OUTER_VID, OUTER_VLAN);
		MAP_FLAG(OUTER_LOC_MAC, DST_MAC);
		/* everything else refers to the inner packet */
		MAP_FLAG(LOC_MAC_IG, IFRM_UNKNOWN_UCAST_DST);
		MAP_FLAG(LOC_MAC_IG, IFRM_UNKNOWN_MCAST_DST);
		MAP_FLAG(REM_HOST, IFRM_SRC_IP);
		MAP_FLAG(LOC_HOST, IFRM_DST_IP);
		MAP_FLAG(REM_MAC, IFRM_SRC_MAC);
		MAP_FLAG(REM_PORT, IFRM_SRC_PORT);
		MAP_FLAG(LOC_MAC, IFRM_DST_MAC);
		MAP_FLAG(LOC_PORT, IFRM_DST_PORT);
		MAP_FLAG(ETHER_TYPE, IFRM_ETHER_TYPE);
		MAP_FLAG(IP_PROTO, IFRM_IP_PROTO);
	} else {
		MAP_FLAG(LOC_MAC_IG, UNKNOWN_UCAST_DST);
		MAP_FLAG(LOC_MAC_IG, UNKNOWN_MCAST_DST);
		MAP_FLAG(REM_HOST, SRC_IP);
		MAP_FLAG(LOC_HOST, DST_IP);
		MAP_FLAG(REM_MAC, SRC_MAC);
		MAP_FLAG(REM_PORT, SRC_PORT);
		MAP_FLAG(LOC_MAC, DST_MAC);
		MAP_FLAG(LOC_PORT, DST_PORT);
		MAP_FLAG(ETHER_TYPE, ETHER_TYPE);
		MAP_FLAG(INNER_VID, INNER_VLAN);
		MAP_FLAG(OUTER_VID, OUTER_VLAN);
		MAP_FLAG(IP_PROTO, IP_PROTO);
	}
#undef MAP_FLAG

	/* Did we map them all? */
	if (mcdi_flags)
		return -EINVAL;

	return match_flags;
}

static void efx_ef10_filter_cleanup_vlans(struct efx_nic *efx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_ef10_filter_vlan *vlan, *next_vlan;

	efx_rwsem_assert_write_locked(&efx->filter_sem);

	list_for_each_entry_safe(vlan, next_vlan, &table->vlan_list, list)
		efx_ef10_filter_del_vlan_internal(efx, vlan);
}

static bool efx_ef10_filter_match_supported(struct efx_nic *efx,
					    bool encap,
					    unsigned int match_flags)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	unsigned int match_pri;
	int mf;

	if (!table) {
		netif_warn(efx, drv, efx->net_dev,
			   "filter match support check before probe\n");
		return false;
	}

	for (match_pri = 0;
	     match_pri < table->rx_match_count;
	     match_pri++) {
		mf = efx_ef10_filter_match_flags_from_mcdi(encap,
				table->rx_match_mcdi_flags[match_pri]);
		if (mf == match_flags)
			return true;
	}

	return false;
}

static int
efx_ef10_filter_table_probe_matches(struct efx_nic *efx,
				    struct efx_ef10_filter_table *table,
				    bool encap)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_GET_PARSER_DISP_INFO_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_GET_PARSER_DISP_INFO_OUT_LENMAX);
	unsigned int pd_match_pri, pd_match_count;
	size_t outlen;
	int rc;

	/* Find out which RX filter types are supported, and their priorities */
	MCDI_SET_DWORD(inbuf, GET_PARSER_DISP_INFO_IN_OP,
		       encap ?
		       MC_CMD_GET_PARSER_DISP_INFO_IN_OP_GET_SUPPORTED_ENCAP_RX_MATCHES :
		       MC_CMD_GET_PARSER_DISP_INFO_IN_OP_GET_SUPPORTED_RX_MATCHES);
	rc = efx_mcdi_rpc(efx, MC_CMD_GET_PARSER_DISP_INFO,
			  inbuf, sizeof(inbuf), outbuf, sizeof(outbuf),
			  &outlen);
	if (rc)
		return rc;

	pd_match_count = MCDI_VAR_ARRAY_LEN(
		outlen, GET_PARSER_DISP_INFO_OUT_SUPPORTED_MATCHES);

	if (pd_match_count == 0)
		netif_warn(efx, probe, efx->net_dev, "%s: pd_match_count = 0\n",
				__func__);

	for (pd_match_pri = 0; pd_match_pri < pd_match_count; pd_match_pri++) {
		u32 mcdi_flags =
			MCDI_ARRAY_DWORD(
				outbuf,
				GET_PARSER_DISP_INFO_OUT_SUPPORTED_MATCHES,
				pd_match_pri);
		rc = efx_ef10_filter_match_flags_from_mcdi(encap, mcdi_flags);
		if (rc < 0) {
			netif_dbg(efx, probe, efx->net_dev,
					"%s: fw flags %#x pri %u not supported in driver\n",
					__func__, mcdi_flags, pd_match_pri);
		} else {
			netif_dbg(efx, probe, efx->net_dev,
					"%s: fw flags %#x pri %u supported as driver flags %#x pri %u\n",
					__func__, mcdi_flags, pd_match_pri,
					rc, table->rx_match_count);
			table->rx_match_mcdi_flags[table->rx_match_count++] =
				mcdi_flags;
		}
	}

	return 0;
}

static int efx_ef10_filter_table_probe_supported_filters(struct efx_nic *efx,
					struct efx_ef10_filter_table *table)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	int rc;

	table->rx_match_count = 0;

	rc = efx_ef10_filter_table_probe_matches(efx, table, false);

	if (!rc && nic_data->datapath_caps &
		   (1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN)) {
		rc = efx_ef10_filter_table_probe_matches(efx, table, true);
	}

	return rc;
}

static int efx_ef10_filter_table_probe(struct efx_nic *efx)
{
	struct net_device *net_dev  = efx->net_dev;
	struct efx_ef10_filter_table *table;
	int rc;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_ef10_vlan *vlan;

	efx_rwsem_assert_write_locked(&efx->filter_sem);

	if (efx->filter_state) /* already probed */
		return 0;

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	rc = efx_ef10_filter_table_probe_supported_filters(efx, table);
	if (rc)
		goto fail;

	table->entry = vzalloc(HUNT_FILTER_TBL_ROWS * sizeof(*table->entry));
	if (!table->entry) {
		rc = -ENOMEM;
		goto fail;
	}

	table->mc_promisc_last = false;
	table->vlan_filter =
		!!(efx->net_dev->features & NETIF_F_HW_VLAN_CTAG_FILTER);
	INIT_LIST_HEAD(&table->vlan_list);

	efx->filter_state = table;
	init_waitqueue_head(&table->waitq);

	list_for_each_entry(vlan, &nic_data->vlan_list, list) {
		rc = efx_ef10_filter_add_vlan(efx, vlan->vid);
		if (rc)
			goto fail_add_vlan;
	}

#ifdef CONFIG_SFC_DEBUGFS
	efx_extend_debugfs_port(efx, efx, 0, filter_debugfs);
	efx_extend_debugfs_port(efx, efx->net_dev, 0, netdev_debugfs);
#endif

	if (!efx_ef10_filter_match_supported(efx, false,
					     EFX_FILTER_MATCH_FLAGS_RFS)) {
		netif_info(efx, probe, efx->net_dev,
			   "RFS filters are not supported in this firmware variant\n");
		efx->net_dev->features &= ~NETIF_F_NTUPLE;
	}
	if ((efx_supported_features(efx) & NETIF_F_HW_VLAN_CTAG_FILTER) &&
	    !(efx_ef10_filter_match_supported(efx, false,
		(EFX_FILTER_MATCH_OUTER_VID | EFX_FILTER_MATCH_LOC_MAC)) &&
	      efx_ef10_filter_match_supported(efx, false,
		(EFX_FILTER_MATCH_OUTER_VID | EFX_FILTER_MATCH_LOC_MAC_IG)))) {

		netif_info(efx, probe, net_dev,
			   "VLAN filters are not supported in this firmware variant\n");
		net_dev->features &= ~NETIF_F_HW_VLAN_CTAG_FILTER;
		efx->fixed_features &= ~NETIF_F_HW_VLAN_CTAG_FILTER;
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NETDEV_HW_FEATURES)
		net_dev->hw_features &= ~NETIF_F_HW_VLAN_CTAG_FILTER;
#elif defined(EFX_HAVE_NETDEV_EXTENDED_HW_FEATURES)
		netdev_extended(net_dev)->hw_features &=
			~NETIF_F_HW_VLAN_CTAG_FILTER;
#else
		efx->hw_features &= ~NETIF_F_HW_VLAN_CTAG_FILTER;
#endif
	}

	return 0;

fail_add_vlan:
	efx_ef10_filter_cleanup_vlans(efx);
	efx->filter_state = NULL;
fail:
	kfree(table);
	return rc;
}

/* Caller must hold efx->filter_sem for read if race against
 * efx_ef10_filter_table_remove() is possible
 */
static void efx_ef10_filter_table_restore(struct efx_nic *efx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_ef10_filter_vlan *vlan;
	unsigned int invalid_filters = 0;
	struct efx_filter_spec *spec;
	unsigned int filter_idx;
	bool failed = false;
	u32 mcdi_flags;
	int match_pri;
	int rc, i;

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_RWSEM_IS_LOCKED)
	WARN_ON(!rwsem_is_locked(&efx->filter_sem));
#endif

	if (!nic_data->must_restore_filters)
		return;

	if (!table)
		return;

	spin_lock_bh(&efx->filter_lock);

	/* remove any filters that are no longer supported */
	for (filter_idx = 0; filter_idx < HUNT_FILTER_TBL_ROWS; filter_idx++) {
		spec = efx_ef10_filter_entry_spec(table, filter_idx);
		if (!spec)
			continue;

		mcdi_flags = efx_ef10_filter_mcdi_flags_from_spec(spec);
		match_pri = 0;
		while (match_pri < table->rx_match_count &&
		       table->rx_match_mcdi_flags[match_pri] != mcdi_flags)
			++match_pri;
		if (match_pri < table->rx_match_count)
			continue;

		list_for_each_entry(vlan, &table->vlan_list, list)
			for (i = 0; i < EFX_EF10_NUM_DEFAULT_FILTERS; ++i)
				if (vlan->default_filters[i] == filter_idx)
					vlan->default_filters[i] =
						EFX_EF10_FILTER_ID_INVALID;

		kfree(spec);
		efx_ef10_filter_set_entry(table, filter_idx, NULL, 0);
		++invalid_filters;
	}
	/* This can happen validly if the MC's capabilities have changed, so
	 * is not an error.
	 */
	if (invalid_filters)
		netif_dbg(efx, drv, efx->net_dev,
			  "Did not restore %u filters that are now unsupported.\n",
			  invalid_filters);

	/* re-add filters in priority order */
	for (match_pri=(table->rx_match_count)-1; match_pri >= 0; match_pri--) {
		for (filter_idx = 0; filter_idx < HUNT_FILTER_TBL_ROWS; filter_idx++) {
			spec = efx_ef10_filter_entry_spec(table, filter_idx);
			if (!spec)
				continue;

			mcdi_flags = efx_ef10_filter_mcdi_flags_from_spec(spec);
			if (mcdi_flags != table->rx_match_mcdi_flags[match_pri])
				continue;

			if (spec->rss_context != EFX_FILTER_RSS_CONTEXT_DEFAULT &&
			    spec->rss_context != nic_data->rx_rss_context)
				netif_warn(efx, drv, efx->net_dev, "Warning: "
					   "unable to restore a filter with "
					   "specific RSS context.\n");

			table->entry[filter_idx].spec |= EFX_EF10_FILTER_FLAG_BUSY;
			spin_unlock_bh(&efx->filter_lock);

			rc = efx_ef10_filter_push(efx, spec,
						  &table->entry[filter_idx].handle,
						  false);
			if (rc)
				failed = true;

			spin_lock_bh(&efx->filter_lock);
			if (rc) {
				kfree(spec);
				efx_ef10_filter_set_entry(table, filter_idx, NULL, 0);
			} else {
				table->entry[filter_idx].spec &=
					~EFX_EF10_FILTER_FLAG_BUSY;
			}
		}
	}

	spin_unlock_bh(&efx->filter_lock);

	if (failed)
		netif_err(efx, hw, efx->net_dev,
			  "unable to restore all filters\n");
	else
		nic_data->must_restore_filters = false;

	wake_up_all(&table->waitq);
}

static void efx_ef10_filter_table_remove(struct efx_nic *efx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	MCDI_DECLARE_BUF(inbuf, MC_CMD_FILTER_OP_IN_LEN);
	struct efx_filter_spec *spec;
	unsigned int filter_idx;
	int rc;

	efx_rwsem_assert_write_locked(&efx->filter_sem);

	if (!table)
		return;

#ifdef CONFIG_SFC_DEBUGFS
	efx_trim_debugfs_port(efx, netdev_debugfs);
	efx_trim_debugfs_port(efx, filter_debugfs);
#endif

	efx_ef10_filter_cleanup_vlans(efx);

	efx->filter_state = NULL;

	for (filter_idx = 0; filter_idx < HUNT_FILTER_TBL_ROWS; filter_idx++) {
		spec = efx_ef10_filter_entry_spec(table, filter_idx);
		if (!spec)
			continue;

		MCDI_SET_DWORD(inbuf, FILTER_OP_IN_OP,
			       efx_ef10_filter_is_exclusive(spec) ?
			       MC_CMD_FILTER_OP_IN_OP_REMOVE :
			       MC_CMD_FILTER_OP_IN_OP_UNSUBSCRIBE);
		MCDI_SET_QWORD(inbuf, FILTER_OP_IN_HANDLE,
			       table->entry[filter_idx].handle);
		rc = efx_mcdi_rpc_quiet(efx, MC_CMD_FILTER_OP, inbuf,
				sizeof(inbuf), NULL, 0, NULL);
		if (rc && (rc != -ENETDOWN))
			netif_info(efx, drv, efx->net_dev,
					"%s: filter %04x remove failed %d\n",
					__func__, filter_idx, rc);
		kfree(spec);
	}

	vfree(table->entry);
	kfree(table);
}

static void efx_ef10_filter_mark_one_old(struct efx_nic *efx, uint16_t *id)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	unsigned int filter_idx;

	if (*id != EFX_EF10_FILTER_ID_INVALID) {
		filter_idx = efx_ef10_filter_get_unsafe_id(efx, *id);
		if (!table->entry[filter_idx].spec)
			netif_dbg(efx, drv, efx->net_dev,
				"%s: marked null spec old %04x:%04x\n",
				__func__, *id, filter_idx);
		table->entry[filter_idx].spec |= EFX_EF10_FILTER_FLAG_AUTO_OLD;
		*id = EFX_EF10_FILTER_ID_INVALID;
	}
}

/* Mark old per-VLAN filters that may need to be removed */
static void _efx_ef10_filter_vlan_mark_old(struct efx_nic *efx,
					   struct efx_ef10_filter_vlan *vlan)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	unsigned int i;

	WARN_ON(!mutex_is_locked(&efx->mac_lock));

	for (i = 0; i < table->dev_uc_count; i++)
		efx_ef10_filter_mark_one_old(efx, &vlan->uc[i]);
	for (i = 0; i < table->dev_mc_count; i++)
		efx_ef10_filter_mark_one_old(efx, &vlan->mc[i]);
	for (i = 0; i < EFX_EF10_NUM_DEFAULT_FILTERS; i++)
		efx_ef10_filter_mark_one_old(efx, &vlan->default_filters[i]);
}

/* Mark old filters that may need to be removed.
 * Caller must hold efx->filter_sem for read if race against
 * efx_ef10_filter_table_remove() is possible
 */
static void efx_ef10_filter_mark_old(struct efx_nic *efx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_ef10_filter_vlan *vlan;

	spin_lock_bh(&efx->filter_lock);
	list_for_each_entry(vlan, &table->vlan_list, list)
		_efx_ef10_filter_vlan_mark_old(efx, vlan);
	spin_unlock_bh(&efx->filter_lock);
}

static void efx_ef10_filter_uc_addr_list(struct efx_nic *efx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct net_device *net_dev = efx->net_dev;
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_DEVICE_UC)
	struct netdev_hw_addr *uc;
#else
	struct dev_addr_list *uc;
#endif
	int addr_count;
	unsigned int i;

	WARN_ON(!mutex_is_locked(&efx->mac_lock));

#ifdef EFX_NOT_UPSTREAM
	if (table->kernel_blocked[EFX_DL_FILTER_BLOCK_KERNEL_UCAST]) {
		table->dev_uc_count = 0;
		return;
	}
#endif
	addr_count = netdev_uc_count(net_dev);
	table->uc_promisc = !!(net_dev->flags & IFF_PROMISC);
	table->dev_uc_count = 1 + addr_count;
	ether_addr_copy(table->dev_uc_list[0].addr, net_dev->dev_addr);
	i = 1;
	netdev_for_each_uc_addr(uc, net_dev) {
		if (i >= EFX_EF10_FILTER_DEV_UC_MAX) {
			table->uc_promisc = true;
			break;
		}
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_DEVICE_UC)
		ether_addr_copy(table->dev_uc_list[i].addr, uc->addr);
#else
		ether_addr_copy(table->dev_uc_list[i].addr, uc->da_addr);
#endif
		i++;
	}
}

static void efx_ef10_filter_mc_addr_list(struct efx_nic *efx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct net_device *net_dev = efx->net_dev;
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_DEVICE_MC)
	struct netdev_hw_addr *mc;
#else
	struct dev_mc_list *mc;
#endif
	unsigned int i, addr_count;

	WARN_ON(!mutex_is_locked(&efx->mac_lock));

	table->mc_overflow = false;

#ifdef EFX_NOT_UPSTREAM
	if (table->kernel_blocked[EFX_DL_FILTER_BLOCK_KERNEL_MCAST]) {
		table->dev_mc_count = 0;
		return;
	}
#endif
	table->mc_promisc = !!(net_dev->flags & (IFF_PROMISC | IFF_ALLMULTI));

	addr_count = netdev_mc_count(net_dev);
	i = 0;
	netdev_for_each_mc_addr(mc, net_dev) {
		if (i >= EFX_EF10_FILTER_DEV_MC_MAX) {
			table->mc_promisc = true;
			table->mc_overflow = true;
			break;
		}
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_DEVICE_MC)
		ether_addr_copy(table->dev_mc_list[i].addr, mc->addr);
#else
		ether_addr_copy(table->dev_mc_list[i].addr, mc->dmi_addr);
#endif
		i++;
	}

	table->dev_mc_count = i;
}

static int efx_ef10_filter_insert_addr_list(struct efx_nic *efx,
					    struct efx_ef10_filter_vlan *vlan,
					    bool multicast, bool rollback)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_ef10_dev_addr *addr_list;
	u16 *ids;
	enum efx_filter_flags filter_flags;
	struct efx_filter_spec spec;
	u8 baddr[ETH_ALEN];
	unsigned int i, j;
	int addr_count;
	int rc;

	WARN_ON(!mutex_is_locked(&efx->mac_lock));

	if (multicast) {
		addr_list = table->dev_mc_list;
		addr_count = table->dev_mc_count;
		ids = vlan->mc;
	} else {
		addr_list = table->dev_uc_list;
		addr_count = table->dev_uc_count;
		ids = vlan->uc;
	}

	filter_flags = efx_rss_enabled(efx) ? EFX_FILTER_FLAG_RX_RSS : 0;

	/* Insert/renew filters */
	for (i = 0; i < addr_count; i++) {
		EFX_BUG_ON_PARANOID(ids[i] != EFX_EF10_FILTER_ID_INVALID);
		efx_filter_init_rx(&spec, EFX_FILTER_PRI_AUTO, filter_flags, 0);
		efx_filter_set_eth_local(&spec, vlan->vid, addr_list[i].addr);
		rc = efx_ef10_filter_insert(efx, &spec, true);
		if (rc < 0) {
			if (rollback) {
				netif_info(efx, drv, efx->net_dev,
					   "efx_ef10_filter_insert failed rc=%d\n",
					   rc);
				/* Fall back to promiscuous */
				for (j = 0; j < i; j++) {
					if (ids[j] == EFX_EF10_FILTER_ID_INVALID)
						continue;
					efx_ef10_filter_remove_unsafe(
						efx, EFX_FILTER_PRI_AUTO,
						ids[j]);
					ids[j] = EFX_EF10_FILTER_ID_INVALID;
				}
				return rc;
			}
			/* Keep invalid ID and continue */
		} else {
			ids[i] = efx_ef10_filter_get_unsafe_id(efx, rc);
		}
	}

#ifdef EFX_NOT_UPSTREAM
	if (!table->kernel_blocked[EFX_DL_FILTER_BLOCK_KERNEL_MCAST])
#endif
	if (multicast && rollback) {
		/* Also need an Ethernet broadcast filter */
		EFX_BUG_ON_PARANOID(vlan->default_filters[EFX_EF10_BCAST] !=
				    EFX_EF10_FILTER_ID_INVALID);
		efx_filter_init_rx(&spec, EFX_FILTER_PRI_AUTO, filter_flags, 0);
		eth_broadcast_addr(baddr);
		efx_filter_set_eth_local(&spec, vlan->vid, baddr);
		rc = efx_ef10_filter_insert(efx, &spec, true);
		if (rc < 0) {
			netif_warn(efx, drv, efx->net_dev,
				   "Broadcast filter insert failed rc=%d\n", rc);
			/* Fall back to promiscuous */
			for (j = 0; j < i; j++) {
				if (ids[j] == EFX_EF10_FILTER_ID_INVALID)
					continue;
				efx_ef10_filter_remove_unsafe(
					efx, EFX_FILTER_PRI_AUTO,
					ids[j]);
				ids[j] = EFX_EF10_FILTER_ID_INVALID;
			}
			return rc;
		} else {
			vlan->default_filters[EFX_EF10_BCAST] =
				efx_ef10_filter_get_unsafe_id(efx, rc);
		}
	}

	return 0;
}

static int efx_ef10_filter_insert_def(struct efx_nic *efx,
				      struct efx_ef10_filter_vlan *vlan,
				      enum efx_encap_type encap_type,
				      bool multicast, bool rollback)
{
#ifdef EFX_NOT_UPSTREAM
	struct efx_ef10_filter_table *table = efx->filter_state;
#endif
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	enum efx_filter_flags filter_flags;
	struct efx_filter_spec spec;
	u8 baddr[ETH_ALEN];
	int rc;
	u16 *id;

#ifdef EFX_NOT_UPSTREAM
	if (table->kernel_blocked[multicast ? EFX_DL_FILTER_BLOCK_KERNEL_MCAST :
					      EFX_DL_FILTER_BLOCK_KERNEL_UCAST])
		return 0;
#endif

	filter_flags = efx_rss_enabled(efx) ? EFX_FILTER_FLAG_RX_RSS : 0;

	efx_filter_init_rx(&spec, EFX_FILTER_PRI_AUTO, filter_flags, 0);

	if (multicast)
		efx_filter_set_mc_def(&spec);
	else
		efx_filter_set_uc_def(&spec);

	if (encap_type) {
		if (nic_data->datapath_caps &
		    (1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN))
			efx_filter_set_encap_type(&spec, encap_type);
		else
			/* don't insert encap filters on non-supporting
			 * platforms. ID will be left as INVALID.
			 */
			return 0;
	}

	if (vlan->vid != EFX_FILTER_VID_UNSPEC)
		efx_filter_set_eth_local(&spec, vlan->vid, NULL);

	rc = efx_ef10_filter_insert(efx, &spec, true);
	if (rc < 0) {
		const char *um = multicast ? "Multicast" : "Unicast";
		const char *encap_name = "";
		const char *encap_ipv = "";

		if ((encap_type & EFX_ENCAP_TYPES_MASK) ==
		    EFX_ENCAP_TYPE_VXLAN)
			encap_name = "VXLAN ";
		else if ((encap_type & EFX_ENCAP_TYPES_MASK) ==
			 EFX_ENCAP_TYPE_NVGRE)
			encap_name = "NVGRE ";
		else if ((encap_type & EFX_ENCAP_TYPES_MASK) ==
			 EFX_ENCAP_TYPE_GENEVE)
			encap_name = "GENEVE ";
		if (encap_type & EFX_ENCAP_FLAG_IPV6)
			encap_ipv = "IPv6 ";
		else if (encap_type)
			encap_ipv = "IPv4 ";

		/* unprivileged functions can't insert mismatch filters
		 * for encapsulated or unicast traffic, so downgrade
		 * those warnings to debug.
		 */
		netif_cond_dbg(efx, drv, efx->net_dev,
			       rc == -EPERM && (encap_type || !multicast), warn,
			       "%s%s%s mismatch filter insert failed rc=%d\n",
			       encap_name, encap_ipv, um, rc);
	} else if (multicast) {
		/* mapping from encap types to default filter IDs (multicast) */
		static enum efx_ef10_default_filters map[] = {
			[EFX_ENCAP_TYPE_NONE] = EFX_EF10_MCDEF,
			[EFX_ENCAP_TYPE_VXLAN] = EFX_EF10_VXLAN4_MCDEF,
			[EFX_ENCAP_TYPE_NVGRE] = EFX_EF10_NVGRE4_MCDEF,
			[EFX_ENCAP_TYPE_GENEVE] = EFX_EF10_GENEVE4_MCDEF,
			[EFX_ENCAP_TYPE_VXLAN | EFX_ENCAP_FLAG_IPV6] =
				EFX_EF10_VXLAN6_MCDEF,
			[EFX_ENCAP_TYPE_NVGRE | EFX_ENCAP_FLAG_IPV6] =
				EFX_EF10_NVGRE6_MCDEF,
			[EFX_ENCAP_TYPE_GENEVE | EFX_ENCAP_FLAG_IPV6] =
				EFX_EF10_GENEVE6_MCDEF,
		};

		/* quick bounds check (BCAST result impossible) */
		BUILD_BUG_ON(EFX_EF10_BCAST != 0);
		if (encap_type >= ARRAY_SIZE(map) || map[encap_type] == 0) {
			WARN_ON(1);
			return -EINVAL;
		}
		/* then follow map */
		id = &vlan->default_filters[map[encap_type]];

		EFX_BUG_ON_PARANOID(*id != EFX_EF10_FILTER_ID_INVALID);
		*id = efx_ef10_filter_get_unsafe_id(efx, rc);
		if (!nic_data->workaround_26807 && !encap_type) {
			/* Also need an Ethernet broadcast filter */
			efx_filter_init_rx(&spec, EFX_FILTER_PRI_AUTO,
					   filter_flags, 0);
			eth_broadcast_addr(baddr);
			efx_filter_set_eth_local(&spec, vlan->vid, baddr);
			rc = efx_ef10_filter_insert(efx, &spec, true);
			if (rc < 0) {
				netif_warn(efx, drv, efx->net_dev,
					   "Broadcast filter insert failed rc=%d\n",
					   rc);
				if (rollback) {
					/* Roll back the mc_def filter */
					efx_ef10_filter_remove_unsafe(
							efx, EFX_FILTER_PRI_AUTO,
							*id);
					*id = EFX_EF10_FILTER_ID_INVALID;
					return rc;
				}
			} else {
				EFX_BUG_ON_PARANOID(
					vlan->default_filters[EFX_EF10_BCAST] !=
					EFX_EF10_FILTER_ID_INVALID);
				vlan->default_filters[EFX_EF10_BCAST] =
					efx_ef10_filter_get_unsafe_id(efx, rc);
			}
		}
		rc = 0;
	} else {
		/* mapping from encap types to default filter IDs (unicast) */
		static enum efx_ef10_default_filters map[] = {
			[EFX_ENCAP_TYPE_NONE] = EFX_EF10_UCDEF,
			[EFX_ENCAP_TYPE_VXLAN] = EFX_EF10_VXLAN4_UCDEF,
			[EFX_ENCAP_TYPE_NVGRE] = EFX_EF10_NVGRE4_UCDEF,
			[EFX_ENCAP_TYPE_GENEVE] = EFX_EF10_GENEVE4_UCDEF,
			[EFX_ENCAP_TYPE_VXLAN | EFX_ENCAP_FLAG_IPV6] =
				EFX_EF10_VXLAN6_UCDEF,
			[EFX_ENCAP_TYPE_NVGRE | EFX_ENCAP_FLAG_IPV6] =
				EFX_EF10_NVGRE6_UCDEF,
			[EFX_ENCAP_TYPE_GENEVE | EFX_ENCAP_FLAG_IPV6] =
				EFX_EF10_GENEVE6_UCDEF,
		};

		/* quick bounds check (BCAST result impossible) */
		BUILD_BUG_ON(EFX_EF10_BCAST != 0);
		if (encap_type >= ARRAY_SIZE(map) || map[encap_type] == 0) {
			WARN_ON(1);
			return -EINVAL;
		}
		/* then follow map */
		id = &vlan->default_filters[map[encap_type]];
		EFX_BUG_ON_PARANOID(*id != EFX_EF10_FILTER_ID_INVALID);
		*id = rc;
		rc = 0;
	}
	return rc;
}

/* Remove filters that weren't renewed.  Since nothing else changes the AUTO_OLD
 * flag or removes these filters, we don't need to hold the filter_lock while
 * scanning for these filters.
 */
static void efx_ef10_filter_remove_old(struct efx_nic *efx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	int remove_failed = 0;
	int remove_noent = 0;
	int rc;
	int i;

	for (i = 0; i < HUNT_FILTER_TBL_ROWS; i++) {
		if (ACCESS_ONCE(table->entry[i].spec) &
				EFX_EF10_FILTER_FLAG_AUTO_OLD) {
			rc = efx_ef10_filter_remove_internal(efx,
					1U << EFX_FILTER_PRI_AUTO, i, true);
			if (rc == -ENOENT)
				remove_noent++;
			else if (rc)
				remove_failed++;
		}
	}

	if (remove_failed)
		netif_info(efx, drv, efx->net_dev,
				"%s: failed to remove %d filters\n",
				__func__, remove_failed);
	if (remove_noent)
		netif_info(efx, drv, efx->net_dev,
				"%s: failed to remove %d non-existent filters\n",
				__func__, remove_noent);
}

static unsigned int efx_ef10_filter_vlan_count_filters(struct efx_nic *efx,
		struct efx_ef10_filter_vlan *vlan)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	unsigned int count = 0;
	unsigned int i;

	WARN_ON(!mutex_is_locked(&efx->mac_lock));

	for (i = 0; i < table->dev_uc_count; i++)
		count += (vlan->uc[i] != EFX_EF10_FILTER_ID_INVALID);
	for (i = 0; i < table->dev_mc_count; i++)
		count += (vlan->mc[i] != EFX_EF10_FILTER_ID_INVALID);
	for (i = 0; i < EFX_EF10_NUM_DEFAULT_FILTERS; i++)
		count += (vlan->default_filters[i] !=
			  EFX_EF10_FILTER_ID_INVALID);

	return count;
}

/* Caller must hold efx->filter_sem for read if race against
 * efx_ef10_filter_table_remove() is possible
 */
static void efx_ef10_filter_vlan_sync_rx_mode(struct efx_nic *efx,
					      struct efx_ef10_filter_vlan *vlan)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	unsigned int n_filters;

	WARN_ON(!mutex_is_locked(&efx->mac_lock));

	/* Do not install unspecified VID if VLAN filtering is enabled.
	 * Do not install all specified VIDs if VLAN filtering is disabled.
	 */
	if ((vlan->vid == EFX_FILTER_VID_UNSPEC) == table->vlan_filter)
		return;

	/* Insert/renew unicast filters */
	if (table->uc_promisc) {
		efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_NONE,
					   false, false);
		efx_ef10_filter_insert_addr_list(efx, vlan, false, false);
	} else {
		/* If any of the filters failed to insert, fall back to
		 * promiscuous mode - add in the uc_def filter.  But keep
		 * our individual unicast filters.
		 */
		if (efx_ef10_filter_insert_addr_list(efx, vlan, false, false))
			efx_ef10_filter_insert_def(efx, vlan,
						   EFX_ENCAP_TYPE_NONE,
						   false, false);
	}
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_VXLAN,
				   false, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_VXLAN |
					      EFX_ENCAP_FLAG_IPV6,
				   false, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_NVGRE,
				   false, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_NVGRE |
					      EFX_ENCAP_FLAG_IPV6,
				   false, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_GENEVE,
				   false, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_GENEVE |
					      EFX_ENCAP_FLAG_IPV6,
				   false, false);

	/* Insert/renew multicast filters */
	/* If changing promiscuous state with cascaded multicast filters, remove
	 * old filters first, so that packets are dropped rather than duplicated
	 */
	if (nic_data->workaround_26807 &&
	    table->mc_promisc_last != table->mc_promisc)
		efx_ef10_filter_remove_old(efx);
	if (table->mc_promisc) {
		if (nic_data->workaround_26807)
		{
			/* If we failed to insert promiscuous filters, rollback and
			 * fall back to individual multicast filters
			 */
			if (efx_ef10_filter_insert_def(efx, vlan,
						       EFX_ENCAP_TYPE_NONE,
						       true, true)) {
				/* Changing promisc state, so remove old filters */
				efx_ef10_filter_remove_old(efx);
				efx_ef10_filter_insert_addr_list(efx, vlan,
								 true, false);
			}
		} else {
			/* If we failed to insert promiscuous filters, don't
			 * rollback.  Regardless, also insert the mc_list,
			 * unless it's incomplete due to overflow
			 */
			efx_ef10_filter_insert_def(efx, vlan,
						   EFX_ENCAP_TYPE_NONE,
						   true, false);
			if (!table->mc_overflow)
				efx_ef10_filter_insert_addr_list(efx, vlan,
								 true, false);
		}
	} else {
		/* If any filters failed to insert, rollback and fall back to
		 * promiscuous mode - mc_def filter and maybe broadcast.  If
		 * that fails, roll back again and insert as many of our
		 * individual multicast filters as we can.
		 */
		if (efx_ef10_filter_insert_addr_list(efx, vlan, true, true)) {
			/* Changing promisc state, so remove old filters */
			if (nic_data->workaround_26807)
				efx_ef10_filter_remove_old(efx);
			if (efx_ef10_filter_insert_def(efx, vlan,
						       EFX_ENCAP_TYPE_NONE,
						       true, true))
				efx_ef10_filter_insert_addr_list(efx, vlan,
								 true, false);
		}
	}
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_VXLAN,
				   true, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_VXLAN |
					      EFX_ENCAP_FLAG_IPV6,
				   true, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_NVGRE,
				   true, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_NVGRE |
					      EFX_ENCAP_FLAG_IPV6,
				   true, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_GENEVE,
				   true, false);
	efx_ef10_filter_insert_def(efx, vlan, EFX_ENCAP_TYPE_GENEVE |
					      EFX_ENCAP_FLAG_IPV6,
				   true, false);

	n_filters = efx_ef10_filter_vlan_count_filters(efx, vlan);
#ifdef EFX_NOT_UPSTREAM
	if (n_filters == 0 && vlan->warn_on_zero_filters &&
	    !(table->kernel_blocked[EFX_DL_FILTER_BLOCK_KERNEL_UCAST] &&
	      table->kernel_blocked[EFX_DL_FILTER_BLOCK_KERNEL_MCAST])) {
#else
	if (n_filters == 0 && vlan->warn_on_zero_filters) {
#endif
		netif_warn(efx, drv, efx->net_dev,
			   "Cannot install VLAN %u filters\n",
			   vlan->vid);
		netif_warn(efx, drv, efx->net_dev,
			   "Maybe it is denied by hypervisor or network access control\n");
		vlan->warn_on_zero_filters = false;
	} else if (n_filters != 0 && !vlan->warn_on_zero_filters) {
		netif_warn(efx, drv, efx->net_dev,
			   "VLAN %u is now allowed\n",
			   vlan->vid);
		vlan->warn_on_zero_filters = true;
	}
}

/* Caller must hold efx->filter_sem for read if race against
 * efx_ef10_filter_table_remove() is possible
 */
static void efx_ef10_filter_sync_rx_mode(struct efx_nic *efx)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct net_device *net_dev = efx->net_dev;
	struct efx_ef10_filter_vlan *vlan;
	bool vlan_filter;

	WARN_ON(!mutex_is_locked(&efx->mac_lock));

	if (!efx_dev_registered(efx))
		return;

	if (!table)
		return;

	efx_ef10_filter_mark_old(efx);

	/* Copy/convert the address lists; add the primary station
	 * address and broadcast address
	 */
	netif_addr_lock_bh(net_dev);
	efx_ef10_filter_uc_addr_list(efx);
	efx_ef10_filter_mc_addr_list(efx);
	netif_addr_unlock_bh(net_dev);

	/* If VLAN filtering changes, all old filters are finally removed.
	 * Do it in advance to avoid conflicts for unicast untagged and
	 * VLAN 0 tagged filters.
	 */
	vlan_filter = !!(net_dev->features & NETIF_F_HW_VLAN_CTAG_FILTER);
	if (table->vlan_filter != vlan_filter) {
		table->vlan_filter = vlan_filter;
		efx_ef10_filter_remove_old(efx);
	}

	list_for_each_entry(vlan, &table->vlan_list, list)
		efx_ef10_filter_vlan_sync_rx_mode(efx, vlan);

	efx_ef10_filter_remove_old(efx);
	table->mc_promisc_last = table->mc_promisc;
}

static struct efx_ef10_filter_vlan *efx_ef10_filter_find_vlan(struct efx_nic *efx, u16 vid)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_ef10_filter_vlan *vlan;

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_RWSEM_IS_LOCKED)
	WARN_ON(!rwsem_is_locked(&efx->filter_sem));
#endif

	list_for_each_entry(vlan, &table->vlan_list, list) {
		if (vlan->vid == vid)
			return vlan;
	}

	return NULL;
}

static int efx_ef10_filter_add_vlan(struct efx_nic *efx, u16 vid)
{
	struct efx_ef10_filter_table *table = efx->filter_state;
	struct efx_ef10_filter_vlan *vlan;
	unsigned int i;

	efx_rwsem_assert_write_locked(&efx->filter_sem);

	vlan = efx_ef10_filter_find_vlan(efx, vid);
	if (WARN_ON(vlan)) {
		netif_err(efx, drv, efx->net_dev,
			  "VLAN %u already added\n", vid);
		return -EALREADY;
	}

	vlan = kzalloc(sizeof(*vlan), GFP_KERNEL);
	if (!vlan)
		return -ENOMEM;

	vlan->vid = vid;

	for (i = 0; i < ARRAY_SIZE(vlan->uc); i++)
		vlan->uc[i] = EFX_EF10_FILTER_ID_INVALID;
	for (i = 0; i < ARRAY_SIZE(vlan->mc); i++)
		vlan->mc[i] = EFX_EF10_FILTER_ID_INVALID;
	for (i = 0; i < EFX_EF10_NUM_DEFAULT_FILTERS; i++)
		vlan->default_filters[i] = EFX_EF10_FILTER_ID_INVALID;

	vlan->warn_on_zero_filters = true;

	list_add_tail(&vlan->list, &table->vlan_list);

	if (efx_dev_registered(efx))
		efx_ef10_filter_vlan_sync_rx_mode(efx, vlan);

	return 0;
}

static void efx_ef10_filter_del_vlan_internal(struct efx_nic *efx,
					      struct efx_ef10_filter_vlan *vlan)
{
	unsigned int i;

	efx_rwsem_assert_write_locked(&efx->filter_sem);

	list_del(&vlan->list);

	for (i = 0; i < ARRAY_SIZE(vlan->uc); i++) {
		if (vlan->uc[i] != EFX_EF10_FILTER_ID_INVALID)
			efx_ef10_filter_remove_unsafe(efx, EFX_FILTER_PRI_AUTO,
						      vlan->uc[i]);
	}
	for (i = 0; i < ARRAY_SIZE(vlan->mc); i++) {
		if (vlan->mc[i] != EFX_EF10_FILTER_ID_INVALID)
			efx_ef10_filter_remove_unsafe(efx, EFX_FILTER_PRI_AUTO,
						      vlan->mc[i]);
	}
	for (i = 0; i < EFX_EF10_NUM_DEFAULT_FILTERS; i++)
		if (vlan->default_filters[i] != EFX_EF10_FILTER_ID_INVALID)
			efx_ef10_filter_remove_unsafe(efx, EFX_FILTER_PRI_AUTO,
						      vlan->default_filters[i]);

	kfree(vlan);
}

static void efx_ef10_filter_del_vlan(struct efx_nic *efx, u16 vid)
{
	struct efx_ef10_filter_vlan *vlan;

	efx_rwsem_assert_write_locked(&efx->filter_sem);

	vlan = efx_ef10_filter_find_vlan(efx, vid);
	if (!vlan) {
		netif_err(efx, drv, efx->net_dev,
			  "VLAN %u not found in filter state\n", vid);
		return;
	}

	efx_ef10_filter_del_vlan_internal(efx, vlan);
}

#ifdef EFX_NOT_UPSTREAM
static int efx_ef10_filter_block_kernel(struct efx_nic *efx,
					enum efx_dl_filter_block_kernel_type
					type)
{
	struct efx_ef10_filter_table *table = efx->filter_state;

	mutex_lock(&efx->mac_lock);
	spin_lock_bh(&efx->filter_lock);
	table->kernel_blocked[type] = true;
	spin_unlock_bh(&efx->filter_lock);

	down_read(&efx->filter_sem);
	efx_ef10_filter_sync_rx_mode(efx);
	up_read(&efx->filter_sem);
	mutex_unlock(&efx->mac_lock);

	if (type == EFX_DL_FILTER_BLOCK_KERNEL_UCAST)
		return efx_ef10_filter_clear_rx(efx, EFX_FILTER_PRI_HINT);
	else
		return 0;
}

static void efx_ef10_filter_unblock_kernel(struct efx_nic *efx,
					   enum efx_dl_filter_block_kernel_type
					   type)
{
	struct efx_ef10_filter_table *table = efx->filter_state;

	mutex_lock(&efx->mac_lock);
	spin_lock_bh(&efx->filter_lock);
	table->kernel_blocked[type] = false;
	spin_unlock_bh(&efx->filter_lock);

	down_read(&efx->filter_sem);
	efx_ef10_filter_sync_rx_mode(efx);
	up_read(&efx->filter_sem);
	mutex_unlock(&efx->mac_lock);
}

static int efx_ef10_vport_filter_insert(struct efx_nic *efx,
					unsigned int vport_id,
					const struct efx_filter_spec *spec_in,
					u64 *filter_id_out,
					bool *is_exclusive_out)
{
	struct efx_filter_spec spec = *spec_in;
	efx_filter_set_vport_id(&spec, vport_id);
	*is_exclusive_out = efx_ef10_filter_is_exclusive(&spec);
	return efx_ef10_filter_push(efx, &spec, filter_id_out, false);
}

static int efx_ef10_vport_filter_remove(struct efx_nic *efx,
					unsigned int vport_id,
					u64 filter_id, bool is_exclusive)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_FILTER_OP_IN_LEN);
	MCDI_SET_DWORD(inbuf, FILTER_OP_IN_OP,
		       is_exclusive ?
		       MC_CMD_FILTER_OP_IN_OP_REMOVE :
		       MC_CMD_FILTER_OP_IN_OP_UNSUBSCRIBE);
	MCDI_SET_QWORD(inbuf, FILTER_OP_IN_HANDLE, filter_id);
	return efx_mcdi_rpc(efx, MC_CMD_FILTER_OP,
			    inbuf, sizeof(inbuf), NULL, 0, NULL);
}

#endif /* EFX_NOT_UPSTREAM */

static int efx_ef10_vport_set_mac_address(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	u8 mac_old[ETH_ALEN];
	int rc, rc2;

	/* Only reconfigure a PF-created vport */
	if (is_zero_ether_addr(nic_data->vport_mac))
		return 0;

	efx_device_detach_sync(efx);
	efx_net_stop(efx->net_dev);
	down_write(&efx->filter_sem);
	efx_ef10_filter_table_remove(efx);
	up_write(&efx->filter_sem);

	rc = efx_ef10_vadaptor_free(efx, nic_data->vport_id);
	if (rc)
		goto restore_filters;

	ether_addr_copy(mac_old, nic_data->vport_mac);
	rc = efx_ef10_vport_del_mac(efx, nic_data->vport_id,
				    nic_data->vport_mac);
	if (rc)
		goto restore_vadaptor;

	rc = efx_ef10_vport_add_mac(efx, nic_data->vport_id,
				    efx->net_dev->dev_addr);
	if (!rc) {
		ether_addr_copy(nic_data->vport_mac, efx->net_dev->dev_addr);
	} else {
		rc2 = efx_ef10_vport_add_mac(efx, nic_data->vport_id, mac_old);
		if (rc2) {
			/* Failed to add original MAC, so clear vport_mac */
			eth_zero_addr(nic_data->vport_mac);
			goto reset_nic;
		}
	}

restore_vadaptor:
	rc2 = efx_ef10_vadaptor_alloc(efx, nic_data->vport_id);
	if (rc2)
		goto reset_nic;
restore_filters:
	mutex_lock(&efx->mac_lock);
	down_write(&efx->filter_sem);
	rc2 = efx_ef10_filter_table_probe(efx);
	up_write(&efx->filter_sem);
	mutex_unlock(&efx->mac_lock);
	if (rc2)
		goto reset_nic;

	rc2 = efx_net_open(efx->net_dev);
	if (rc2)
		goto reset_nic;

	efx_device_attach_if_not_resetting(efx);

	return rc;

reset_nic:
	netif_err(efx, drv, efx->net_dev,
		  "Failed to restore when changing MAC address - scheduling reset\n");
	efx_schedule_reset(efx, RESET_TYPE_DATAPATH);

	return rc ? rc : rc2;
}

static int efx_ef10_set_mac_address(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_VADAPTOR_SET_MAC_IN_LEN);
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	bool was_enabled = efx->port_enabled;
	int rc;

	ether_addr_copy(MCDI_PTR(inbuf, VADAPTOR_SET_MAC_IN_MACADDR),
			efx->net_dev->dev_addr);
	MCDI_SET_DWORD(inbuf, VADAPTOR_SET_MAC_IN_UPSTREAM_PORT_ID,
		       nic_data->vport_id);

	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_VADAPTOR_SET_MAC, inbuf,
				sizeof(inbuf), NULL, 0, NULL);

	if (!rc) {
		down_write(&efx->filter_sem);
		efx_ef10_filter_table_restore(efx);
		up_write(&efx->filter_sem);
	} else if (rc == -EBUSY) {

		/* VADAPTOR_SET_MAC without tearing down queues and
		 * filters not allowed by firmware, try again after
		 * tearing down the queues and filters. This path will
		 * fail in the presence on Onload stack
		 */

		efx_device_detach_sync(efx);
		efx_net_stop(efx->net_dev);

		mutex_lock(&efx->mac_lock);
		down_write(&efx->filter_sem);
		efx_ef10_filter_table_remove(efx);

		rc = efx_mcdi_rpc_quiet(efx, MC_CMD_VADAPTOR_SET_MAC, inbuf,
					sizeof(inbuf), NULL, 0, NULL);

		efx_ef10_filter_table_probe(efx);
		up_write(&efx->filter_sem);
		mutex_unlock(&efx->mac_lock);

		if (was_enabled)
			efx_net_open(efx->net_dev);
		efx_device_attach_if_not_resetting(efx);
	}

#ifdef CONFIG_SFC_SRIOV
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_PHYSFN)
	if (efx->pci_dev->is_virtfn && efx->pci_dev->physfn) {
		struct pci_dev *pci_dev_pf = efx->pci_dev->physfn;

		if (rc == -EPERM) {
			struct efx_nic *efx_pf;

			/* Switch to PF and change MAC address on vport */
			efx_pf = pci_get_drvdata(pci_dev_pf);

			rc = efx_ef10_sriov_set_vf_mac(efx_pf,
						       nic_data->vf_index,
						       efx->net_dev->dev_addr);
		} else if (!rc) {
			struct efx_nic *efx_pf = pci_get_drvdata(pci_dev_pf);
			struct efx_ef10_nic_data *nic_data = efx_pf->nic_data;
			unsigned int i;

			/* MAC address successfully changed by VF (with MAC
			 * spoofing) so update the parent PF if possible.
			 */
			for (i = 0; i < efx_pf->vf_count; ++i) {
				struct ef10_vf *vf = nic_data->vf + i;

				if (vf->efx == efx) {
					ether_addr_copy(vf->mac,
							efx->net_dev->dev_addr);
					return 0;
				}
			}
		}
	} else
#endif
#endif
	if (rc == -EPERM) {
		netif_err(efx, drv, efx->net_dev,
			  "Cannot change MAC address; use sfboot to enable mac-spoofing on this interface\n");
	} else if (rc == -ENOSYS && !efx_ef10_is_vf(efx)) {
		/* If the active MCFW does not support MC_CMD_VADAPTOR_SET_MAC
		 * fall-back to the method of changing the MAC address on the
		 * vport.  This only applies to PFs because such versions of
		 * MCFW do not support VFs.
		 */
		rc = efx_ef10_vport_set_mac_address(efx);
	} else if (rc) {
		efx_mcdi_display_error(efx, MC_CMD_VADAPTOR_SET_MAC,
				       sizeof(inbuf), NULL, 0, rc);
	}

	return rc;
}

static int efx_ef10_mac_reconfigure(struct efx_nic *efx, bool mtu_only)
{
	int rc;
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	WARN_ON(!mutex_is_locked(&efx->mac_lock));

	efx_ef10_filter_sync_rx_mode(efx);

	rc = efx_mcdi_set_mac(efx);
	if (rc == -EPERM && mtu_only && (nic_data->datapath_caps &
		    (1 << MC_CMD_GET_CAPABILITIES_OUT_SET_MAC_ENHANCED_LBN)))
		return efx_mcdi_set_mtu(efx);
	return rc;
}

/* On older firmwares there is only a single thread on the MC, so even
 * the shortest operation can be blocked for some time by an operation
 * requested by a different function.
 * See bug61269 for further discussion.
 *
 * On newer firmwares that support multithreaded MCDI commands we extend
 * the timeout for commands we know can run longer.
 */
#define MCDI_EF10_RPC_TIMEOUT       (10 * HZ)
#define MCDI_EF10_RPC_LONG_TIMEOUT  (60 * HZ)
#define MCDI_EF10_RPC_POST_RST_TIME (10 * HZ)

static unsigned int efx_ef10_mcdi_rpc_timeout(struct efx_nic *efx,
					      unsigned int cmd)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	switch (cmd) {
	case MC_CMD_POLL_BIST:
	case MC_CMD_NVRAM_ERASE:
	case MC_CMD_LICENSING_V3:
	case MC_CMD_NVRAM_UPDATE_FINISH:
		/* Potentially longer running commands. */
		if (nic_data->datapath_caps2 &
		    (1 << MC_CMD_GET_CAPABILITIES_V3_OUT_NVRAM_UPDATE_REPORT_VERIFY_RESULT_LBN))
			return MCDI_EF10_RPC_LONG_TIMEOUT;
		/* fall through */
	default:
		/* Some things take longer shortly after a reset. */
		if (time_before(jiffies,
				efx->last_reset + MCDI_EF10_RPC_POST_RST_TIME))
			return MCDI_EF10_RPC_POST_RST_TIME;
		return MCDI_EF10_RPC_TIMEOUT;
	}
}

static unsigned int efx_ef10_mcdi_acquire_timeout(struct efx_nic *efx)
{
	/* The longest running command we know */
	return efx_ef10_mcdi_rpc_timeout(efx, MC_CMD_LICENSING_V3);
}

static int efx_ef10_start_bist(struct efx_nic *efx, u32 bist_type)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_START_BIST_IN_LEN);

	MCDI_SET_DWORD(inbuf, START_BIST_IN_TYPE, bist_type);
	return efx_mcdi_rpc(efx, MC_CMD_START_BIST, inbuf, sizeof(inbuf),
			    NULL, 0, NULL);
}

/* MC BISTs follow a different poll mechanism to phy BISTs.
 * The BIST is done in the poll handler on the MC, and the MCDI command
 * will block until the BIST is done.
 */
static int efx_ef10_poll_bist(struct efx_nic *efx)
{
	int rc;
	MCDI_DECLARE_BUF(outbuf, MC_CMD_POLL_BIST_OUT_LEN);
	size_t outlen;
	u32 result;

	rc = efx_mcdi_rpc(efx, MC_CMD_POLL_BIST, NULL, 0,
			   outbuf, sizeof(outbuf), &outlen);
	if (rc != 0)
		return rc;

	if (outlen < MC_CMD_POLL_BIST_OUT_LEN)
		return -EIO;

	result = MCDI_DWORD(outbuf, POLL_BIST_OUT_RESULT);
	switch (result) {
	case MC_CMD_POLL_BIST_PASSED:
		netif_dbg(efx, hw, efx->net_dev, "BIST passed.\n");
		return 0;
	case MC_CMD_POLL_BIST_TIMEOUT:
		netif_err(efx, hw, efx->net_dev, "BIST timed out\n");
		return -EIO;
	case MC_CMD_POLL_BIST_FAILED:
		netif_err(efx, hw, efx->net_dev, "BIST failed.\n");
		return -EIO;
	default:
		netif_err(efx, hw, efx->net_dev,
			  "BIST returned unknown result %u", result);
		return -EIO;
	}
}

static int efx_ef10_run_bist(struct efx_nic *efx, u32 bist_type)
{
	int rc;

	netif_dbg(efx, drv, efx->net_dev, "starting BIST type %u\n", bist_type);

	rc = efx_ef10_start_bist(efx, bist_type);
	if (rc != 0)
		return rc;

	return efx_ef10_poll_bist(efx);
}

static int
efx_ef10_test_chip(struct efx_nic *efx, struct efx_self_tests *tests)
{
	int rc, rc2;

	efx_reset_down(efx, RESET_TYPE_WORLD);

	rc = efx_mcdi_rpc(efx, MC_CMD_ENABLE_OFFLINE_BIST,
			  NULL, 0, NULL, 0, NULL);
	if (rc != 0)
		goto out;

	tests->memory = efx_ef10_run_bist(efx, MC_CMD_MC_MEM_BIST) ? -1 : 1;
	tests->registers = efx_ef10_run_bist(efx, MC_CMD_REG_BIST) ? -1 : 1;

	rc = efx_mcdi_reset(efx, RESET_TYPE_WORLD);

out:
	if (rc == -EPERM)
		rc = 0;
	rc2 = efx_reset_up(efx, RESET_TYPE_WORLD, rc == 0);
	return rc ? rc : rc2;
}

#ifdef CONFIG_SFC_MTD

struct efx_ef10_nvram_type_info {
	u16 type, type_mask;
	u8 port;
	const char *name;
};

static const struct efx_ef10_nvram_type_info efx_ef10_nvram_types[] = {
	{ NVRAM_PARTITION_TYPE_MC_FIRMWARE,	   0,    0, "sfc_mcfw" },
	{ NVRAM_PARTITION_TYPE_MC_FIRMWARE_BACKUP, 0,    0, "sfc_mcfw_backup" },
	{ NVRAM_PARTITION_TYPE_EXPANSION_ROM,	   0,    0, "sfc_exp_rom" },
	{ NVRAM_PARTITION_TYPE_STATIC_CONFIG,	   0,    0, "sfc_static_cfg" },
	{ NVRAM_PARTITION_TYPE_DYNAMIC_CONFIG,	   0,    0, "sfc_dynamic_cfg" },
	{ NVRAM_PARTITION_TYPE_EXPROM_CONFIG_PORT0, 0,   0, "sfc_exp_rom_cfg" },
	{ NVRAM_PARTITION_TYPE_EXPROM_CONFIG_PORT1, 0,   1, "sfc_exp_rom_cfg" },
	{ NVRAM_PARTITION_TYPE_EXPROM_CONFIG_PORT2, 0,   2, "sfc_exp_rom_cfg" },
	{ NVRAM_PARTITION_TYPE_EXPROM_CONFIG_PORT3, 0,   3, "sfc_exp_rom_cfg" },
	{ NVRAM_PARTITION_TYPE_LICENSE,		   0,    0, "sfc_license" },
	{ NVRAM_PARTITION_TYPE_PHY_MIN,		   0xff, 0, "sfc_phy_fw" },
	{ NVRAM_PARTITION_TYPE_FPGA,		   0,    0, "sfc_fpga" },
	{ NVRAM_PARTITION_TYPE_FPGA_BACKUP,	   0,    0, "sfc_fpgadiag" },
	{ NVRAM_PARTITION_TYPE_FC_FIRMWARE,	   0,    0, "sfc_fcfw" },
	{ NVRAM_PARTITION_TYPE_MUM_FIRMWARE,	   0,    0, "sfc_mumfw" },
	{ NVRAM_PARTITION_TYPE_EXPANSION_UEFI,	   0,    0, "sfc_uefi" },
	{ NVRAM_PARTITION_TYPE_STATUS,		   0,    0, "sfc_status" }
};

static int efx_ef10_mtd_probe_partition(struct efx_nic *efx,
					struct efx_mcdi_mtd_partition *part,
					unsigned int type)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_NVRAM_METADATA_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_NVRAM_METADATA_OUT_LENMAX);
	const struct efx_ef10_nvram_type_info *info;
	size_t size, erase_size, write_size, outlen;
	bool protected;
	int rc;

	for (info = efx_ef10_nvram_types; ; info++) {
		if (info ==
		    efx_ef10_nvram_types + ARRAY_SIZE(efx_ef10_nvram_types))
			return -ENODEV;
		if ((type & ~info->type_mask) == info->type)
			break;
	}
	if (info->port != efx_port_num(efx))
		return -ENODEV;

	rc = efx_mcdi_nvram_info(efx, type, &size, &erase_size, &write_size,
				 &protected);
	if (rc)
		return rc;
	if (protected && !efx_allow_nvconfig_writes)
		return -ENODEV; /* hide it */

	part->nvram_type = type;

	/* The metadata command fails if the dynamic config is empty. In this
	 * situation we want to block a firmware upgrade until the problem has
	 * been fixed.
	 */
	if (type == NVRAM_PARTITION_TYPE_MC_FIRMWARE)
		part->fw_subtype = 0xff;
	else
		part->fw_subtype = 0;

	MCDI_SET_DWORD(inbuf, NVRAM_METADATA_IN_TYPE, type);
	rc = efx_mcdi_rpc(efx, MC_CMD_NVRAM_METADATA, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), &outlen);
	if (rc == 0) {
		if (outlen < MC_CMD_NVRAM_METADATA_OUT_LENMIN)
			return -EIO;
		if (MCDI_DWORD(outbuf, NVRAM_METADATA_OUT_FLAGS) &
		    (1 << MC_CMD_NVRAM_METADATA_OUT_SUBTYPE_VALID_LBN))
			part->fw_subtype = MCDI_DWORD(outbuf,
						   NVRAM_METADATA_OUT_SUBTYPE);
	}

	part->common.dev_type_name = "EF10 NVRAM manager";
	part->common.type_name = info->name;

	part->common.mtd.type = MTD_NORFLASH;
	part->common.mtd.flags = MTD_CAP_NORFLASH;
	part->common.mtd.size = size;
	part->common.mtd.erasesize = erase_size;
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_USE_MTD_WRITESIZE)
	part->common.mtd.writesize = write_size;
#else
	part->common.writesize = write_size;
#endif

	return 0;
}

static int efx_ef10_mtd_probe(struct efx_nic *efx)
{
	MCDI_DECLARE_BUF(outbuf, MC_CMD_NVRAM_PARTITIONS_OUT_LENMAX);
	struct efx_mcdi_mtd_partition *parts;
	size_t outlen, n_parts_total, i, n_parts;
	unsigned int type;
	int rc;

	ASSERT_RTNL();

	BUILD_BUG_ON(MC_CMD_NVRAM_PARTITIONS_IN_LEN != 0);
	rc = efx_mcdi_rpc(efx, MC_CMD_NVRAM_PARTITIONS, NULL, 0,
			  outbuf, sizeof(outbuf), &outlen);
	if (rc)
		return rc;
	if (outlen < MC_CMD_NVRAM_PARTITIONS_OUT_LENMIN)
		return -EIO;

	n_parts_total = MCDI_DWORD(outbuf, NVRAM_PARTITIONS_OUT_NUM_PARTITIONS);
	if (n_parts_total >
	    MCDI_VAR_ARRAY_LEN(outlen, NVRAM_PARTITIONS_OUT_TYPE_ID))
		return -EIO;

	parts = kcalloc(n_parts_total, sizeof(*parts), GFP_KERNEL);
	if (!parts)
		return -ENOMEM;

	n_parts = 0;
	for (i = 0; i < n_parts_total; i++) {
		type = MCDI_ARRAY_DWORD(outbuf, NVRAM_PARTITIONS_OUT_TYPE_ID,
					i);
		rc = efx_ef10_mtd_probe_partition(efx, &parts[n_parts], type);
		if (rc == 0)
			n_parts++;
		else if (rc != -ENODEV)
			goto fail;
	}

	rc = efx_mtd_add(efx, &parts[0].common, n_parts, sizeof(*parts));
fail:
	if (rc)
		kfree(parts);
	return rc;
}

#endif /* CONFIG_SFC_MTD */

#ifdef CONFIG_SFC_PTP

static void efx_ef10_ptp_write_host_time(struct efx_nic *efx, u32 host_time)
{
	_efx_writed(efx, cpu_to_le32(host_time), ER_DZ_MC_DB_LWRD);
}

static void efx_ef10_ptp_write_host_time_vf(struct efx_nic *Efx, u32 host_time) {}

#endif /* CONFIG_SFC_PTP */

#ifdef EFX_NOT_UPSTREAM

int efx_ef10_update_keys(struct efx_nic *efx,
			 struct efx_update_license2 *key_stats)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_LICENSING_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_LICENSING_OUT_LEN);
	int rc;

	MCDI_SET_DWORD(inbuf, LICENSING_IN_OP,
		       MC_CMD_LICENSING_IN_OP_UPDATE_LICENSE);
	rc = efx_mcdi_rpc(efx, MC_CMD_LICENSING, inbuf, sizeof(inbuf),
			  NULL, 0, NULL);
	if (rc)
		return rc;

	MCDI_SET_DWORD(inbuf, LICENSING_IN_OP,
		       MC_CMD_LICENSING_IN_OP_GET_KEY_STATS);
	rc = efx_mcdi_rpc(efx, MC_CMD_LICENSING, inbuf, sizeof(inbuf),
			  outbuf, sizeof(outbuf), NULL);
	if (rc)
		return rc;

	key_stats->valid_keys =
		MCDI_DWORD(outbuf, LICENSING_OUT_VALID_APP_KEYS);
	key_stats->invalid_keys =
		MCDI_DWORD(outbuf, LICENSING_OUT_INVALID_APP_KEYS);
	key_stats->blacklisted_keys =
		MCDI_DWORD(outbuf, LICENSING_OUT_BLACKLISTED_APP_KEYS);
	key_stats->unverifiable_keys =
		MCDI_DWORD(outbuf, LICENSING_OUT_UNVERIFIABLE_APP_KEYS);
	key_stats->wrong_node_keys =
		MCDI_DWORD(outbuf, LICENSING_OUT_WRONG_NODE_APP_KEYS);
	return 0;
}

int efx_ef10_licensed_app_state(struct efx_nic *efx,
				struct efx_licensed_app_state *app_state)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_GET_LICENSED_APP_STATE_IN_LEN);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_GET_LICENSED_APP_STATE_OUT_LEN);
	int rc;

	MCDI_SET_DWORD(inbuf, LICENSING_IN_OP,MC_CMD_GET_LICENSED_APP_STATE);
	MCDI_SET_DWORD(inbuf, GET_LICENSED_APP_STATE_IN_APP_ID,
		       app_state->app_id);

	rc = efx_mcdi_rpc(efx, MC_CMD_GET_LICENSED_APP_STATE, inbuf,
			  sizeof(inbuf), outbuf, sizeof(outbuf), NULL);
	if (!rc)
		app_state->state =
			MCDI_DWORD(outbuf, GET_LICENSED_APP_STATE_OUT_STATE);
	return rc;
}

#endif /* EFX_NOT_UPSTREAM */

#ifdef CONFIG_SFC_PTP
static int efx_ef10_rx_enable_timestamping(struct efx_channel *channel,
					   bool temp)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_PTP_IN_TIME_EVENT_SUBSCRIBE_LEN);
	int rc;
	u32 qid;

	if (channel->sync_events_state == SYNC_EVENTS_REQUESTED ||
	    channel->sync_events_state == SYNC_EVENTS_VALID ||
	    (temp && channel->sync_events_state == SYNC_EVENTS_DISABLED))
		return 0;
	channel->sync_events_state = SYNC_EVENTS_REQUESTED;

	/* Try to subscribe with sync status reporting enabled. If this fails,
	 * fallback to using the old scheme */
	MCDI_SET_DWORD(inbuf, PTP_IN_OP, MC_CMD_PTP_OP_TIME_EVENT_SUBSCRIBE);
	MCDI_SET_DWORD(inbuf, PTP_IN_PERIPH_ID, 0);
	qid = channel->channel |
	      (1 << MC_CMD_PTP_IN_TIME_EVENT_SUBSCRIBE_REPORT_SYNC_STATUS_LBN);
	MCDI_SET_DWORD(inbuf, PTP_IN_TIME_EVENT_SUBSCRIBE_QUEUE, qid);

	rc = efx_mcdi_rpc(channel->efx, MC_CMD_PTP,
			  inbuf, sizeof(inbuf), NULL, 0, NULL);
	if (rc == -ERANGE) {
		MCDI_SET_DWORD(inbuf, PTP_IN_TIME_EVENT_SUBSCRIBE_QUEUE,
			       channel->channel);

		rc = efx_mcdi_rpc(channel->efx, MC_CMD_PTP,
				  inbuf, sizeof(inbuf), NULL, 0, NULL);
	}

	if (rc != 0)
		channel->sync_events_state = temp ? SYNC_EVENTS_QUIESCENT :
						    SYNC_EVENTS_DISABLED;

	return rc;
}

static int efx_ef10_rx_disable_timestamping(struct efx_channel *channel,
					    bool temp)
{
	MCDI_DECLARE_BUF(inbuf, MC_CMD_PTP_IN_TIME_EVENT_UNSUBSCRIBE_LEN);
	int rc;

	if (channel->sync_events_state == SYNC_EVENTS_DISABLED ||
	    (temp && channel->sync_events_state == SYNC_EVENTS_QUIESCENT))
		return 0;
	if (channel->sync_events_state == SYNC_EVENTS_QUIESCENT) {
		channel->sync_events_state = SYNC_EVENTS_DISABLED;
		return 0;
	}
	channel->sync_events_state = temp ? SYNC_EVENTS_QUIESCENT :
					    SYNC_EVENTS_DISABLED;

	MCDI_SET_DWORD(inbuf, PTP_IN_OP, MC_CMD_PTP_OP_TIME_EVENT_UNSUBSCRIBE);
	MCDI_SET_DWORD(inbuf, PTP_IN_PERIPH_ID, 0);
	MCDI_SET_DWORD(inbuf, PTP_IN_TIME_EVENT_UNSUBSCRIBE_CONTROL,
		       MC_CMD_PTP_IN_TIME_EVENT_UNSUBSCRIBE_SINGLE);
	MCDI_SET_DWORD(inbuf, PTP_IN_TIME_EVENT_UNSUBSCRIBE_QUEUE,
		       channel->channel);

	rc = efx_mcdi_rpc(channel->efx, MC_CMD_PTP,
			  inbuf, sizeof(inbuf), NULL, 0, NULL);

	return rc;
}

static int efx_ef10_ptp_set_ts_config_vf(struct efx_nic *efx,
				      struct hwtstamp_config *init)
{
	return -EOPNOTSUPP;
}

static int efx_ef10_ptp_set_ts_sync_events(struct efx_nic *efx, bool en,
					   bool temp)
{
	int (*set)(struct efx_channel *channel, bool temp);
	struct efx_channel *channel;

	set = en ?
	      efx_ef10_rx_enable_timestamping :
	      efx_ef10_rx_disable_timestamping;

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_TSTAMP)
	/* For RX timestamping on a socket we need to get sync events on
	 * all channels.
	 */
	efx_for_each_channel(channel, efx) {
#else
	channel = efx_ptp_channel(efx);
	if (channel) {
#endif
		int rc = set(channel, temp);
		if (en && rc != 0) {
			efx_ef10_ptp_set_ts_sync_events(efx, false, temp);
			return rc;
		}
	}

	return 0;
}

static int efx_ef10_ptp_set_ts_config(struct efx_nic *efx,
				      struct hwtstamp_config *init)
{
	int rc;

	switch (init->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		efx_ef10_ptp_set_ts_sync_events(efx, false, false);
		/* if TX timestamping is still requested then leave PTP on */
		return efx_ptp_change_mode(efx,
					   init->tx_type != HWTSTAMP_TX_OFF, 0);
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_TSTAMP)
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		init->rx_filter = HWTSTAMP_FILTER_ALL;
		rc = efx_ptp_change_mode(efx, true, MC_CMD_PTP_MODE_V2);
#else
	/* We need to know which PTP protocol to match in efx_ptp_rx() */
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		init->rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
		/* this will still work for PTP v1 */
		rc = efx_ptp_change_mode(efx, true,
					 MC_CMD_PTP_MODE_V2_ENHANCED);
#endif
		if (!rc)
			rc = efx_ef10_ptp_set_ts_sync_events(efx, true, false);
		if (rc)
			efx_ptp_change_mode(efx, false, 0);
		return rc;
	default:
		return -ERANGE;
	}
}
#endif

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_NEED_GET_PHYS_PORT_ID)
static int efx_ef10_get_phys_port_id(struct efx_nic *efx,
				     struct netdev_phys_item_id *ppid)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	if (!is_valid_ether_addr(nic_data->port_id))
		return -EOPNOTSUPP;

	ppid->id_len = ETH_ALEN;
	memcpy(ppid->id, nic_data->port_id, ppid->id_len);

	return 0;
}
#endif

#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NDO_VLAN_RX_ADD_VID)
static int efx_ef10_vlan_rx_add_vid(struct efx_nic *efx, __be16 proto, u16 vid)
{
	if (proto != htons(ETH_P_8021Q))
		return -EINVAL;

	return efx_ef10_add_vlan(efx, vid);
}

static int efx_ef10_vlan_rx_kill_vid(struct efx_nic *efx, __be16 proto, u16 vid)
{
	if (proto != htons(ETH_P_8021Q))
		return -EINVAL;

	return efx_ef10_del_vlan(efx, vid);
}
#endif

/* We rely on the MCDI wiping out our TX rings if it made any changes to the
 * ports table, ensuring that any TSO descriptors that were made on a now-
 * removed tunnel port will be blown away and won't break things when we try
 * to transmit them using the new ports table.
 */
static int efx_ef10_set_udp_tnl_ports(struct efx_nic *efx, bool unloading)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	MCDI_DECLARE_BUF(inbuf, MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS_IN_LENMAX);
	MCDI_DECLARE_BUF(outbuf, MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS_OUT_LEN);
	bool will_reset = false;
	size_t num_entries = 0;
	size_t inlen, outlen;
	size_t i;
	int rc;
	efx_dword_t flags_and_num_entries;

	WARN_ON(!mutex_is_locked(&nic_data->udp_tunnels_lock));

	nic_data->udp_tunnels_dirty = false;

	if (!(nic_data->datapath_caps &
	    (1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN))) {
		efx_device_attach_if_not_resetting(efx);
		return 0;
	}

	BUILD_BUG_ON(ARRAY_SIZE(nic_data->udp_tunnels) >
		     MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS_IN_ENTRIES_MAXNUM);

	for (i = 0; i < ARRAY_SIZE(nic_data->udp_tunnels); ++i) {
		if (nic_data->udp_tunnels[i].count &&
		    nic_data->udp_tunnels[i].port) {
			efx_dword_t entry;

			EFX_POPULATE_DWORD_2(entry,
				TUNNEL_ENCAP_UDP_PORT_ENTRY_UDP_PORT,
					ntohs(nic_data->udp_tunnels[i].port),
				TUNNEL_ENCAP_UDP_PORT_ENTRY_PROTOCOL,
					nic_data->udp_tunnels[i].type);
			*_MCDI_ARRAY_DWORD(inbuf,
				SET_TUNNEL_ENCAP_UDP_PORTS_IN_ENTRIES,
				num_entries++) = entry;
		}
	}

	BUILD_BUG_ON((MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS_IN_NUM_ENTRIES_OFST -
		      MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS_IN_FLAGS_OFST) * 8 !=
		     EFX_WORD_1_LBN);
	BUILD_BUG_ON(MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS_IN_NUM_ENTRIES_LEN * 8 !=
		     EFX_WORD_1_WIDTH);
	EFX_POPULATE_DWORD_2(flags_and_num_entries,
			     MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS_IN_UNLOADING,
				!!unloading,
			     EFX_WORD_1, num_entries);
	*_MCDI_DWORD(inbuf, SET_TUNNEL_ENCAP_UDP_PORTS_IN_FLAGS) =
		flags_and_num_entries;

	WARN_ON(unloading && num_entries);

	inlen = MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS_IN_LEN(num_entries);

	rc = efx_mcdi_rpc_quiet(efx, MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS,
				inbuf, inlen, outbuf, sizeof(outbuf), &outlen);
	if (rc == -EIO) {
		/* Most likely the MC rebooted due to another function also
		 * setting its tunnel port list. Mark the tunnel port list as
		 * dirty, so it will be pushed upon coming up from the reboot.
		 */
		nic_data->udp_tunnels_dirty = true;
		return 0;
	}

	if (rc) {
		/* expected not available on unprivileged functions */
		if (rc != -EPERM)
			netif_warn(efx, drv, efx->net_dev,
				   "Unable to set UDP tunnel ports; rc=%d.\n", rc);
	} else if (MCDI_DWORD(outbuf, SET_TUNNEL_ENCAP_UDP_PORTS_OUT_FLAGS) &
		   (1 << MC_CMD_SET_TUNNEL_ENCAP_UDP_PORTS_OUT_RESETTING_LBN)) {
		netif_info(efx, drv, efx->net_dev,
			   "Rebooting MC due to UDP tunnel port list change\n");
		will_reset = true;
		if (unloading)
			/* Delay for the MC reset to complete. This will make
			 * unloading other functions a bit smoother. This is a
			 * race, but the other unload will work whichever way
			 * it goes, this just avoids an unnecessary error
			 * message.
			 */
			msleep(100);
	}
	if (!will_reset && !unloading) {
		/* The caller will have detached, relying on the MC reset to
		 * trigger a re-attach.  Since there won't be an MC reset, we
		 * have to do the attach ourselves.
		 */
		efx_device_attach_if_not_resetting(efx);
	}

	return rc;
}

static int efx_ef10_udp_tnl_push_ports(struct efx_nic *efx)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	int rc = 0;

	mutex_lock(&nic_data->udp_tunnels_lock);
	if (nic_data->udp_tunnels_dirty)
	{
		/* Make sure all TX are stopped while we modify the table, else
		 * we might race against an efx_features_check().
		 */
		efx_device_detach_sync(efx);
		rc = efx_ef10_set_udp_tnl_ports(efx, false);
	}
	mutex_unlock(&nic_data->udp_tunnels_lock);
	return rc;
}

static struct efx_udp_tunnel *__efx_ef10_udp_tnl_lookup_port(struct efx_nic *efx,
							     __be16 port)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(nic_data->udp_tunnels); ++i) {
		if (!nic_data->udp_tunnels[i].count)
			continue;
		if (nic_data->udp_tunnels[i].port == port)
			return &nic_data->udp_tunnels[i];
	}
	return NULL;
}

static int efx_ef10_udp_tnl_add_port(struct efx_nic *efx,
				     struct efx_udp_tunnel tnl)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_udp_tunnel *match;
	char typebuf[8];
	size_t i;
	int rc;

	if (!(nic_data->datapath_caps &
	      (1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN)))
		return 0;

	efx_get_udp_tunnel_type_name(tnl.type, typebuf, sizeof(typebuf));
	netif_dbg(efx, drv, efx->net_dev, "Adding UDP tunnel (%s) port %d\n",
		  typebuf, ntohs(tnl.port));

	mutex_lock(&nic_data->udp_tunnels_lock);
	/* Make sure all TX are stopped while we add to the table, else we
	 * might race against an efx_features_check().
	 */
	if (!nic_data->udp_tunnels_dirty)
		efx_device_detach_sync(efx);

	match = __efx_ef10_udp_tnl_lookup_port(efx, tnl.port);
	if (match != NULL) {
		if (match->type == tnl.type) {
			netif_dbg(efx, drv, efx->net_dev,
				  "Referencing existing tunnel entry\n");
			match->count++;
			/* No need to cause an MCDI update */
			rc = 0;
			goto restart;
		}
		efx_get_udp_tunnel_type_name(match->type,
					     typebuf, sizeof(typebuf));
		netif_dbg(efx, drv, efx->net_dev,
			  "UDP port %d is already in use by %s\n",
			  ntohs(tnl.port), typebuf);
		rc = -EEXIST;
		goto restart;
	}

	for (i = 0; i < ARRAY_SIZE(nic_data->udp_tunnels); ++i)
		if (!nic_data->udp_tunnels[i].count) {
			nic_data->udp_tunnels[i] = tnl;
			nic_data->udp_tunnels[i].count = 1;
			if (!nic_data->udp_tunnels_dirty)
				rc = efx_ef10_set_udp_tnl_ports(efx, false);
			else
				rc = 0;
			goto unlock_out;
		}

	netif_dbg(efx, drv, efx->net_dev,
		  "Unable to add UDP tunnel (%s) port %d; insufficient resources.\n",
		  typebuf, ntohs(tnl.port));

	rc = -ENOMEM;

restart:
	if (!nic_data->udp_tunnels_dirty)
		/* We didn't do the MCDI, so we won't get a reset, so we
		 * need to reattach ourselves
		 */
		netif_device_attach(efx->net_dev);
unlock_out:
	mutex_unlock(&nic_data->udp_tunnels_lock);
	return rc;
}

/* Called under the TX lock with the TX queue running, hence no-one can be
 * in the middle of updating the UDP tunnels table.  However, they could
 * have tried and failed the MCDI, in which case they'll have set the dirty
 * flag before dropping their locks.
 */
static bool efx_ef10_udp_tnl_has_port(struct efx_nic *efx, __be16 port)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;

	if (!(nic_data->datapath_caps &
	      (1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN)))
		return false;

	if (nic_data->udp_tunnels_dirty)
		/* SW table may not match HW state, so just assume we can't
		 * use any UDP tunnel offloads.
		 */
		return false;

	return __efx_ef10_udp_tnl_lookup_port(efx, port) != NULL;
}

static int efx_ef10_udp_tnl_del_port(struct efx_nic *efx,
				     struct efx_udp_tunnel tnl)
{
	struct efx_ef10_nic_data *nic_data = efx->nic_data;
	struct efx_udp_tunnel *match;
	char typebuf[8];
	int rc;

	if (!(nic_data->datapath_caps &
	      (1 << MC_CMD_GET_CAPABILITIES_OUT_VXLAN_NVGRE_LBN)))
		return 0;

	efx_get_udp_tunnel_type_name(tnl.type, typebuf, sizeof(typebuf));
	netif_dbg(efx, drv, efx->net_dev, "Removing UDP tunnel (%s) port %d\n",
		  typebuf, ntohs(tnl.port));

	mutex_lock(&nic_data->udp_tunnels_lock);
	/* Make sure all TX are stopped while we remove from the table, else we
	 * might race against an efx_features_check().
	 */
	if (!nic_data->udp_tunnels_dirty)
		efx_device_detach_sync(efx);

	match = __efx_ef10_udp_tnl_lookup_port(efx, tnl.port);
	if (match != NULL) {
		if (match->type == tnl.type) {
			if (--match->count) {
				/* Port is still in use, so nothing to do */
				netif_dbg(efx, drv, efx->net_dev,
					  "UDP tunnel port %d remains active\n",
					  ntohs(tnl.port));
				rc = 0;
				goto restart;
			}
			if (!nic_data->udp_tunnels_dirty)
				rc = efx_ef10_set_udp_tnl_ports(efx, false);
			else
				rc = 0;
			goto out_unlock;
		}
		efx_get_udp_tunnel_type_name(match->type,
					     typebuf, sizeof(typebuf));
		netif_warn(efx, drv, efx->net_dev,
			  "UDP port %d is actually in use by %s, not removing\n",
			  ntohs(tnl.port), typebuf);
	}
	rc = -ENOENT;

restart:
	if (!nic_data->udp_tunnels_dirty)
		/* We didn't do the MCDI, so we won't get a reset, so we
		 * need to reattach ourselves
		 */
		netif_device_attach(efx->net_dev);
out_unlock:
	mutex_unlock(&nic_data->udp_tunnels_lock);
	return rc;
}

#if !defined(EFX_USE_KCOMPAT) || defined(NETIF_F_IPV6_CSUM)
#define EF10_OFFLOAD_FEATURES		\
	(NETIF_F_IP_CSUM |		\
	 NETIF_F_HW_VLAN_CTAG_FILTER |	\
	 NETIF_F_IPV6_CSUM |		\
	 NETIF_F_RXHASH |		\
	 NETIF_F_NTUPLE)
#else
#define EF10_OFFLOAD_FEATURES		\
	(NETIF_F_IP_CSUM |		\
	 NETIF_F_HW_VLAN_CTAG_FILTER |	\
	 NETIF_F_RXHASH |		\
	 NETIF_F_NTUPLE)
#endif

const struct efx_nic_type efx_hunt_a0_vf_nic_type = {
	.is_vf = true,
	.mem_bar = EFX_MEM_VF_BAR,
	.mem_map_size = efx_ef10_initial_mem_map_size,
	.probe = efx_ef10_probe_vf,
	.remove = efx_ef10_remove,
	.dimension_resources = efx_ef10_dimension_resources,
	.init = efx_ef10_init_nic,
	.fini = efx_port_dummy_op_void,
	.monitor = efx_ef10_monitor,
	.hw_unavailable = efx_ef10_hw_unavailable,
	.map_reset_reason = efx_ef10_map_reset_reason,
	.map_reset_flags = efx_ef10_map_reset_flags,
	.reset = efx_ef10_reset,
	.probe_port = efx_mcdi_port_probe,
	.remove_port = efx_mcdi_port_remove,
	.fini_dmaq = efx_ef10_fini_dmaq,
	.prepare_flr = efx_ef10_prepare_flr,
	.finish_flr = efx_port_dummy_op_void,
	.describe_stats = efx_ef10_describe_stats,
	.update_stats = efx_ef10_update_stats_vf,
	.start_stats = efx_ef10_start_stats_vf,
	.pull_stats = efx_ef10_pull_stats_vf,
	.stop_stats = efx_ef10_stop_stats_vf,
	.update_stats_period = efx_ef10_vf_schedule_stats_work,
	.set_id_led = efx_mcdi_set_id_led,
	.push_irq_moderation = efx_ef10_push_irq_moderation,
	.calc_mac_mtu = efx_nic_calc_mac_mtu,
	.reconfigure_mac = efx_ef10_mac_reconfigure,
	.check_mac_fault = efx_mcdi_mac_check_fault,
	.reconfigure_port = efx_mcdi_port_reconfigure,
	.get_wol = efx_ef10_get_wol_vf,
	.set_wol = efx_ef10_set_wol_vf,
	.resume_wol = efx_port_dummy_op_void,
	.mcdi_request = efx_ef10_mcdi_request,
	.mcdi_poll_response = efx_ef10_mcdi_poll_response,
	.mcdi_read_response = efx_ef10_mcdi_read_response,
	.mcdi_poll_reboot = efx_ef10_mcdi_poll_reboot,
	.mcdi_reboot_detected = efx_ef10_mcdi_reboot_detected,
	.irq_enable_master = efx_port_dummy_op_void,
	.irq_test_generate = efx_ef10_irq_test_generate,
	.irq_disable_non_ev = efx_port_dummy_op_void,
	.irq_handle_msi = efx_ef10_msi_interrupt,
	.irq_handle_legacy = efx_ef10_legacy_interrupt,
	.tx_probe = efx_ef10_tx_probe,
	.tx_init = efx_ef10_tx_init,
	.tx_remove = efx_ef10_tx_remove,
	.tx_write = efx_ef10_tx_write,
	.tx_notify = efx_ef10_notify_tx_desc,
	.tx_limit_len = efx_ef10_tx_limit_len,
	.rx_push_rss_config = efx_ef10_vf_rx_push_rss_config,
	.rx_pull_rss_config = efx_ef10_rx_pull_rss_config,
	.rx_probe = efx_ef10_rx_probe,
	.rx_init = efx_ef10_rx_init,
	.rx_remove = efx_ef10_rx_remove,
	.rx_write = efx_ef10_rx_write,
	.rx_defer_refill = efx_ef10_rx_defer_refill,
	.ev_probe = efx_ef10_ev_probe,
	.ev_init = efx_ef10_ev_init,
	.ev_fini = efx_ef10_ev_fini,
	.ev_remove = efx_ef10_ev_remove,
	.ev_process = efx_ef10_ev_process,
	.ev_read_ack = efx_ef10_ev_read_ack,
	.ev_test_generate = efx_ef10_ev_test_generate,
	.filter_table_probe = efx_ef10_filter_table_probe,
	.filter_table_restore = efx_ef10_filter_table_restore,
	.filter_table_remove = efx_ef10_filter_table_remove,
	.filter_match_supported = efx_ef10_filter_match_supported,
	.filter_update_rx_scatter = efx_ef10_filter_update_rx_scatter,
#ifdef EFX_NOT_UPSTREAM
	/* locks only required for driverlink */
	.filter_insert = efx_ef10_filter_insert_locked,
	.filter_remove_safe = efx_ef10_filter_remove_safe_locked,
#else
	.filter_insert = efx_ef10_filter_insert,
        .filter_remove_safe = efx_ef10_filter_remove_safe,
#endif
	.filter_get_safe = efx_ef10_filter_get_safe,
	.filter_clear_rx = efx_ef10_filter_clear_rx,
	.filter_redirect = efx_ef10_filter_redirect,
	.filter_count_rx_used = efx_ef10_filter_count_rx_used,
	.filter_get_rx_id_limit = efx_ef10_filter_get_rx_id_limit,
	.filter_get_rx_ids = efx_ef10_filter_get_rx_ids,
#ifdef CONFIG_RFS_ACCEL
	.filter_async_insert = efx_ef10_filter_async_insert,
	.filter_rfs_expire_one = efx_ef10_filter_rfs_expire_one,
#endif
#if defined(EFX_NOT_UPSTREAM) && defined(EFX_USE_SARFS)
	.filter_async_remove = efx_ef10_filter_async_remove,
#endif
#ifdef EFX_NOT_UPSTREAM
	.filter_block_kernel = efx_ef10_filter_block_kernel,
	.filter_unblock_kernel = efx_ef10_filter_unblock_kernel,
	.vport_filter_insert = efx_ef10_vport_filter_insert,
	.vport_filter_remove = efx_ef10_vport_filter_remove,
#endif
#ifdef CONFIG_SFC_MTD
	.mtd_probe = efx_port_dummy_op_int,
#endif
#ifdef CONFIG_SFC_PTP
	.ptp_write_host_time = efx_ef10_ptp_write_host_time_vf,
	.ptp_set_ts_config = efx_ef10_ptp_set_ts_config_vf,
#endif
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NDO_VLAN_RX_ADD_VID)
	.vlan_rx_add_vid = efx_ef10_vlan_rx_add_vid,
	.vlan_rx_kill_vid = efx_ef10_vlan_rx_kill_vid,
#endif
	.vswitching_probe = efx_ef10_vswitching_probe_vf,
	.vswitching_restore = efx_ef10_vswitching_restore_vf,
	.vswitching_remove = efx_ef10_vswitching_remove_vf,
	.get_mac_address = efx_ef10_get_mac_address_vf,
	.set_mac_address = efx_ef10_set_mac_address,
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_NEED_GET_PHYS_PORT_ID)
	.get_phys_port_id = efx_ef10_get_phys_port_id,
#endif
	.revision = EFX_REV_HUNT_A0,
	.max_dma_mask = DMA_BIT_MASK(ESF_DZ_TX_KER_BUF_ADDR_WIDTH),
	.rx_prefix_size = ES_DZ_RX_PREFIX_SIZE,
	.rx_hash_offset = ES_DZ_RX_PREFIX_HASH_OFST,
	.rx_ts_offset = ES_DZ_RX_PREFIX_TSTAMP_OFST,
	.can_rx_scatter = true,
	.always_rx_scatter = true,
	.option_descriptors = true,
	.min_interrupt_mode = EFX_INT_MODE_MSIX,
	.max_interrupt_mode = EFX_INT_MODE_MSIX,
	.timer_period_max = 1 << ERF_DD_EVQ_IND_TIMER_VAL_WIDTH,
	.ef10_resources = {
		.hdr.next = ((struct efx_dl_device_info *)
			     &efx_hunt_a0_nic_type.dl_hash_insertion.hdr),
		.hdr.type = EFX_DL_EF10_RESOURCES,
		.vi_base = 0,
		.vi_shift = 0,
		.vi_min = 0,
		.vi_lim = 0,
		.flags = 0,
		.vport_id = EVB_PORT_ID_ASSIGNED
	},
	.dl_hash_insertion = {
		.hdr.type = EFX_DL_HASH_INSERTION,
		.data_offset = ES_DZ_RX_PREFIX_SIZE,
		.hash_offset = ES_DZ_RX_PREFIX_HASH_OFST,
		.flags = (EFX_DL_HASH_TOEP_TCPIP4 | EFX_DL_HASH_TOEP_IP4 |
			  EFX_DL_HASH_TOEP_TCPIP6 | EFX_DL_HASH_TOEP_IP6),
	},
	.offload_features = EF10_OFFLOAD_FEATURES,
	.mcdi_max_ver = 2,
	.mcdi_rpc_timeout = efx_ef10_mcdi_rpc_timeout,
	.mcdi_acquire_timeout = efx_ef10_mcdi_acquire_timeout,
	.max_rx_ip_filters = HUNT_FILTER_TBL_ROWS,
	.hwtstamp_filters = 1 << HWTSTAMP_FILTER_NONE |
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_TSTAMP)
	1 << HWTSTAMP_FILTER_ALL,
#else
	1 << HWTSTAMP_FILTER_PTP_V1_L4_EVENT |
	1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT,
#endif
	.rx_hash_key_size = 40,
};

const struct efx_nic_type efx_hunt_a0_nic_type = {
	.is_vf = false,
	.mem_bar = EFX_MEM_BAR,
	.mem_map_size = efx_ef10_initial_mem_map_size,
	.probe = efx_ef10_probe_pf,
	.remove = efx_ef10_remove,
	.dimension_resources = efx_ef10_dimension_resources,
	.init = efx_ef10_init_nic,
	.fini = efx_port_dummy_op_void,
	.monitor = efx_ef10_monitor,
	.hw_unavailable = efx_ef10_hw_unavailable,
	.map_reset_reason = efx_ef10_map_reset_reason,
	.map_reset_flags = efx_ef10_map_reset_flags,
	.reset = efx_ef10_reset,
	.probe_port = efx_mcdi_port_probe,
	.remove_port = efx_mcdi_port_remove,
	.fini_dmaq = efx_ef10_fini_dmaq,
	.prepare_flr = efx_ef10_prepare_flr,
	.finish_flr = efx_port_dummy_op_void,
	.describe_stats = efx_ef10_describe_stats,
	.update_stats = efx_ef10_update_stats_pf,
	.start_stats = efx_mcdi_mac_start_stats,
	.pull_stats = efx_mcdi_mac_pull_stats,
	.stop_stats = efx_mcdi_mac_stop_stats,
	.update_stats_period = efx_mcdi_mac_update_stats_period,
	.set_id_led = efx_mcdi_set_id_led,
	.push_irq_moderation = efx_ef10_push_irq_moderation,
	.calc_mac_mtu = efx_nic_calc_mac_mtu,
	.reconfigure_mac = efx_ef10_mac_reconfigure,
	.check_mac_fault = efx_mcdi_mac_check_fault,
	.reconfigure_port = efx_mcdi_port_reconfigure,
	.get_wol = efx_ef10_get_wol,
	.set_wol = efx_ef10_set_wol,
	.resume_wol = efx_port_dummy_op_void,
	.test_chip = efx_ef10_test_chip,
	.test_nvram = efx_mcdi_nvram_test_all,
	.mcdi_request = efx_ef10_mcdi_request,
	.mcdi_poll_response = efx_ef10_mcdi_poll_response,
	.mcdi_read_response = efx_ef10_mcdi_read_response,
	.mcdi_poll_reboot = efx_ef10_mcdi_poll_reboot,
	.mcdi_reboot_detected = efx_ef10_mcdi_reboot_detected,
	.irq_enable_master = efx_port_dummy_op_void,
	.irq_test_generate = efx_ef10_irq_test_generate,
	.irq_disable_non_ev = efx_port_dummy_op_void,
	.irq_handle_msi = efx_ef10_msi_interrupt,
	.irq_handle_legacy = efx_ef10_legacy_interrupt,
	.tx_probe = efx_ef10_tx_probe,
	.tx_init = efx_ef10_tx_init,
	.tx_remove = efx_ef10_tx_remove,
	.tx_write = efx_ef10_tx_write,
	.tx_notify = efx_ef10_notify_tx_desc,
	.tx_limit_len = efx_ef10_tx_limit_len,
	.rx_push_rss_config = efx_ef10_pf_rx_push_rss_config,
	.rx_pull_rss_config = efx_ef10_rx_pull_rss_config,
	.rx_probe = efx_ef10_rx_probe,
	.rx_init = efx_ef10_rx_init,
	.rx_remove = efx_ef10_rx_remove,
	.rx_write = efx_ef10_rx_write,
	.rx_defer_refill = efx_ef10_rx_defer_refill,
	.ev_probe = efx_ef10_ev_probe,
	.ev_init = efx_ef10_ev_init,
	.ev_fini = efx_ef10_ev_fini,
	.ev_remove = efx_ef10_ev_remove,
	.ev_process = efx_ef10_ev_process,
	.ev_read_ack = efx_ef10_ev_read_ack,
	.ev_test_generate = efx_ef10_ev_test_generate,
	.filter_table_probe = efx_ef10_filter_table_probe,
	.filter_table_restore = efx_ef10_filter_table_restore,
	.filter_table_remove = efx_ef10_filter_table_remove,
	.filter_match_supported = efx_ef10_filter_match_supported,
	.filter_update_rx_scatter = efx_ef10_filter_update_rx_scatter,
#ifdef EFX_NOT_UPSTREAM
	/* locks only required for driverlink */
	.filter_insert = efx_ef10_filter_insert_locked,
	.filter_remove_safe = efx_ef10_filter_remove_safe_locked,
#else
	.filter_insert = efx_ef10_filter_insert,
        .filter_remove_safe = efx_ef10_filter_remove_safe,
#endif
	.filter_get_safe = efx_ef10_filter_get_safe,
	.filter_clear_rx = efx_ef10_filter_clear_rx,
	.filter_redirect = efx_ef10_filter_redirect,
	.filter_count_rx_used = efx_ef10_filter_count_rx_used,
	.filter_get_rx_id_limit = efx_ef10_filter_get_rx_id_limit,
	.filter_get_rx_ids = efx_ef10_filter_get_rx_ids,
#if (defined(EFX_NOT_UPSTREAM) && defined(EFX_USE_SARFS)) || defined(CONFIG_RFS_ACCEL)
	.filter_async_insert = efx_ef10_filter_async_insert,
#endif
#ifdef CONFIG_RFS_ACCEL
	.filter_rfs_expire_one = efx_ef10_filter_rfs_expire_one,
#endif
#if defined(EFX_NOT_UPSTREAM) && defined(EFX_USE_SARFS)
	.filter_async_remove = efx_ef10_filter_async_remove,
#endif
#ifdef EFX_NOT_UPSTREAM
	.filter_block_kernel = efx_ef10_filter_block_kernel,
	.filter_unblock_kernel = efx_ef10_filter_unblock_kernel,
	.vport_filter_insert = efx_ef10_vport_filter_insert,
	.vport_filter_remove = efx_ef10_vport_filter_remove,
#endif
#ifdef CONFIG_SFC_MTD
	.mtd_probe = efx_ef10_mtd_probe,
	.mtd_rename = efx_mcdi_mtd_rename,
	.mtd_read = efx_mcdi_mtd_read,
	.mtd_erase = efx_mcdi_mtd_erase,
	.mtd_write = efx_mcdi_mtd_write,
	.mtd_sync = efx_mcdi_mtd_sync,
#endif
#ifdef CONFIG_SFC_PTP
	.ptp_write_host_time = efx_ef10_ptp_write_host_time,
	.ptp_set_ts_sync_events = efx_ef10_ptp_set_ts_sync_events,
	.ptp_set_ts_config = efx_ef10_ptp_set_ts_config,
#endif
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NDO_VLAN_RX_ADD_VID)
	.vlan_rx_add_vid = efx_ef10_vlan_rx_add_vid,
	.vlan_rx_kill_vid = efx_ef10_vlan_rx_kill_vid,
#endif
	.udp_tnl_push_ports = efx_ef10_udp_tnl_push_ports,
	.udp_tnl_add_port = efx_ef10_udp_tnl_add_port,
	.udp_tnl_has_port = efx_ef10_udp_tnl_has_port,
	.udp_tnl_del_port = efx_ef10_udp_tnl_del_port,
#ifdef CONFIG_SFC_SRIOV
	.sriov_configure = efx_ef10_sriov_configure,
	.sriov_init = efx_ef10_sriov_init,
	.sriov_fini = efx_ef10_sriov_fini,
	.sriov_wanted = efx_ef10_sriov_wanted,
	.sriov_reset = efx_port_dummy_op_void,
	.sriov_flr = efx_ef10_sriov_flr,
	.sriov_set_vf_mac = efx_ef10_sriov_set_vf_mac,
	.sriov_set_vf_vlan = efx_ef10_sriov_set_vf_vlan,
	.sriov_set_vf_spoofchk = efx_ef10_sriov_set_vf_spoofchk,
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NDO_SET_VF_MAC)
	.sriov_get_vf_config = efx_ef10_sriov_get_vf_config,
#endif
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_VF_LINK_STATE)
	.sriov_set_vf_link_state = efx_ef10_sriov_set_vf_link_state,
#endif
#endif
	.vswitching_probe = efx_ef10_vswitching_probe_pf,
	.vswitching_restore = efx_ef10_vswitching_restore_pf,
	.vswitching_remove = efx_ef10_vswitching_remove_pf,
	.get_mac_address = efx_ef10_get_mac_address_pf,
	.set_mac_address = efx_ef10_set_mac_address,
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_NEED_GET_PHYS_PORT_ID)
	.get_phys_port_id = efx_ef10_get_phys_port_id,
#endif
	.revision = EFX_REV_HUNT_A0,
	.max_dma_mask = DMA_BIT_MASK(ESF_DZ_TX_KER_BUF_ADDR_WIDTH),
	.rx_prefix_size = ES_DZ_RX_PREFIX_SIZE,
	.rx_hash_offset = ES_DZ_RX_PREFIX_HASH_OFST,
	.rx_ts_offset = ES_DZ_RX_PREFIX_TSTAMP_OFST,
	.can_rx_scatter = true,
	.always_rx_scatter = true,
	.option_descriptors = true,
	.min_interrupt_mode = EFX_INT_MODE_LEGACY,
	.max_interrupt_mode = EFX_INT_MODE_MSIX,
	.timer_period_max = 1 << ERF_DD_EVQ_IND_TIMER_VAL_WIDTH,
	.ef10_resources = {
		.hdr.next = ((struct efx_dl_device_info *)
			     &efx_hunt_a0_nic_type.dl_hash_insertion.hdr),
		.hdr.type = EFX_DL_EF10_RESOURCES,
		.vi_base = 0,
		.vi_shift = 0,
		.vi_min = 0,
		.vi_lim = 0,
		.flags = 0,
		.vport_id = EVB_PORT_ID_ASSIGNED
	},
	.dl_hash_insertion = {
		.hdr.type = EFX_DL_HASH_INSERTION,
		.data_offset = ES_DZ_RX_PREFIX_SIZE,
		.hash_offset = ES_DZ_RX_PREFIX_HASH_OFST,
		.flags = (EFX_DL_HASH_TOEP_TCPIP4 | EFX_DL_HASH_TOEP_IP4 |
			  EFX_DL_HASH_TOEP_TCPIP6 | EFX_DL_HASH_TOEP_IP6),
	},
	.offload_features = EF10_OFFLOAD_FEATURES,
	.mcdi_max_ver = 2,
	.mcdi_rpc_timeout = efx_ef10_mcdi_rpc_timeout,
	.mcdi_acquire_timeout = efx_ef10_mcdi_acquire_timeout,
	.max_rx_ip_filters = HUNT_FILTER_TBL_ROWS,
	.hwtstamp_filters = 1 << HWTSTAMP_FILTER_NONE |
#if !defined(EFX_USE_KCOMPAT) || defined(EFX_HAVE_NET_TSTAMP)
			    1 << HWTSTAMP_FILTER_ALL,
#else
		 	    1 << HWTSTAMP_FILTER_PTP_V1_L4_EVENT |
			    1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT,
#endif
	.rx_hash_key_size = 40,
};
