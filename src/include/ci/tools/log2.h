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


#ifndef __CI_TOOLS_LOG2_H__
#define __CI_TOOLS_LOG2_H__

/**********************************************************************
 * powers of two
 **********************************************************************/ 

/* The smallest [o] such that (1u << o) >= n. */

ci_inline unsigned ci_log2_ge(unsigned long n, unsigned min_order) {
  unsigned order = min_order;
  while( (1ul << order) < n )  ++order;
  return order;
}


/* The smallest [o] such that (1u << o) > n. */

ci_inline unsigned ci_log2_g(unsigned long n, unsigned min_order) {
  unsigned order = min_order;
  while( (1ul << order) <= n )  ++order;
  return order;
}


/* The largest [o] such that (1u << o) <= n.  Requires n > 0. */

ci_inline unsigned ci_log2_le(unsigned long n) {
  unsigned order = 1;
  while( (1ul << order) <= n )  ++order;
  return (order - 1);
}

ci_inline unsigned long
ci_pow2(unsigned order) {
  return (1ul << order);
}

#define CI_IS_POW2(x)  ((x) && ! ((x) & ((x) - 1)))


#endif /* __CI_TOOLS_LOG2_H__ */
