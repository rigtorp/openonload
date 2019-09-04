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

#ifndef __KERNEL__

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <errno.h>
#include <string.h>

#include <ci/tools/namespace.h>

/* Compare whether network namespace of the given file is
 * the same as the one of the running thread */
int ci_check_net_namespace(const char* ns_filename)
{
  struct stat ours, theirs;
  char fname[32];

  if( stat(ns_filename, &theirs) < 0 )
    return -errno;
  int tid = syscall(__NR_gettid);
  snprintf(fname, sizeof(fname), "/proc/%d/ns/net", tid);
  if( stat(fname, &ours) < 0 )
    return -errno;
  return theirs.st_ino == ours.st_ino;
}


int ci_switch_net_namespace(const char* ns_filename)
{
#ifndef __NR_setns
  return -ENOSYS;
#else
  int fd;
  int rc;

  fd = open(ns_filename, O_RDONLY);
  if( fd < 0 )
    return -errno;

  /* Recent libcs provide setns(), but there are several distro kernels (recent
   * RHEL6, Debian 7) that have the syscall despite the distro having an old
   * libc, so we use the syscall directly. */
  rc = syscall(__NR_setns, fd, CLONE_NEWNET);
  close(fd);

  if( rc < 0 )
    return -errno;

  return 0;
#endif
}

#endif /* ndef __KERNEL__ */
