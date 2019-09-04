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
** \author  djr
**  \brief  Tools for testing onload.
**   \date  2009/05/13
**    \cop  (c) Solarflare Communications Inc.
** </L5_PRIVATE>
*//*
\**************************************************************************/

/*! \cidoxg_lib_ciapp */
#include <ci/app.h>
#include <ci/app/onload.h>


#if !defined(_WIN32) && !defined(__MACH__)
/* If the onload library is present, and defines onload_version, then this
 * will resolve to the onload library.  Otherwise &onload_version will be
 * null (because it is weak and undefined).
 */
extern const char*const onload_version __attribute__((weak));
#endif
extern char** environ;


int ci_onload_is_active(void)
{
#if !defined(_WIN32) && !defined(__MACH__)
  const char* ld_preload;
  if( &onload_version )
    return 1;
  ld_preload = getenv("LD_PRELOAD");
  if( ld_preload == NULL )
    return 0;
  return strstr(ld_preload, "libcitransport") != NULL
    ||   strstr(ld_preload, "libonload") != NULL;
#else
  return 0;
#endif
}


void ci_onload_info_dump(FILE* f, const char* pf)
{
#if !defined(_WIN32) && !defined(__MACH__)
  const char* ld_preload;
  char** p;

  ld_preload = getenv("LD_PRELOAD");
  if( ld_preload )
    fprintf(f, "%sLD_PRELOAD=%s\n", pf, ld_preload);
  if( &onload_version )
    fprintf(f, "%sonload_version=%s\n", pf, onload_version);
  if( ci_onload_is_active() )
    for( p = environ; *p != NULL; ++p )
      if( strncmp("EF_", *p, 3) == 0 )
        fprintf(f, "%s%s\n", pf, *p);
#endif
}
