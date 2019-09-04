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

#ifndef __CI_DRIVER_EFAB_HARDWARE_EF10_EVQ_H__
#define __CI_DRIVER_EFAB_HARDWARE_EF10_EVQ_H__


/* Primes the EVQ by poking the current pointer to the relevant register.  See
 * also ef10_update_evq_rptr_bug35388_workaround(), for use on hardware where
 * writes to that register can cause a lockup. */
static inline void ef10_update_evq_rptr(volatile void* io_page, int rptr)
{
	writel(rptr << ERF_DZ_EVQ_RPTR_LBN,
	       (char*) io_page + ER_DZ_EVQ_RPTR_REG);
	mmiowb();
}


/* Workaround for the lockup issue: bug35981, bug35887, bug35388, bug36064. */
static inline void
ef10_update_evq_rptr_bug35388_workaround(volatile void* io_page, int rptr)
{
        const uint32_t REV0_OP_RPTR_HI = 0x800;
        const uint32_t REV0_OP_RPTR_LO = 0x900;

	uint32_t rptr_hi = REV0_OP_RPTR_HI | ((rptr >> 8) & 0xff);
	uint32_t rptr_lo = REV0_OP_RPTR_LO | (rptr & 0xff);

	writel(rptr_hi, (char*) io_page + ER_DZ_TX_DESC_UPD_REG + 8);
	wmb();
	writel(rptr_lo, (char*) io_page + ER_DZ_TX_DESC_UPD_REG + 8);
	mmiowb();
}


#endif
