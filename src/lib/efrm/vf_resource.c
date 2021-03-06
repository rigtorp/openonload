/*
** Copyright 2005-2019  Solarflare Communications Inc.
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
 * Driver for Solarflare network controllers -
 *          resource management for Xen backend, OpenOnload, etc
 *           (including support for SFE4001 10GBT NIC)
 *
 * This file contains the VF resource manager.
 *
 * Copyright 2005-2007: Solarflare Communications Inc,
 *                      9501 Jeronimo Road, Suite 250,
 *                      Irvine, CA 92618, USA
 *
 * Developed and maintained by Solarflare Communications:
 *                      <linux-xen-drivers@solarflare.com>
 *                      <onload-dev@solarflare.com>
 *
 * Certain parts of the driver were implemented by
 *          Alexandra Kossovsky <Alexandra.Kossovsky@oktetlabs.ru>
 *          OKTET Labs Ltd, Russia,
 *          http://oktetlabs.ru, <info@oktetlabs.ru>
 *          by request of Solarflare Communications
 *
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

#include <ci/efrm/config.h>

#ifdef CONFIG_SFC_RESOURCE_VF

#include <ci/efrm/nic_table.h>
#include <ci/efrm/private.h>
#include "efrm_internal.h"
#include <ci/efrm/vi_resource_manager.h>
#include <ci/efrm/vf_resource_private.h>
#include <ci/efrm/pd.h>

struct efrm_vf_nic_params {
	struct list_head free_list; /* List of free VFs */
	int vfs_probed;             /* Number of already-discovered VFs */

	unsigned vi_base;
	unsigned vi_scale;
	unsigned vf_count;          /* Total number of VFs */
};
struct efrm_vf_resource_manager {
	struct efrm_resource_manager rm;
	struct efrm_vf_nic_params nic[EFHW_MAX_NR_DEVS];
};
static struct efrm_vf_resource_manager *efrm_vf_manager;


/*********************************************************************
 *
 *  Create/destroy RM
 *
 *********************************************************************/
static void efrm_vf_rm_dtor(struct efrm_resource_manager *rm)
{
	EFRM_ASSERT(&efrm_vf_manager->rm == rm);
}
int
efrm_create_vf_resource_manager(struct efrm_resource_manager **rm_out)
{
	int rc;
	int nic_index;

	EFRM_ASSERT(rm_out);

	efrm_vf_manager = kzalloc(sizeof(*efrm_vf_manager), GFP_KERNEL);
	if (efrm_vf_manager == NULL)
		return -ENOMEM;

	for (nic_index = 0; nic_index < EFHW_MAX_NR_DEVS; ++nic_index)
		INIT_LIST_HEAD(&efrm_vf_manager->nic[nic_index].free_list);

	rc = efrm_resource_manager_ctor(&efrm_vf_manager->rm,
					efrm_vf_rm_dtor, "VF",
					EFRM_RESOURCE_VF);
	if (rc < 0)
		goto fail1;

	*rm_out = &efrm_vf_manager->rm;
	return 0;

fail1:
	EFRM_DO_DEBUG(memset(efrm_vf_manager, 0, sizeof(*efrm_vf_manager)));
	kfree(efrm_vf_manager);
	return rc;
}

void
efrm_vf_init_nic_params(struct efhw_nic* nic,
			const struct vi_resource_dimensions *res_dim)
{
	struct efrm_vf_nic_params *nic_params =
		&efrm_vf_manager->nic[nic->index];

#ifndef NDEBUG
	int nic_index;

	/* Sanity check the nic index to ensure it's been initialised before
	 * we got here.
	 */
	for (nic_index = 0; nic_index < EFHW_MAX_NR_DEVS; ++nic_index)
		if (efrm_nic_tablep->nic[nic_index] != NULL &&
		    memcmp(efrm_nic_tablep->nic[nic_index]->mac_addr,
			   nic->mac_addr, ETH_ALEN) == 0)
			break;

	EFRM_ASSERT(nic_index < EFHW_MAX_NR_DEVS);
#endif

	EFRM_TRACE("vf_vi_base=%u vf_vi_scale=%u vf_count=%u",
		   res_dim->vf_vi_base, res_dim->vf_vi_scale,
		   res_dim->vf_count);

	nic_params->vi_base = res_dim->vf_vi_base;
	nic_params->vi_scale = res_dim->vf_vi_scale;
	nic_params->vf_count = res_dim->vf_count;
}

