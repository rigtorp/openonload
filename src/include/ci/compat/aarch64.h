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
** <L5_PRIVATE L5_OPEN>
** \author  
**  \brief  
**   \date  
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/

/*! \cidoxg_include_ci_compat */

#ifndef __CI_COMPAT_AARCH64_H__
#define __CI_COMPAT_AARCH64_H__

#define CI_MY_BYTE_ORDER	CI_LITTLE_ENDIAN

#define CI_WORD_SIZE		8
#define CI_PTR_SIZE		8

#if defined(AARCH64_PAGE_SIZE)
#define CI_PAGE_SIZE		AARCH64_PAGE_SIZE
#elif defined(__KERNEL__)
#include <asm/page.h>
#define CI_PAGE_SIZE		PAGE_SIZE
#else
#error "PAGE_SIZE is not known"
#endif

/* There is no simple way to do ffs at compile time,
 * and sysconf returns PAGE_SIZE, not page shift, so just
 * check the sizes that are known to be supported on Linux/arm64
 */
#if CI_PAGE_SIZE == (4 * 1024)
#define CI_PAGE_SHIFT	(12u)
#elif CI_PAGE_SIZE == (16 * 1024)
#define CI_PAGE_SHIFT	(14u)
#elif CI_PAGE_SIZE == (64 * 1024)
#define CI_PAGE_SHIFT	(16u)
#else
#error "Unsupported PAGE_SIZE"
#endif
#define CI_PAGE_MASK		(~((ci_uintptr_t) CI_PAGE_SIZE - 1))

#define CI_CACHE_LINE_SIZE      64

#define CI_CPU_HAS_IOSPACE 0 /* CPU has a separate IO space */

#define CI_MAX_TIME_T 0x7fffffffffffffffLL

#endif  /* __CI_COMPAT_AARCH64_H__ */

/*! \cidoxg_end */
