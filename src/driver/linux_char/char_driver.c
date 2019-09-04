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

/**************************************************************************\
*//*! \file char_driver.c OS interface to driver
** <L5_PRIVATE L5_SOURCE>
** \author  ok_sasha
**  \brief  Package - driver/linux	Linux driver support
**     $Id$
**   \date  2002/08
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/
  
/*! \cidoxg_driver_linux */
 

/*--------------------------------------------------------------------
 *
 * Compile time assertions for this file
 *
 *--------------------------------------------------------------------*/

#define __ci_driver_shell__	/* implements driver to kernel interface */

/*--------------------------------------------------------------------
 *
 * CI headers
 *
 *--------------------------------------------------------------------*/

#include "linux_char_internal.h"
#include <ci/efhw/public.h>
#include <ci/efrm/efrm_client.h>
#include <ci/efch/op_types.h>
#include "char_internal.h"
#include <linux/init.h>


int phys_mode_gid = 0;
module_param(phys_mode_gid, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(phys_mode_gid,
                 "Group id of ef_vi apps which may use physical buffer mode.  "
                 "0 is default.  "
		 "-1 means \"any user may use physical buffer mode\".  "
		 "-2 means means \"physical buffer mode forbidden\".");


MODULE_AUTHOR("Solarflare Communications");
MODULE_LICENSE("GPL");




/*--------------------------------------------------------------------
 *
 * Driver log/debug settings, exported to dependent modules (ip)
 *
 *--------------------------------------------------------------------*/


/************************************
 * IOCTL                            *
 ************************************/

ci_noinline int
ioctl_resource_alloc (ci_private_char_t *priv, ulong arg)
{
  int rc;
  ci_resource_alloc_t local;
  copy_from_user_ret(&local, (caddr_t) arg, sizeof(local), -EFAULT);
  rc = efch_resource_alloc(&priv->rt, &local);
  if( rc < 0 )  return rc;
  copy_to_user_ret((caddr_t) arg, &local, sizeof(local), -EFAULT);
  return rc;
}

ci_noinline int
ioctl_resource_op (ci_private_char_t *priv, ulong arg)
{
  ci_resource_op_t local;
  int rc, copy_out = 0;
  copy_from_user_ret(&local, (caddr_t) arg, sizeof(local), -EFAULT);
  rc = efch_resource_op(&priv->rt, &local, &copy_out);
  if( copy_out )
    copy_to_user_ret((caddr_t) arg, &local, sizeof(local), -EFAULT);

  return rc;
}

ci_noinline int
ioctl_license_challenge (ci_private_char_t *priv, ulong arg)
{
  struct ci_license_challenge_op_s local;
  int rc, copy_out = 0;
  copy_from_user_ret(&local, (caddr_t) arg, sizeof(local), -EFAULT);
  rc = efch_license_challenge(&priv->rt, &local, &copy_out);
  if( copy_out )
    copy_to_user_ret((caddr_t) arg, &local, sizeof(local), -EFAULT);

  return rc;
}

ci_noinline int
ioctl_resource_prime (ci_private_char_t *priv, ulong arg)
{
  ci_resource_prime_op_t local;
  copy_from_user_ret(&local, (caddr_t) arg, sizeof(local), -EFAULT);
  return efch_vi_prime(priv, local.crp_id, local.crp_current_ptr);
}

ci_noinline int
ioctl_filter_add (ci_private_char_t *priv, ulong arg)
{
  ci_filter_add_t local, *filter_add;
  uint16_t in_len, out_size;
  int rc, copy_out = 0;
  filter_add = (ci_filter_add_t *)arg;
  if( get_user(in_len, &(filter_add->in.in_len)) )
    return -EFAULT;
  memset(&local, 0, sizeof(local));
  copy_from_user_ret(&local, filter_add, min_t(uint16_t, sizeof(local), in_len),
                     -EFAULT);
  out_size = local.in.out_size;
  rc = efch_filter_add(&priv->rt, &local, &copy_out);
  if( copy_out )
    copy_to_user_ret(filter_add, &local, min(local.out.out_len, out_size),
                     -EFAULT);

  return rc;
}


ci_noinline int
ioctl_capabilities_op (ci_private_char_t *priv, ulong arg)
{
  struct efch_capabilities_in in;
  struct efch_capabilities_out out;
  int rc;

  copy_from_user_ret(&in, (caddr_t) arg, sizeof(in), -EFAULT);

  rc = efch_capabilities_op(&in, &out);

  if( rc == 0 )
    copy_to_user_ret((caddr_t) arg, &out, sizeof(out), -EFAULT);

  return rc;
}

ci_noinline int
ioctl_v3_license_challenge (ci_private_char_t *priv, ulong arg)
{
  struct ci_v3_license_challenge_op_s local;
  int rc, copy_out = 0;
  copy_from_user_ret(&local, (caddr_t) arg, sizeof(local), -EFAULT);
  rc = efch_v3_license_challenge(&priv->rt, &local, &copy_out);
  if( (rc == 0) && copy_out )
    copy_to_user_ret((caddr_t) arg, &local, sizeof(local), -EFAULT);

  return rc;
}


static long
ci_char_fop_ioctl(struct file *filp, uint cmd, ulong arg) 
{ 
  ci_private_char_t *priv = (ci_private_char_t *) filp->private_data;

  switch (cmd) {
  case CI_RESOURCE_OP:
    return ioctl_resource_op (priv, arg);

  case CI_RESOURCE_ALLOC:
    return ioctl_resource_alloc (priv, arg);

  case CI_LICENSE_CHALLENGE:
    return ioctl_license_challenge (priv, arg);

  case CI_RESOURCE_PRIME:
    return ioctl_resource_prime (priv, arg);

  case CI_FILTER_ADD:
    return ioctl_filter_add (priv, arg);

  case CI_CAPABILITIES_OP:
    return ioctl_capabilities_op (priv, arg);

  case CI_V3_LICENSE_CHALLENGE:
    return ioctl_v3_license_challenge (priv, arg);

    default:
    ci_log("unknown ioctl (%u)", cmd);
    return -ENOTTY;

  }
  return 0;
}


/****************************************************************************
 *
 * open - create a new file descriptor and hang private state
 *
 ****************************************************************************/
static int
ci_char_fop_open(struct inode *inode, struct file *filp)
{
  ci_private_char_t *priv;

  EFCH_TRACE("%s:", __FUNCTION__);

  if ((priv = CI_ALLOC_OBJ(ci_private_char_t)) == NULL)
    return -ENOMEM;
  CI_ZERO(priv);
  /* priv->cpcp_vi = NULL; */
  init_waitqueue_head(&priv->cpcp_poll_queue);
  ci_resource_table_ctor(&priv->rt,
            ci_is_sysadmin() ? CI_CAP_BAR | CI_CAP_PHYS | CI_CAP_DRV : 0);
  filp->private_data = (void*) priv;
  return 0; 
}

/****************************************************************************
 *
 * close - cleanup filedescriptor and private state
 *
 ****************************************************************************/
static int
ci_char_fop_close(struct inode *inode, struct file *filp) 
{  
  ci_private_char_t *priv = (ci_private_char_t *) filp->private_data;

  EFCH_TRACE("%s:", __FUNCTION__);

  /* cleanup private state */
  filp->private_data = 0;
  ci_resource_table_dtor(&priv->rt);
  ci_free(priv);

  return 0;  
} 


static unsigned ci_char_fop_poll(struct file* filp, poll_table* wait)
{
  ci_private_char_t *priv = (ci_private_char_t *) filp->private_data;
  return efch_vi_poll(priv, filp, wait);
}


/*--------------------------------------------------------------------
 *
 * char device interface
 *
 *--------------------------------------------------------------------*/

struct file_operations ci_char_fops = {
  .owner = THIS_MODULE,
  .unlocked_ioctl = ci_char_fop_ioctl,
  .compat_ioctl = ci_char_fop_ioctl,
  .mmap = ci_char_fop_mmap,
  .open = ci_char_fop_open,
  .release = ci_char_fop_close,
  .poll = ci_char_fop_poll,
};

static int ci_char_major = 0;


/************************************************
 * Init/destroy module functions                *
 ************************************************/

static int init_etherfabric_char(void)
{
  int rc = 0;
  int major = 0; /* specify default major number here */

  ci_set_log_prefix("[sfc_char] ");

  ci_mm_tbl_init();

  if ((rc = register_chrdev(major, EFAB_CHAR_NAME, &ci_char_fops)) < 0) {
    ci_log("%s: can't register char device %d", EFAB_CHAR_NAME, rc);
    return rc;
  }

  if (major == 0)
    major = rc;

  ci_char_major = major;

  return 0;
}

/**************************************************************************** 
 * 
 * close_driver: unregister the character device and the PCI driver
 * 
 ****************************************************************************/ 
static void 
cleanup_etherfabric_char(void) 
{ 
  if (ci_char_major) 
    unregister_chrdev(ci_char_major, EFAB_CHAR_NAME);
}

module_init(init_etherfabric_char);
module_exit(cleanup_etherfabric_char);

