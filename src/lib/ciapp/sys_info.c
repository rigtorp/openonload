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
*//*! \file
** <L5_PRIVATE L5_SOURCE>
** \author  djr
**  \brief  Tools for test apps.
**   \date  2009/05/15
**    \cop  (c) Solarflare Communications Inc.
** </L5_PRIVATE>
*//*
\**************************************************************************/

/* Following needed for asprintf */
#define _GNU_SOURCE 1
#include <stdio.h>

#include <ci/app.h>


#include <sys/utsname.h>
#include <sys/types.h>
#include <dirent.h>
#include <libgen.h>


void ci_app_grep(const char *label, const char *file, size_t map_len, char *find) {
  char *buf, *p1, *p2;
  int fd;

  if ((fd = open(file, O_RDONLY)) == -1)
    return;

  if (!(buf = malloc(map_len+1)))
      return;
  buf[map_len] = 0;

  read(fd, buf, map_len);
  if ((p1 = strstr(buf, find))) {
    if ((p2 = strstr(p1 ,"\n"))) {
      printf("%s %.*s\n", label, (int)(p2-p1), p1);
    }
  }
  free(buf);
  close(fd);
}

void ci_app_dump_sys_info(void)
{
  char date[30];
  char *cmd;
  char name[256];
  char *base;
  int rc;
  time_t tt;
  struct tm *tm;
  struct utsname un;
  DIR *dir;

  if( ci_cmdline )
    printf("# cmdline: %s\n", ci_cmdline);

  tt = time(NULL);
  tm = localtime(&tt);
  if (strftime(date, sizeof(date), "%a %b %d %H:%M:%S %Z %Y", tm))
    printf("# date: %s\n", date);

  if (uname(&un) >= 0)
    printf("# uname: %s %s %s %s %s\n", un.sysname, un.nodename, un.release,
	   un.version, un.machine);

  ci_app_grep("# cpu: ", "/proc/cpuinfo", 1024, "model name");
  ci_app_grep("# ram: ", "/proc/meminfo", 1024, "MemTotal");

  if ((dir = opendir("/sys/class/net/")) != 0) {
    struct dirent *ent;
    while ((ent = readdir(dir))) {
      CI_TRY(asprintf(&cmd, "/sys/class/net/%s/device/driver", 
                      ent->d_name));
      rc = readlink(cmd, name, 255);
      if (rc != -1) {
        /* readlink does not NULL terminate string */
        name[rc] = '\0';
        base = basename(name);
        if (strncmp(base, "sfc", 4) == 0)
          printf("# sfnics: %s\n", ent->d_name);
      }
      free(cmd);
    }
    closedir(dir);
  }
}
