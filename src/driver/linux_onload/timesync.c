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

#include <driver/linux_onload/onload_kernel_compat.h>

#include <onload/debug.h>
#include <onload/tcp_helper_fns.h>

/*****************************************************************************
 *          oo_timesync state                                                *
 *****************************************************************************/

#ifdef __KERNEL__
/* module parameter:  jiffies between synchronisation times */
static int timesync_period = 500;
module_param(timesync_period, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(timesync_period,
                 "Period in milliseconds between synchronising the Onload"
                 "clock with the system clock");



/* Maintains an estimate of cpu frequency.  WARNING!!! You need to use
 * oo_timesync_wait_for_cpu_khz_to_stabilize() before reading this.
 * Note that this can take a long time to stabilize (order of ms).
 * Note that it is probably not a good idea to block on it too early
 * e.g. at module initialisation.
 */
unsigned oo_timesync_cpu_khz;

/* Internal, do not use!  Use
 *  oo_timesync_wait_for_cpu_khz_to_stabilize() instead.  Signal sent
 *  when equal to 2. */
static int signal_cpu_khz_stabilized = 0;
static struct timer_list timer_node;


DECLARE_COMPLETION(cpu_khz_stabilized_completion);


/* Look at comments above oo_timesync_cpu_khz */
void oo_timesync_wait_for_cpu_khz_to_stabilize(void)
{
  wait_for_completion(&cpu_khz_stabilized_completion);
}


#if BITS_PER_LONG != 64
/* The following division functions are a simplied version of the
 * algorithm found in
 * http://www.hackersdelight.org/HDcode/newCode/divDouble.c.txt */

/* Divide 64 bits dividend and 32 bits divisor and return 32 bits
 * quotient */
static ci_uint32 div_64dd_32ds_32qt(ci_uint64 dividend, ci_uint32 divisor)
{
  ci_uint32 low, high, quotient = 0, c = 32;
  ci_uint64 d = (ci_uint64)divisor << 31;

  low = dividend & 0xffffffff;
  high = dividend >> 32;

  while( dividend > 0xffffffff ) {
    quotient <<= 1;
    if( dividend >= d ) {
      dividend -= d;
      quotient |= 1;
    }
    d >>= 1;
    c--;
  }
  quotient <<= c;
  if( ! dividend )
    return quotient;
  low = dividend;
  return quotient | (low / divisor);
}


/* Divide 64 bits dividend and 32 bits divisor and return 64 bits
 * quotient */
static ci_uint64 div_64dd_32ds_64qt(ci_uint64 dividend, ci_uint32 divisor)
{
  ci_uint32 low, high, high1;

  low = dividend & 0xffffffff;
  high = dividend >> 32;

  if( ! high )
    return low / divisor;

  high1 = high % divisor;
  high /= divisor;
  low = div_64dd_32ds_32qt((ci_uint64)high1 << 32 | low, divisor);

  return (ci_uint64)high << 32 | low;
}
#endif


/* Divide 64 bits dividend and 64 bits divisor and return 64 bits
 * quotient */
static ci_uint64 div_64dd_64ds_64qt(ci_uint64 dividend, ci_uint64 divisor)
{
#if BITS_PER_LONG == 64
  return dividend / divisor;
#else
  ci_uint32 high;
  ci_uint64 quotient;
  int n;

  high = divisor >> 32;
  if( ! high )
    return div_64dd_32ds_64qt(dividend, divisor);

  n = 1 + fls(high);
  quotient = div_64dd_32ds_64qt(dividend >> n, divisor >> n);

  if( quotient != 0 )
    quotient--;
  if( (dividend - quotient * divisor) >= divisor )
    quotient++;
  return quotient;
#endif
}


static void oo_timesync_stabilize_cpu_khz(struct oo_timesync* oo_ts)
{
  static int cpu_khz_warned = 0;

  /* Want at least two data points in oo_ts (oo_timesync_update called
   * twice) before computing cpu_khz */
  if( signal_cpu_khz_stabilized == 0 ) {
    ++signal_cpu_khz_stabilized;
    return;
  }

  /* Current oo_timesync implementation guarantees smoothed_ns <
   * 16*(10**10).  uint64 will give us at least 10**18.  If
   * smoothed_ticks is in same order as smoothed_ns, then we can
   * multiply by 10**6 without dange of overflow.  This is better than
   * dividing first as that can introduce large errors when
   * smoothed_ticks is in the same order as smoothed_ns.  Note: we
   * cannot use doubles in kernel. */
  oo_timesync_cpu_khz = div_64dd_64ds_64qt((ci_uint64)oo_ts->smoothed_ticks *
                                           1000000, oo_ts->smoothed_ns);

  /* Warn if the oo_timesync_cpu_khz computation over or under flowed. */
  if( oo_timesync_cpu_khz < 400000 || oo_timesync_cpu_khz > 10000000 )
    if( ! cpu_khz_warned ) {
      cpu_khz_warned = 1;
      ci_log("WARNING: cpu_khz computed to be %d which may not be correct\n",
             oo_timesync_cpu_khz);
    }

  if( signal_cpu_khz_stabilized == 1 ) {
    complete_all(&cpu_khz_stabilized_completion);
    ++signal_cpu_khz_stabilized;
  }
}

/* Linux 4.15 changed the argument type for the timer callback
 * function */
#ifdef EFRM_HAVE_TIMER_CALLBACK_TIMERLIST
static void stabilize_cpu_khz_timer(struct timer_list *unused)
#else
static void stabilize_cpu_khz_timer(unsigned long unused)
#endif
{
  oo_timesync_update(efab_tcp_driver.timesync);
  /* If oo_timesync_update called too soon.  Start timer
   * again. oo_timesync_update is subsequently called from elsewhere -
   * this is just to get the first couple of datapoints
   */
  if( signal_cpu_khz_stabilized != 2 )
    mod_timer(&timer_node, jiffies + HZ / 2);
}


static spinlock_t timesync_lock;

int oo_timesync_ctor(struct oo_timesync *oo_ts)
{
  ci_uint64 now_frc;
  struct timespec mono_now, wall_now;

  ci_frc64(&now_frc);
  ktime_get_ts(&mono_now);
  getnstimeofday(&wall_now);

  oo_ts->wall_clock.tv_sec = wall_now.tv_sec;
  oo_ts->wall_clock.tv_nsec = wall_now.tv_nsec;
  oo_ts->mono_clock.tv_sec = mono_now.tv_sec;
  oo_ts->mono_clock.tv_nsec = mono_now.tv_nsec;
  oo_ts->clock_made = now_frc;

  /* Set to zero to prevent smoothing when first set */
  oo_ts->smoothed_ticks = 0;
  oo_ts->smoothed_ns = 0;
  oo_ts->generation_count = 0;
  oo_ts->update_jiffies = jiffies - 1;

  spin_lock_init(&timesync_lock);

  timer_setup(&timer_node, stabilize_cpu_khz_timer, 0);
  timer_node.expires = jiffies + 1;
  add_timer(&timer_node);

  return 0;
}


void oo_timesync_dtor(struct oo_timesync *oo_ts)
{
  /* ensure no one is blocked waiting */
  complete_all(&cpu_khz_stabilized_completion);
  /* ensure the timer is gone */
  signal_cpu_khz_stabilized = 2;
  ci_wmb();
  del_timer_sync(&timer_node);
}

#define TIMESYNC_SMOOTH_SAMPLES 16
#define TIMESYNC_SMOOTH_SAMPLES_MASK 0xf
static ci_uint64 timesync_smooth_tick_samples[TIMESYNC_SMOOTH_SAMPLES];
static ci_uint64 timesync_smooth_ns_samples[TIMESYNC_SMOOTH_SAMPLES];
static int timesync_smooth_i = 0;

#define ONE_SECOND_IN_NS 1000000000llu
#define TEN_SECOND_IN_NS 10000000000llu

void oo_timesync_update(struct oo_timesync *oo_ts)
{
  ci_uint64 frc, ticks, ns;
  struct timespec mono_ts, wall_ts;
  int use_this_sample = 1;

  if( time_after(jiffies, (unsigned long)oo_ts->update_jiffies) ) {
    spin_lock_bh(&timesync_lock);
    /* Re-check incase it was updated while we waited for the lock */
    if( time_after(jiffies, (unsigned long)oo_ts->update_jiffies) ) {
      ci_frc64(&frc);
      /* We need to use two system clocks: one monotonic
       * (ktime_get_ts()) to provide accurate estimates of time
       * deltas, and one wall clock (getnstimeofday()) for using as a
       * basis for reporting timestamps through the APIs
       *
       * In future getmonotonicraw() might be better than
       * ktime_get_ts() (it doesn't include NTP adjustments) but it's
       * not available on older (RHEL5) kernels
       */
      ktime_get_ts(&mono_ts);
      getnstimeofday(&wall_ts);

      /* FRC ticks since last update */
      ticks = frc - oo_ts->clock_made;

      /* Nanoseconds since last update */
      if( mono_ts.tv_sec == oo_ts->mono_clock.tv_sec &&
          mono_ts.tv_nsec > oo_ts->mono_clock.tv_nsec ) {
        ns = mono_ts.tv_nsec - oo_ts->mono_clock.tv_nsec;
        ci_assert_lt(ns, ONE_SECOND_IN_NS);
      }
      else if( mono_ts.tv_sec > oo_ts->mono_clock.tv_sec ) {
        ci_assert_le(oo_ts->mono_clock.tv_nsec, ONE_SECOND_IN_NS);
        ns = mono_ts.tv_nsec + (ONE_SECOND_IN_NS - oo_ts->mono_clock.tv_nsec) +
          (mono_ts.tv_sec - oo_ts->mono_clock.tv_sec - 1) * ONE_SECOND_IN_NS;

        if( ns > TEN_SECOND_IN_NS ) {
          /* We've seen a big gap, which is suspicious (e.g. clock has
           * jumped), so don't include this period in the smoothing
           * state.
           *
           * There could just be a big gap due to no stacks existing,
           * so don't shout about it.
           */
          use_this_sample = 0;
        }
      } 
      else {
        /* Time has gone backwards. Work around this by not taking a
         * sample, but updating state about time clock made so that
         * next time we update we'll (hopefully) get a better estimate.
         *
         * This really shouldn't happen: ktime_get_ts() is monotonic
         */
        OO_DEBUG_VERB(ci_log("%s: time has jumped backwards, ignoring sample",
                             __FUNCTION__));
        ns = 0; /* Keep compiler happy */
        use_this_sample = 0;
      }

      ++oo_ts->generation_count;
      ci_wmb();

      if( use_this_sample ) {
        /* We used to scale ns and ticks down to guarantee this, but
         * now shouldn't get here if there has been a big gap.
         */
        ci_assert_le(ns, TEN_SECOND_IN_NS);

        oo_ts->smoothed_ticks += ticks;
        oo_ts->smoothed_ticks -=
          timesync_smooth_tick_samples[timesync_smooth_i];
        timesync_smooth_tick_samples[timesync_smooth_i] = ticks;
        oo_ts->smoothed_ns += ns;
        oo_ts->smoothed_ns -= timesync_smooth_ns_samples[timesync_smooth_i];
        timesync_smooth_ns_samples[timesync_smooth_i] = ns;
        timesync_smooth_i =
          (timesync_smooth_i + 1) & TIMESYNC_SMOOTH_SAMPLES_MASK;
      }

      /* Store the current times as a baseline for next sample */
      oo_ts->wall_clock.tv_sec = wall_ts.tv_sec;
      oo_ts->wall_clock.tv_nsec = wall_ts.tv_nsec;
      oo_ts->mono_clock.tv_sec = mono_ts.tv_sec;
      oo_ts->mono_clock.tv_nsec = mono_ts.tv_nsec;
      oo_ts->clock_made = frc;

      oo_ts->update_jiffies = jiffies + msecs_to_jiffies(timesync_period);

      ci_wmb();

      /* Avoid zero for generation count as that is special value for
       * "not yet initialized"
       */
      if( oo_ts->generation_count + 1 == 0 )
        oo_ts->generation_count = 2;
      else
        ++oo_ts->generation_count;

      /* Only consider this a useful sample if we were able to use the sample
       */
      if( use_this_sample )
        oo_timesync_stabilize_cpu_khz(oo_ts);
    }
    spin_unlock_bh(&timesync_lock);
  }
}
#endif

