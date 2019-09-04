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

/**************************************************************************\
*//*! \file
** <L5_PRIVATE L5_SOURCE>
** \author  
**  \brief  
**   \date  
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/
  
/*! \cidoxg_lib_citools */

#include "citools_internal.h"
#include <stdarg.h>


#ifndef  CI_LOG_PREFIX_DEFAULT 
# define CI_LOG_PREFIX_DEFAULT  "ci "
#endif

#ifndef  CI_LOG_LEVEL_DEFAULT 
# define CI_LOG_LEVEL_DEFAULT   1  /* critical */
#endif

#ifndef  CI_LOG_FN_DEFAULT
# define CI_LOG_FN_DEFAULT  ci_log_stderr
#endif

void (*ci_log_fn)(const char* msg) = CI_LOG_FN_DEFAULT;
int    ci_log_level                = CI_LOG_LEVEL_DEFAULT;
int    ci_log_options		   = 0;

const char* ci_log_prefix     = CI_LOG_PREFIX_DEFAULT;
static int ci_log_prefix_len = sizeof(CI_LOG_PREFIX_DEFAULT) - 1;


void ci_vlog(const char* fmt, va_list args)
{
  int n = 0;
  char line[CI_LOG_MAX_LINE];

  ci_assert(ci_log_prefix);
  ci_assert(fmt);

  if( ci_log_options ) {
#if defined(__linux__) && defined(__KERNEL__)
    if( ci_log_options & CI_LOG_CPU )
      n += ci_sprintf(line + n, "%d ", (int) smp_processor_id());
    if( ci_log_options & CI_LOG_PID )
      n += ci_sprintf(line + n, "%d ",
                      in_interrupt() ? 0 : (int) current->tgid);
    if( ci_log_options & CI_LOG_TID )
      n += ci_sprintf(line + n, "%d ",
                      in_interrupt() ? 0: (int) current->pid);
#elif defined(__FreeBSD__) && !defined(__KERNEL__)
    if( ci_log_options & CI_LOG_PID )
      n += ci_sprintf(line + n, "%d ", (int) getpid());
    if( ci_log_options & CI_LOG_TID )
      n += ci_sprintf(line + n, "%ld ", (long) pthread_self());
#elif defined(__unix__) && !defined(__KERNEL__)
    if( ci_log_options & CI_LOG_PID )
      n += ci_sprintf(line + n, "%d ", (int) getpid());
    if( ci_log_options & CI_LOG_TID )
      n += ci_sprintf(line + n, "%lx ", (long) pthread_self());
#endif
#ifdef CI_HAVE_FRC64
    if( ci_log_options & CI_LOG_TIME )
      n += ci_sprintf(line + n, "%010"CI_PRIu64" ",
		   (ci_uint64) (ci_frc64_get() & 0xffffffffffull));
#elif defined(CI_HAVE_FRC32)
    if( ci_log_options & CI_LOG_TIME )
      n += ci_sprintf(line + n, "%010u ", (unsigned) ci_frc32_get());
#endif
    if( ci_log_options & CI_LOG_DELTA ) {
      static ci_uint32 prev = 0;
      ci_uint32 now = ci_frc32_get();
      n += ci_sprintf(line + n, "%06u ", (unsigned) now - prev);
      prev = now;
    }
  }

  memcpy(line + n, ci_log_prefix, ci_log_prefix_len);
#ifdef CI_HAVE_NPRINTF
  vsnprintf(line + n + ci_log_prefix_len,
	    CI_LOG_MAX_LINE - ci_log_prefix_len - n, fmt, args);
#else
  {
    int len;
    len = ci_vsprintf(line + n + ci_log_prefix_len, fmt, args);
    if (len+1 > CI_LOG_MAX_LINE - ci_log_prefix_len - n) {
#if defined(__linux__) && !defined(__KERNEL__)
      printk(KERN_ERR "ci_vlog: too long %d : %50s", len, line);
#endif
      CI_BOMB();
    }
  }
#endif

  ci_log_fn(line);
}


void ci_log(const char* fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  ci_vlog(fmt, args);
  va_end(args);
}

/* Wrapper to make ci_log conform to the signature of an oo_dump_log_fn_t. */
void ci_log_dump_fn(void* unused, const char* fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  ci_vlog(fmt, args);
  va_end(args);
}


void ci_set_log_prefix(const char* prefix)
{
  if( !prefix ) {
    ci_log_prefix = CI_LOG_PREFIX_DEFAULT;
    return;
  }

  ci_assert(strlen(prefix) < CI_LOG_MAX_LINE);

  ci_log_prefix = prefix;

  ci_log_prefix_len = strlen(ci_log_prefix);
}


void ci_rlvlog(int* limit, const char* fmt, va_list args)
{
  if( *limit <= 0 )
    return;
  ci_vlog(fmt, args);
  if( --(*limit) == 0 )
    ci_log("... message limit reached");
}

void ci_rllog(int* limit, const char* fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  ci_rlvlog(limit, fmt, args);
  va_end(args);
}


/*! \cidoxg_end */