void
efrm_vf_nic_params(struct efhw_nic* nic, unsigned *vi_base_out,
		   unsigned *vi_scale_out, unsigned *vf_count_out)
{
	struct efrm_vf_nic_params *nic_params =
		&efrm_vf_manager->nic[nic->index];

	*vi_base_out = nic_params->vi_base;
	*vi_scale_out = nic_params->vi_scale;
	*vf_count_out = nic_params->vf_count;
}

/*********************************************************************
 *
 *  Alloc/release resource
 *
 *********************************************************************/

struct efrm_vf *
efrm_vf_from_resource(struct efrm_resource *rs)
{
	return efrm_vf(rs);
}
EXPORT_SYMBOL(efrm_vf_from_resource);


struct efrm_resource *
efrm_vf_to_resource(struct efrm_vf *vf)
{
	return &vf->rs;
}


int
efrm_vf_resource_alloc(struct efrm_client *client, 
		       struct efrm_vf *linked, int use_iommu,
		       struct efrm_vf **vf_out)
{
	struct efrm_vf_nic_params *nic =
		&efrm_vf_manager->nic[client->nic->index];
	struct efrm_vf *vf;
	int rc = 0;
	struct efrm_pd_owner_ids *owner_ids;

	if (nic->vf_count != nic->vfs_probed) {
		EFRM_ERR("%s: not all VFs for NIC %d are discovered yet: "
			 "%d out of %d", __func__, client->nic->index, 
			 nic->vfs_probed, nic->vf_count);
		return -EBUSY;
	}

	spin_lock_bh(&efrm_vf_manager->rm.rm_lock);
	if (list_empty(&nic->free_list)) {
		spin_unlock_bh(&efrm_vf_manager->rm.rm_lock);
		return rc == 0 ? -ENOBUFS : rc;
	}
	vf = list_entry(nic->free_list.next, struct efrm_vf, link);
	list_del(&vf->link);
	spin_unlock_bh(&efrm_vf_manager->rm.rm_lock);

	rc = efrm_vf_alloc_init(vf, linked, use_iommu);
	if (rc != 0) {
		/* Scary warnings are already printed, just return */
		/* Add to the tail of the list in hope another function
		 * is better. */
		list_add_tail(&vf->link,
			      &efrm_vf_manager->nic[vf->nic_index].free_list);
		return rc;
	}

	EFRM_ASSERT(vf);
	EFRM_ASSERT(vf->irq_count);
	EFRM_ASSERT(vf->vi_count);

	rc = efrm_buddy_range_ctor(&vf->vi_instances, vf->vi_base,
				   vf->vi_base + vf->vi_count);
	if (rc < 0) {
		EFRM_ERR("NIC %d VF %d: efrm_buddy_range_ctor(%d, %d) failed",
			 client->nic->index, vf->pci_dev_fn,
			 vf->vi_base, vf->vi_base + vf->vi_count);
		spin_lock_bh(&efrm_vf_manager->rm.rm_lock);
		list_add(&vf->link,
			 &efrm_vf_manager->nic[vf->nic_index].free_list);
		spin_unlock_bh(&efrm_vf_manager->rm.rm_lock);
		return -ENOMEM;
	}

	EFRM_ASSERT(vf->rs.rs_ref_count == 0);
	efrm_resource_init(&vf->rs, EFRM_RESOURCE_VF, vf->pci_dev_fn);

	efrm_client_add_resource(client, &vf->rs);

