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
** <L5_PRIVATE L5_SOURCE>
** \author  slp
**  \brief  Linux-specific functions used in common code
**     $Id$
**   \date  2002/08
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/
  
/*! \cidoxg_driver_linux_onload */
 
/*--------------------------------------------------------------------
 *
 * CI headers
 *
 *--------------------------------------------------------------------*/

#include <onload/fd_private.h>
#include <ci/internal/ip.h>
#include <onload/linux_onload_internal.h>
#include <onload/tcp_helper.h>


ci_private_t *
ci_fpriv(struct file *filp)
{
  if( filp->f_op != &oo_fops ) /* not one of ours */
    return NULL;

  return (ci_private_t *)(filp->private_data);
}

struct file *
ci_privf(ci_private_t *priv)
{
  return priv->_filp;
}


#if CI_MEMLEAK_DEBUG_ALLOC_TABLE
/* See ci/tools/memleak_debug.h */
struct ci_alloc_entry *ci_alloc_table[CI_ALLOC_TABLE_BULKS];
unsigned int ci_alloc_table_sz = 0;
EXPORT_SYMBOL(ci_alloc_table_add);
EXPORT_SYMBOL(ci_alloc_table_del);
#endif /* CI_MEMLEAK_DEBUG_ALLOC_TABLE */


#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0))

/* Kernel versions earlier than 3.14 contain a bug which prevents
 * processes started via call_usermodehelper() from setting their
 * CPU affinity.
 *
 * This local variant of call_usermodehelper() fixes that bug.
 *
 * Clearing the PF_NO_SETAFFINITY flag like this should be safe on
 * newer kernels too, or (more relevantly) on older kernels with
 * backported fixes.
 *
 * See https://github.com/torvalds/linux/commit/b88fae644e5e3922251a4b242f435f5e3b49c381 for details.
 */

#ifdef PF_NO_SETAFFINITY
static int
ci_usermodehelper_init(struct subprocess_info *info
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,0))
                       , struct cred *new
#endif
  )
{
  current->flags &= ~PF_NO_SETAFFINITY;
  return 0;
}
#else
#define ci_usermodehelper_init NULL
#endif

#ifdef EFRM_HAVE_USERMODEHELPER_SETUP

int
ci_call_usermodehelper_setup(char *path, char **argv, char **envp, int wait)
{
  struct subprocess_info *info;

#ifdef EFRM_HAVE_USERMODEHELPER_SETUP_INFO
  info = call_usermodehelper_setup(path, argv, envp, GFP_KERNEL,
                                   ci_usermodehelper_init, NULL, NULL);
  if( info == NULL )
    return -ENOMEM;

#else /* ! EFRM_HAVE_USERMODEHELPER_SETUP_INFO */

  info = call_usermodehelper_setup(path, argv, envp, GFP_KERNEL);
  if( info == NULL )
    return -ENOMEM;
#ifdef PF_NO_SETAFFINITY
  /* linux<3.10 does not have PF_NO_SETAFFINITY, so this chunk
   * of code is useless. */
  call_usermodehelper_setfns(info, ci_usermodehelper_init, NULL, NULL);
#endif /* PF_NO_SETAFFINITY */

#endif /* EFRM_HAVE_USERMODEHELPER_SETUP_INFO */

  return call_usermodehelper_exec(info, wait);
}

#else

int
ci_call_usermodehelper_fns(char *path, char **argv, char **envp, int wait)
{
  return call_usermodehelper_fns(path, argv, envp, wait,
                                 ci_usermodehelper_init, NULL, NULL);
}

#endif /* EFRM_HAVE_USERMODEHELPER_SETUP */

/* Some kernels have call_usermodehelper_fns() which can be used here;
 * other kernels do not have it.  In that case we use _setup() and _exec(),
 * but we also need to handle the two different variants of _setup().
 */
int
ci_call_usermodehelper(char *path, char **argv, char **envp, int wait)
{
#ifdef EFRM_HAVE_USERMODEHELPER_SETUP
  return ci_call_usermodehelper_setup(path, argv, envp, wait);
#else
  return ci_call_usermodehelper_fns(path, argv, envp, wait);
#endif
}

#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0) */
/*! \cidoxg_end */
