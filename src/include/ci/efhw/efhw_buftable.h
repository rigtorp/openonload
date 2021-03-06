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

#ifndef __CI_EFHW_EFHW_BUFTABLE_H__
#define __CI_EFHW_EFHW_BUFTABLE_H__

#include <ci/efhw/efhw_types.h>

#ifndef NDEBUG
static inline void
efhw_buffer_table_set_debug(struct efhw_buffer_table_block *block,
			     int first_entry, int n_entries)
{
	EFHW_ASSERT(first_entry >= 0);
	EFHW_ASSERT(first_entry < EFHW_BUFFER_TABLE_BLOCK_SIZE);
	EFHW_ASSERT(n_entries > 0);
	EFHW_ASSERT(n_entries <= EFHW_BUFFER_TABLE_BLOCK_SIZE);
	EFHW_ASSERT(first_entry + n_entries <=
		    EFHW_BUFFER_TABLE_BLOCK_SIZE);
}

static inline void
efhw_buffer_table_clear_debug(struct efhw_buffer_table_block *block,
			       int first_entry, int n_entries)
{
	EFHW_ASSERT(first_entry >= 0);
	EFHW_ASSERT(first_entry < EFHW_BUFFER_TABLE_BLOCK_SIZE);
	EFHW_ASSERT(n_entries > 0);
	EFHW_ASSERT(n_entries <= EFHW_BUFFER_TABLE_BLOCK_SIZE);
	EFHW_ASSERT(first_entry + n_entries <=
		    EFHW_BUFFER_TABLE_BLOCK_SIZE);
}

#endif /* NDEBUG */

#endif /* __CI_EFHW_EFHW_BUFTABLE_H__ */