	owner_ids = efrm_pd_owner_ids_ctor(
		/* On falcon owner_ids are global, so base this block on the
		 * base vi id to avoid overlap.
		 */
		client->nic->devtype.arch == EFHW_ARCH_EF10 ? 1 : vf->vi_base,
		(1 << vf->vi_scale));
	if (!owner_ids)
		return -ENOMEM;
	vf->owner_ids = owner_ids;

	EFRM_TRACE("NIC %d VF %d allocated",
		   client->nic->index, vf->pci_dev_fn);
	*vf_out = vf;
	return 0;
}
EXPORT_SYMBOL(efrm_vf_resource_alloc);

void
efrm_vf_resource_free(struct efrm_vf *vf)
{
	EFRM_TRACE("NIC %d VF %d free",
		   vf->rs.rs_client->nic->index, vf->pci_dev_fn);
	EFRM_ASSERT(vf->rs.rs_ref_count == 0);
	efrm_buddy_dtor(&vf->vi_instances);
	efrm_vf_free_reset(vf);

	spin_lock_bh(&efrm_vf_manager->rm.rm_lock);
	list_add(&vf->link, &efrm_vf_manager->nic[vf->nic_index].free_list);
	spin_unlock_bh(&efrm_vf_manager->rm.rm_lock);
	efrm_client_put(vf->rs.rs_client);
	efrm_pd_owner_ids_dtor(vf->owner_ids);
}

void efrm_vf_resource_release(struct efrm_vf *vf)
{
	if (__efrm_resource_release(efrm_vf_to_resource(vf)))
		efrm_vf_resource_free(vf);
}
EXPORT_SYMBOL(efrm_vf_resource_release);

int efrm_vf_get_nic_index(struct efrm_vf *vf)
{
	return vf->nic_index;
}

efhw_iommu_domain *
efrm_vf_get_iommu_domain(struct efrm_vf *vf)
{
	return vf ? vf->iommu_domain : NULL;
}

/*********************************************************************
 *
 *  VF creation:
 *  OS-independent parts to be called after VF is really probed by OS.
 *
 *********************************************************************/
static void efrm_vf_enumerate(int nic_index)
{
	int first_fn = 0xffff, second_fn = 0xffff;
	struct efrm_vf_nic_params *nic = &efrm_vf_manager->nic[nic_index];
	struct list_head *link;

	EFRM_ASSERT(nic->vfs_probed == nic->vf_count);

	EFRM_NOTICE("All %d VFs for NIC %d are discovered",
		    nic->vf_count, nic_index);

	if (nic->vfs_probed == 1) {
		list_entry(nic->free_list.next, struct efrm_vf,
			   link)->vi_base = nic->vi_base;
		return;
	}

	/* Find the smallest pci_dev_fn and the next one. */
	list_for_each(link, &nic->free_list) {
		int fn = list_entry(link, struct efrm_vf, link)->pci_dev_fn;
		if (first_fn > fn) {
			second_fn = first_fn;
			first_fn = fn;
		}
		else if (second_fn > fn)
			second_fn = fn;
	}

	/* Next, calculate vi_base for each VF */
	spin_lock_bh(&efrm_vf_manager->rm.rm_lock);
	list_for_each(link, &nic->free_list) {
		struct efrm_vf *vf = list_entry(link, struct efrm_vf, link);
		vf->vi_base = nic->vi_base +
			(vf->pci_dev_fn - first_fn) /
			(second_fn - first_fn) *
			(1 << nic->vi_scale);
		EFRM_TRACE("NIC %d VF %d: VI instances %d-%d", nic_index,
			   vf->pci_dev_fn, vf->vi_base,
			   vf->vi_base + vf->vi_count);
	}
	spin_unlock_bh(&efrm_vf_manager->rm.rm_lock);
}

int efrm_vf_probed(struct efrm_vf *vf)
{
	int nic_index;
	INIT_LIST_HEAD(&vf->link);

	/* Find the NIC we are working with */
	for (nic_index = 0; nic_index < EFHW_MAX_NR_DEVS; ++nic_index)
		if (efrm_nic_tablep->nic[nic_index] != NULL &&
		    memcmp(efrm_nic_tablep->nic[nic_index]->mac_addr,
			   vf->mac_addr, ETH_ALEN) == 0)
			break;

	EFRM_ASSERT(vf->vi_scale == efrm_vf_manager->nic[nic_index].vi_scale);

	if (nic_index == EFHW_MAX_NR_DEVS) {
		EFRM_ERR("%s: no NIC with MAC "MAC_ADDR_FMT, __func__,
			 MAC_ADDR_VAL(vf->mac_addr));
		return -ENOENT;
	}

	if (efrm_nic_tablep->nic[vf->nic_index]->flags
		& NIC_FLAG_ONLOAD_UNSUPPORTED) {
		EFRM_ERR("%s: NIC does not support onload",
				__func__);
		return -ENOENT;
	}

	vf->nic_index = nic_index;
	spin_lock_bh(&efrm_vf_manager->rm.rm_lock);
	EFRM_ASSERT(vf->vi_count);
	list_add(&vf->link, &efrm_vf_manager->nic[nic_index].free_list);
	efrm_vf_manager->nic[nic_index].vfs_probed++;
	spin_unlock_bh(&efrm_vf_manager->rm.rm_lock);

	/* If we've got the last VF, re-enumerate them and set vi_base */
	if (efrm_vf_manager->nic[nic_index].vfs_probed ==
	    efrm_vf_manager->nic[nic_index].vf_count) {
		efrm_vf_enumerate(nic_index);
	}

	return 0;
}
void efrm_vf_removed(struct efrm_vf *vf)
{
	spin_lock_bh(&efrm_vf_manager->rm.rm_lock);
	list_del(&vf->link);
	vf->vi_count = 0;
	efrm_vf_manager->nic[vf->nic_index].vfs_probed--;
	spin_unlock_bh(&efrm_vf_manager->rm.rm_lock);
}


#ifndef IOAPIC_RANGE_START
# define IOAPIC_RANGE_START      (0xfee00000)
#endif
#ifndef IOAPIC_RANGE_END
# define IOAPIC_RANGE_END        (0xfeefffff)
#endif
/*
 * Simple linear allocator but ignoring IOAPIC ranges
 * SFC bug 29357 notes that iova reuse can be dangerous
 */
unsigned long efrm_vf_alloc_ioaddrs(struct efrm_vf *vf, int n_pages,
				    efhw_iommu_domain **iommu_domain_out)
{
	unsigned long iova;
	size_t size;

	EFRM_ASSERT(vf);
	if (vf->iova_basep == NULL) {
		if (iommu_domain_out)
			*iommu_domain_out = NULL;
		return 0;
	}

	size = n_pages * PAGE_SIZE;
	iova = *vf->iova_basep;
	if (size > 0) {
		spin_lock_bh(&efrm_vf_manager->rm.rm_lock);
		iova = *vf->iova_basep;
		if ((iova & (size - 1)) != 0 )
			iova = (iova + size - 1) & ~(size - 1);

		if ((iova <= IOAPIC_RANGE_END) &&
		    ((iova + size) > IOAPIC_RANGE_START))
			iova = IOAPIC_RANGE_END + 1;
		else if (iova <= vf->pci_dev->resource[0].end &&
			 iova + size > vf->pci_dev->resource[0].start)
			iova = vf->pci_dev->resource[0].end + 1;
		else if (iova <= vf->pci_dev->resource[2].end &&
			 iova + size > vf->pci_dev->resource[2].start)
			iova = vf->pci_dev->resource[2].end + 1;

		if ((iova & (size - 1)) != 0 )
			iova = (iova + size - 1) & ~(size - 1);
		*vf->iova_basep = iova + size;
		spin_unlock_bh(&efrm_vf_manager->rm.rm_lock);
	}

	if (iommu_domain_out)
		*iommu_domain_out = vf->iommu_domain;
	return iova;
}
#endif /*CONFIG_SFC_RESOURCE_VF*/
