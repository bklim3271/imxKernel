// SPDX-License-Identifier: GPL-2.0
/*
 * Fast Ethernet Controller (ENET) PTP driver for MX6x.
 *
 * Copyright (C) 2012 Freescale Semiconductor, Inc.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/fec.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_net.h>

#include "fec.h"

/* FEC 1588 register bits */
#define FEC_T_CTRL_SLAVE                0x00002000
#define FEC_T_CTRL_CAPTURE              0x00000800
#define FEC_T_CTRL_RESTART              0x00000200
#define FEC_T_CTRL_PERIOD_RST           0x00000030
#define FEC_T_CTRL_PERIOD_EN		0x00000010
#define FEC_T_CTRL_ENABLE               0x00000001

#define FEC_T_INC_MASK                  0x0000007f
#define FEC_T_INC_OFFSET                0
#define FEC_T_INC_CORR_MASK             0x00007f00
#define FEC_T_INC_CORR_OFFSET           8

#define FEC_T_CTRL_PINPER		0x00000080
#define FEC_T_TF0_MASK			0x00000001
#define FEC_T_TF0_OFFSET		0
#define FEC_T_TF1_MASK			0x00000002
#define FEC_T_TF1_OFFSET		1
#define FEC_T_TF2_MASK			0x00000004
#define FEC_T_TF2_OFFSET		2
#define FEC_T_TF3_MASK			0x00000008
#define FEC_T_TF3_OFFSET		3
#define FEC_T_TDRE_MASK			0x00000001
#define FEC_T_TDRE_OFFSET		0
#define FEC_T_TMODE_MASK		0x0000003C
#define FEC_T_TMODE_OFFSET		2
#define FEC_T_TIE_MASK			0x00000040
#define FEC_T_TIE_OFFSET		6
#define FEC_T_TF_MASK			0x00000080
#define FEC_T_TF_OFFSET			7

#define FEC_ATIME_CTRL		0x400
#define FEC_ATIME		0x404
#define FEC_ATIME_EVT_OFFSET	0x408
#define FEC_ATIME_EVT_PERIOD	0x40c
#define FEC_ATIME_CORR		0x410
#define FEC_ATIME_INC		0x414
#define FEC_TS_TIMESTAMP	0x418

#define FEC_TGSR		0x604
#define FEC_TCSR(n)		(0x608 + n * 0x08)
#define FEC_TCCR(n)		(0x60C + n * 0x08)
#define MAX_TIMER_CHANNEL	3
#define FEC_TMODE_TOGGLE	0x05
#define FEC_HIGH_PULSE		0x0F
#define FEC_CC_MULT	(1 << 31)
#define FEC_COUNTER_PERIOD	(1 << 31)
#define PPS_OUPUT_RELOAD_PERIOD	NSEC_PER_SEC
#define FEC_CHANNLE_0		0
#define DEFAULT_PPS_CHANNEL	FEC_CHANNLE_0

/**
 * fec_ptp_enable_pps
 * @fep: the fec_enet_private structure handle
 * @enable: enable the channel pps output
 *
 * This function enble the PPS ouput on the timer channel.
 */
static int fec_ptp_enable_pps(struct fec_enet_private *fep, uint enable)
{
	unsigned long flags;
	u32 val, tempval;
	struct timespec64 ts;
	u64 ns;
	val = 0;

	if (fep->pps_enable == enable)
		return 0;

	fep->pps_channel = DEFAULT_PPS_CHANNEL;
	fep->reload_period = PPS_OUPUT_RELOAD_PERIOD;

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);

#ifdef CONFIG_AVB_SUPPORT
	if (fep->rec_enable && (fep->pps_channel == fep->rec_channel)) {
		raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);
		return -EBUSY;
	}
#endif

	if (enable) {
		/* clear capture or output compare interrupt status if have.
		 */
		writel(FEC_T_TF_MASK, fep->hwp + FEC_TCSR(fep->pps_channel));

		/* It is recommended to double check the TMODE field in the
		 * TCSR register to be cleared before the first compare counter
		 * is written into TCCR register. Just add a double check.
		 */
		val = readl(fep->hwp + FEC_TCSR(fep->pps_channel));
		do {
			val &= ~(FEC_T_TMODE_MASK);
			writel(val, fep->hwp + FEC_TCSR(fep->pps_channel));
			val = readl(fep->hwp + FEC_TCSR(fep->pps_channel));
		} while (val & FEC_T_TMODE_MASK);

		/* Dummy read counter to update the counter */
		timecounter_read(&fep->tc);
		/* We want to find the first compare event in the next
		 * second point. So we need to know what the ptp time
		 * is now and how many nanoseconds is ahead to get next second.
		 * The remaining nanosecond ahead before the next second would be
		 * NSEC_PER_SEC - ts.tv_nsec. Add the remaining nanoseconds
		 * to current timer would be next second.
		 */
		tempval = readl(fep->hwp + FEC_ATIME_CTRL);
		tempval |= FEC_T_CTRL_CAPTURE;
		writel(tempval, fep->hwp + FEC_ATIME_CTRL);

		tempval = readl(fep->hwp + FEC_ATIME);
		/* Convert the ptp local counter to 1588 timestamp */
		ns = timecounter_cyc2time(&fep->tc, tempval);
		ts = ns_to_timespec64(ns);

		/* The tempval is  less than 3 seconds, and  so val is less than
		 * 4 seconds. No overflow for 32bit calculation.
		 */
		val = NSEC_PER_SEC - (u32)ts.tv_nsec + tempval;

		/* Need to consider the situation that the current time is
		 * very close to the second point, which means NSEC_PER_SEC
		 * - ts.tv_nsec is close to be zero(For example 20ns); Since the timer
		 * is still running when we calculate the first compare event, it is
		 * possible that the remaining nanoseonds run out before the compare
		 * counter is calculated and written into TCCR register. To avoid
		 * this possibility, we will set the compare event to be the next
		 * of next second. The current setting is 31-bit timer and wrap
		 * around over 2 seconds. So it is okay to set the next of next
		 * seond for the timer.
		 */
		val += NSEC_PER_SEC;

		/* We add (2 * NSEC_PER_SEC - (u32)ts.tv_nsec) to current
		 * ptp counter, which maybe cause 32-bit wrap. Since the
		 * (NSEC_PER_SEC - (u32)ts.tv_nsec) is less than 2 second.
		 * We can ensure the wrap will not cause issue. If the offset
		 * is bigger than fep->cc.mask would be a error.
		 */
		val &= fep->cc.mask;
		writel(val, fep->hwp + FEC_TCCR(fep->pps_channel));

		/* Calculate the second the compare event timestamp */
		fep->next_counter = (val + fep->reload_period) & fep->cc.mask;

		/* * Enable compare event when overflow */
		val = readl(fep->hwp + FEC_ATIME_CTRL);
		val |= FEC_T_CTRL_PINPER;
		writel(val, fep->hwp + FEC_ATIME_CTRL);

		/* Compare channel setting. */
		val = readl(fep->hwp + FEC_TCSR(fep->pps_channel));
		val |= (1 << FEC_T_TF_OFFSET | 1 << FEC_T_TIE_OFFSET);
		val &= ~(1 << FEC_T_TDRE_OFFSET);
		val &= ~(FEC_T_TMODE_MASK);
		val |= (FEC_HIGH_PULSE << FEC_T_TMODE_OFFSET);
		writel(val, fep->hwp + FEC_TCSR(fep->pps_channel));

		/* Write the second compare event timestamp and calculate
		 * the third timestamp. Refer the TCCR register detail in the spec.
		 */
		writel(fep->next_counter, fep->hwp + FEC_TCCR(fep->pps_channel));
		fep->next_counter = (fep->next_counter + fep->reload_period) & fep->cc.mask;
	} else {
		writel(0, fep->hwp + FEC_TCSR(fep->pps_channel));
	}

	fep->pps_enable = enable;
	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);

	return 0;
}

#ifdef CONFIG_AVB_SUPPORT
/**
 * fec_timecounter_set
 * @start_tstamp: new time in ns
 *
 * update the FEC timecounter structure to a new time, and make sure
 * the HW counter matches that value.
 */
static inline void fec_timecounter_set(struct fec_enet_private *fep,
		      u64 start_tstamp)
{
	u32 tempval;

	tempval = start_tstamp & fep->cc.mask;
	writel(tempval, fep->hwp + FEC_ATIME);
	fep->tc.cycle_last = tempval;
	fep->tc.nsec = start_tstamp;
}

#endif

/**
 * fec_ptp_read - read raw cycle counter (to be used by time counter)
 * @cc: the cyclecounter structure
 *
 * this function reads the cyclecounter registers and is called by the
 * cyclecounter structure used to construct a ns counter from the
 * arbitrary fixed point registers
 */
static u64 fec_ptp_read(const struct cyclecounter *cc)
{
	struct fec_enet_private *fep =
		container_of(cc, struct fec_enet_private, cc);
	u32 tempval;

	tempval = readl_relaxed(fep->hwp + FEC_ATIME_CTRL);
	tempval |= FEC_T_CTRL_CAPTURE;
	writel(tempval, fep->hwp + FEC_ATIME_CTRL);

	if (fep->quirks & FEC_QUIRK_BUG_CAPTURE)
		udelay(1);

	return readl_relaxed(fep->hwp + FEC_ATIME);
}

/**
 * fec_ptp_start_cyclecounter - create the cycle counter from hw
 * @ndev: network device
 *
 * this function initializes the timecounter and cyclecounter
 * structures for use in generated a ns counter from the arbitrary
 * fixed point cycles registers in the hardware.
 */
void fec_ptp_start_cyclecounter(struct net_device *ndev)
{
	struct fec_enet_private *fep = netdev_priv(ndev);
	unsigned long flags;
	int inc;

	inc = 1000000000 / fep->cycle_speed;

	/* grab the ptp lock */
	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);

	/* 1ns counter, disable correction period */
	writel_relaxed(0, fep->hwp + FEC_ATIME_CORR);
	writel_relaxed(inc << FEC_T_INC_OFFSET, fep->hwp + FEC_ATIME_INC);


#ifdef CONFIG_AVB_SUPPORT
	/* use 32-bits timer counter */
	writel_relaxed(0, fep->hwp + FEC_ATIME_EVT_PERIOD);
	writel_relaxed(FEC_T_CTRL_ENABLE, fep->hwp + FEC_ATIME_CTRL);
#else
	/* use 31-bit timer counter */
	writel_relaxed(FEC_COUNTER_PERIOD, fep->hwp + FEC_ATIME_EVT_PERIOD);
	writel_relaxed(FEC_T_CTRL_ENABLE | FEC_T_CTRL_PERIOD_RST,
		fep->hwp + FEC_ATIME_CTRL);
#endif

	memset(&fep->cc, 0, sizeof(fep->cc));
	fep->cc.read = fec_ptp_read;
#ifdef CONFIG_AVB_SUPPORT
	fep->cc.mask = CLOCKSOURCE_MASK(32);
#else
	fep->cc.mask = CLOCKSOURCE_MASK(31);
#endif
	fep->cc.shift = 31;
	fep->cc.mult = FEC_CC_MULT;

	/* reset the ns time counter */
#ifdef CONFIG_AVB_SUPPORT
	fep->tc.cc = &fep->cc;
	fec_timecounter_set(fep, 0);
#else
	timecounter_init(&fep->tc, &fep->cc, 0);
#endif

	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);
}

#ifdef CONFIG_AVB_SUPPORT
/**
 * fec_ptp_adjfreq - adjust ptp cycle frequency
 * @ptp: the ptp clock structure
 * @ppb: parts per billion adjustment from base
 *
 * Adjust the frequency of the ptp cycle counter by the
 * indicated ppb from the base frequency.
 *
 * This version adjusts the HW counter. The HW implementation results in the following equation:
 * fe * diff = ppb * (pc + 1)
 * with:
 *   . fe : ENET ref clock frequency in Hz
 *   . diff = inc_corr - inc : difference between default increment and correction increment
 *   . ppb : parts per billion adjustment from base
 *   . pc : correction period (in number of fe clock cycles)
 *
 * Limitations:
 *  . only add or remove 1ns per correction period. This will limit jitter and improve short term
 *  accuracy (in particular for trigger events during clock recovery) but increase the max possible
 *  error, since the correction period will be the shortest possible and thus may not be optimal.
 *  In the case of an adjustment of about 100ppm (which should be the max if the ref clock
 *  is within the 802.1AS spec), the max error will be about 0.2ppm with a 50MHz ref clock, and
 *  0.08ppm with a 125MHz ref clock.
 *  Long term accuracy will also be lower (20ns per 100ms @50MHz, 8ns per 100ms @125MHz), but this
 *  can be fixed by phase adjustments.
 *  . It seems not all period/correction values are valid. With a 1ns correction, all even
 *  period values return wrong timings (half the requested correction), but on the other hand odd
 *  values are not taken into account systematically by the hardware, so we choose the closest even
 *  value that matches the following equation:
 *  fe * diff = 2 * ppb * (pc + 1)
 *  . given we force abs(diff) = 1, limit max adjustment to prevent pc < 1.
 *
 *  TODO: adjust the SW counter as well so that SW can still get the highest possible accuracy?
 */
static int fec_ptp_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
	unsigned long flags;
	u32 inc, cor, pc;

	struct fec_enet_private *fep =
			container_of(ptp, struct fec_enet_private, ptp_caps);

	inc = fep->ptp_inc;
	if (ppb == 0) {
		cor = 0;
		pc = 0;
	} else {
		if (ppb < 0) {
			ppb = -ppb;
			cor = inc - 1;
		}
		else
			cor = inc + 1;

		if (ppb > fep->ptp_caps.max_adj) {
			pr_err("ppb value %d outside accepted range (max_adj = %d)", cor > inc?ppb:-ppb, ptp->max_adj);
			return -1;
		}

		pc = (((fep->cycle_speed / (2*ppb)) - 1) + 1) & ~ 0x1; // + 1) & ~ 0x1 returns the closest even value
	}
//	printk("ptp_adjfreq: ppb = %d pc = 0x%x cor = %d\n", cor > inc?ppb:-ppb, pc, cor);

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);
	/*
	 * dummy read kept for reliability.
	 */
//	timecounter_read(&fep->tc);

	writel_relaxed((cor << FEC_T_INC_CORR_OFFSET) | (inc << FEC_T_INC_OFFSET), fep->hwp + FEC_ATIME_INC);
	writel_relaxed(pc, fep->hwp + FEC_ATIME_CORR);

	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);

	return 0;
}

/**
 * fec_ptp_adjtime
 * @ptp: the ptp clock structure
 * @delta: offset to adjust the cycle counter by
 *
 * adjust the timer by updating the HW counter AND the timecounter structure
 * as well. Since updating the HW register requires a read and a write, make
 * a crude estimate of the read time and subtract it from the desired delta.
 */
static int fec_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct fec_enet_private *fep =
	    container_of(ptp, struct fec_enet_private, ptp_caps);
	unsigned long flags;
	u64 now, then;
	s64 real_delta;

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);

	now = timecounter_read(&fep->tc);
	then = timecounter_read(&fep->tc);

	real_delta = delta + (then - now);

	now = timecounter_read(&fep->tc);
	now += real_delta;

	fec_timecounter_set(fep, now);

	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);

	return 0;
}

/**
 * fec_ptp_settime
 * @ptp: the ptp clock structure
 * @ts: the timespec containing the new time for the cycle counter
 *
 * Update the timecounter to use a new base value instead of the kernel
 * wall timer value, and update the HW counter as well.
 */
static int fec_ptp_settime(struct ptp_clock_info *ptp,
			   const struct timespec64 *ts)
{
	struct fec_enet_private *fep =
	    container_of(ptp, struct fec_enet_private, ptp_caps);

	u64 ns;
	unsigned long flags;

	ns = ts->tv_sec * 1000000000ULL;
	ns += ts->tv_nsec;

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);
	fec_timecounter_set(fep, ns);
	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);
	return 0;
}

/**
 * fec_ptp_read
 * @data: fec private context ptr
 * @cnt: data pointer for counter value
 *
 * Returns status
 */
int fec_ptp_read_cnt(void *data, u32 *cnt)
{
	struct fec_enet_private *fep = data;
	unsigned long flags;

	/* Note : it might be possible to avoid taking the lock */
	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);

	*cnt = (u32) fec_ptp_read(&fep->cc);

	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);

	return 0;
}
EXPORT_SYMBOL(fec_ptp_read_cnt);

/**
 * fec_ptp_tc_start
 * @data: fec private context ptr
 * @id: TC register ID
 * @ts_0: First timestamp
 * @ts_1: Second timestamp
 * @tcsr_val: TCSR register value
 *
 * Returns 0 on success, -1 if PTP counter is not
 * enabled.
 */
int fec_ptp_tc_start(void *data, u8 id, u32 ts_0, u32 ts_1, u32 tcsr_val)
{
	struct fec_enet_private *fep = data;
	unsigned long flags;
	u32 ctrl_val;
	int rc = 0;

	if (id > MAX_TIMER_CHANNEL) {
		rc = -1;
		goto exit;
	}

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);

	/* Simple resource sharing handling with pps */
	if (fep->pps_enable && (fep->pps_channel == id)) {
		rc = -1;
		goto exit_unlock;
	}

	ctrl_val = readl_relaxed(fep->hwp + FEC_ATIME_CTRL);
	if (!(ctrl_val & FEC_T_CTRL_ENABLE)) {
		rc = -1;
		goto exit_unlock;
	}

	writel_relaxed(ts_0, fep->hwp + FEC_TCCR(id));
	writel_relaxed(tcsr_val, fep->hwp + FEC_TCSR(id));
	writel_relaxed(ts_1, fep->hwp + FEC_TCCR(id));

	fep->rec_enable = 1;
	fep->rec_channel = id;

exit_unlock:
	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);
exit:
	return rc;
}
EXPORT_SYMBOL(fec_ptp_tc_start);

/**
 * fec_ptp_tc_stop
 * @data: fec private context ptr
 * @id: TC register ID
 *
 * Returns none
 */
void fec_ptp_tc_stop(void *data, u8 id)
{
	struct fec_enet_private *fep = data;
	unsigned long flags;

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);

	writel_relaxed(0, fep->hwp + FEC_TCCR(id));
	writel_relaxed(FEC_T_TF_MASK, fep->hwp + FEC_TCSR(id));

	fep->rec_enable = 0;

	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);
}
EXPORT_SYMBOL(fec_ptp_tc_stop);

/**
 * fec_ptp_tc_reload
 * @data: fec private context ptr
 * @id: TC register ID
 * @ts: New timestamp to load
 *
 * Returns 0 if success, -1 if compare has not occured
 * or if PTP counter is not enabled.
 */
int fec_ptp_tc_reload(void *data, u8 id, u32 ts)
{
	struct fec_enet_private *fep = data;
	unsigned long flags;
	u32 tcsr_val;
	u32 ctrl_val;
	int rc = 0;

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);

	ctrl_val = readl_relaxed(fep->hwp + FEC_ATIME_CTRL);
	if (!(ctrl_val & FEC_T_CTRL_ENABLE)) {
		rc = -1;
		goto exit;
	}

	tcsr_val = readl_relaxed(fep->hwp + FEC_TCSR(id));
	if (tcsr_val & FEC_T_TF_MASK) {
		writel_relaxed(ts, fep->hwp + FEC_TCCR(id));
		writel_relaxed(tcsr_val, fep->hwp + FEC_TCSR(id));
	}
	else
		rc = -1;

exit:
	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);
	return rc;
}
EXPORT_SYMBOL(fec_ptp_tc_reload);
#else /* CONFIG_AVB_SUPPORT */

/**
 * fec_ptp_adjfreq - adjust ptp cycle frequency
 * @ptp: the ptp clock structure
 * @ppb: parts per billion adjustment from base
 *
 * Adjust the frequency of the ptp cycle counter by the
 * indicated ppb from the base frequency.
 *
 * Because ENET hardware frequency adjust is complex,
 * using software method to do that.
 */
static int fec_ptp_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
	unsigned long flags;
	int neg_adj = 0;
	u32 i, tmp;
	u32 corr_inc, corr_period;
	u32 corr_ns;
	u64 lhs, rhs;

	struct fec_enet_private *fep =
	    container_of(ptp, struct fec_enet_private, ptp_caps);

	if (ppb == 0)
		return 0;

	if (ppb < 0) {
		ppb = -ppb;
		neg_adj = 1;
	}

	/* In theory, corr_inc/corr_period = ppb/NSEC_PER_SEC;
	 * Try to find the corr_inc  between 1 to fep->ptp_inc to
	 * meet adjustment requirement.
	 */
	lhs = NSEC_PER_SEC;
	rhs = (u64)ppb * (u64)fep->ptp_inc;
	for (i = 1; i <= fep->ptp_inc; i++) {
		if (lhs >= rhs) {
			corr_inc = i;
			corr_period = div_u64(lhs, rhs);
			break;
		}
		lhs += NSEC_PER_SEC;
	}
	/* Not found? Set it to high value - double speed
	 * correct in every clock step.
	 */
	if (i > fep->ptp_inc) {
		corr_inc = fep->ptp_inc;
		corr_period = 1;
	}

	if (neg_adj)
		corr_ns = fep->ptp_inc - corr_inc;
	else
		corr_ns = fep->ptp_inc + corr_inc;

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);

	tmp = readl(fep->hwp + FEC_ATIME_INC) & FEC_T_INC_MASK;
	tmp |= corr_ns << FEC_T_INC_CORR_OFFSET;
	writel(tmp, fep->hwp + FEC_ATIME_INC);
	corr_period = corr_period > 1 ? corr_period - 1 : corr_period;
	writel(corr_period, fep->hwp + FEC_ATIME_CORR);
	/* dummy read to update the timer. */
	timecounter_read(&fep->tc);

	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);

	return 0;
}

/**
 * fec_ptp_adjtime
 * @ptp: the ptp clock structure
 * @delta: offset to adjust the cycle counter by
 *
 * adjust the timer by resetting the timecounter structure.
 */
static int fec_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct fec_enet_private *fep =
	    container_of(ptp, struct fec_enet_private, ptp_caps);
	unsigned long flags;

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);
	timecounter_adjtime(&fep->tc, delta);
	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);

	return 0;
}

/**
 * fec_ptp_settime
 * @ptp: the ptp clock structure
 * @ts: the timespec containing the new time for the cycle counter
 *
 * reset the timecounter to use a new base value instead of the kernel
 * wall timer value.
 */
static int fec_ptp_settime(struct ptp_clock_info *ptp,
			   const struct timespec64 *ts)
{
	struct fec_enet_private *fep =
	    container_of(ptp, struct fec_enet_private, ptp_caps);

	u64 ns;
	unsigned long flags;
	u32 counter;

	mutex_lock(&fep->ptp_clk_mutex);
	/* Check the ptp clock */
	if (!fep->ptp_clk_on) {
		mutex_unlock(&fep->ptp_clk_mutex);
		return -EINVAL;
	}

	ns = timespec64_to_ns(ts);
	/* Get the timer value based on timestamp.
	 * Update the counter with the masked value.
	 */
	counter = ns & fep->cc.mask;

	raw_spin_lock_irqsave(&fep->tmreg_lock, flags);
	writel(counter, fep->hwp + FEC_ATIME);
	timecounter_init(&fep->tc, &fep->cc, ns);
	raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);
	mutex_unlock(&fep->ptp_clk_mutex);

	return 0;
}
#endif /* CONFIG_AVB_SUPPORT */

/**
 * fec_ptp_gettime
 * @ptp: the ptp clock structure
 * @ts: timespec structure to hold the current time value
 *
 * read the timecounter and return the correct value on ns,
 * after converting it into a struct timespec.
 */
static int fec_ptp_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct fec_enet_private *adapter =
	    container_of(ptp, struct fec_enet_private, ptp_caps);
	u64 ns;
	u32 remainder;
	unsigned long flags;

	mutex_lock(&adapter->ptp_clk_mutex);
	/* Check the ptp clock */
	if (!adapter->ptp_clk_on) {
		mutex_unlock(&adapter->ptp_clk_mutex);
		return -EINVAL;
	}
	raw_spin_lock_irqsave(&adapter->tmreg_lock, flags);
	ns = timecounter_read(&adapter->tc);
	raw_spin_unlock_irqrestore(&adapter->tmreg_lock, flags);
	mutex_unlock(&adapter->ptp_clk_mutex);

	ts->tv_sec = div_u64_rem(ns, 1000000000ULL, &remainder);
	ts->tv_nsec = remainder;

	return 0;
}

/**
 * fec_ptp_enable
 * @ptp: the ptp clock structure
 * @rq: the requested feature to change
 * @on: whether to enable or disable the feature
 *
 */
static int fec_ptp_enable(struct ptp_clock_info *ptp,
			  struct ptp_clock_request *rq, int on)
{
	struct fec_enet_private *fep =
	    container_of(ptp, struct fec_enet_private, ptp_caps);
	int ret = 0;

	if (rq->type == PTP_CLK_REQ_PPS) {
		ret = fec_ptp_enable_pps(fep, on);

		return ret;
	}
	return -EOPNOTSUPP;
}

/**
 * fec_ptp_disable_hwts - disable hardware time stamping
 * @ndev: pointer to net_device
 */
void fec_ptp_disable_hwts(struct net_device *ndev)
{
	struct fec_enet_private *fep = netdev_priv(ndev);

	fep->hwts_tx_en = 0;
	fep->hwts_rx_en = 0;
}

int fec_ptp_set(struct net_device *ndev, struct ifreq *ifr)
{
	struct fec_enet_private *fep = netdev_priv(ndev);

	struct hwtstamp_config config;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	/* reserved for future extensions */
	if (config.flags)
		return -EINVAL;

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		fep->hwts_tx_en = 0;
		break;
	case HWTSTAMP_TX_ON:
		fep->hwts_tx_en = 1;
		break;
	default:
		return -ERANGE;
	}

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		fep->hwts_rx_en = 0;
		break;

	default:
		fep->hwts_rx_en = 1;
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	}

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
	    -EFAULT : 0;
}

int fec_ptp_get(struct net_device *ndev, struct ifreq *ifr)
{
	struct fec_enet_private *fep = netdev_priv(ndev);
	struct hwtstamp_config config;

	config.flags = 0;
	config.tx_type = fep->hwts_tx_en ? HWTSTAMP_TX_ON : HWTSTAMP_TX_OFF;
	config.rx_filter = (fep->hwts_rx_en ?
			    HWTSTAMP_FILTER_ALL : HWTSTAMP_FILTER_NONE);

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
		-EFAULT : 0;
}

/*
 * fec_time_keep - call timecounter_read every second to avoid timer overrun
 *                 because ENET just support 32bit counter, will timeout in 4s
 */
static void fec_time_keep(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct fec_enet_private *fep = container_of(dwork, struct fec_enet_private, time_keep);
	unsigned long flags;

	mutex_lock(&fep->ptp_clk_mutex);
	if (fep->ptp_clk_on) {
		raw_spin_lock_irqsave(&fep->tmreg_lock, flags);
		timecounter_read(&fep->tc);
		raw_spin_unlock_irqrestore(&fep->tmreg_lock, flags);
	}
	mutex_unlock(&fep->ptp_clk_mutex);

	schedule_delayed_work(&fep->time_keep, HZ);
}

/* This function checks the pps event and reloads the timer compare counter. */
static irqreturn_t fec_pps_interrupt(int irq, void *dev_id)
{
	struct net_device *ndev = dev_id;
	struct fec_enet_private *fep = netdev_priv(ndev);
	u32 val;
	u8 channel = fep->pps_channel;
	struct ptp_clock_event event;

	if (fep->pps_enable)
		goto exit;

	val = readl(fep->hwp + FEC_TCSR(channel));
	if (val & FEC_T_TF_MASK) {
		/* Write the next next compare(not the next according the spec)
		 * value to the register
		 */
		writel(fep->next_counter, fep->hwp + FEC_TCCR(channel));
		do {
			writel(val, fep->hwp + FEC_TCSR(channel));
		} while (readl(fep->hwp + FEC_TCSR(channel)) & FEC_T_TF_MASK);

		/* Update the counter; */
		fep->next_counter = (fep->next_counter + fep->reload_period) &
				fep->cc.mask;

		event.type = PTP_CLOCK_PPS;
		ptp_clock_event(fep->ptp_clock, &event);
		return IRQ_HANDLED;
	}

exit:
	return IRQ_NONE;
}

/**
 * fec_ptp_init
 * @pdev: The FEC network adapter
 * @irq_idx: the interrupt index
 *
 * This function performs the required steps for enabling ptp
 * support. If ptp support has already been loaded it simply calls the
 * cyclecounter init routine and exits.
 */

void fec_ptp_init(struct platform_device *pdev, int irq_idx)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct fec_enet_private *fep = netdev_priv(ndev);
	int irq;
	int ret;

	fep->ptp_caps.owner = THIS_MODULE;
	strlcpy(fep->ptp_caps.name, "fec ptp", sizeof(fep->ptp_caps.name));

	fep->cycle_speed = clk_get_rate(fep->clk_ptp);
	if (!fep->cycle_speed) {
		fep->cycle_speed = NSEC_PER_SEC;
		dev_err(&fep->pdev->dev, "clk_ptp clock rate is zero\n");
	}
	fep->ptp_inc = NSEC_PER_SEC / fep->cycle_speed;

#ifdef CONFIG_AVB_SUPPORT
	fep->ptp_caps.max_adj = fep->cycle_speed / 2;
#else
	fep->ptp_caps.max_adj = 250000000;
#endif
	fep->ptp_caps.n_alarm = 0;
	fep->ptp_caps.n_ext_ts = 0;
	fep->ptp_caps.n_per_out = 0;
	fep->ptp_caps.n_pins = 0;
	fep->ptp_caps.pps = 1;
	fep->ptp_caps.adjfreq = fec_ptp_adjfreq;
	fep->ptp_caps.adjtime = fec_ptp_adjtime;
	fep->ptp_caps.gettime64 = fec_ptp_gettime;
	fep->ptp_caps.settime64 = fec_ptp_settime;
	fep->ptp_caps.enable = fec_ptp_enable;

	raw_spin_lock_init(&fep->tmreg_lock);

	fec_ptp_start_cyclecounter(ndev);

	INIT_DELAYED_WORK(&fep->time_keep, fec_time_keep);

	irq = platform_get_irq_byname_optional(pdev, "pps");
	if (irq < 0)
		irq = platform_get_irq_optional(pdev, irq_idx);
	/* Failure to get an irq is not fatal,
	 * only the PTP_CLOCK_PPS clock events should stop
	 */
	if (irq >= 0) {
		ret = devm_request_irq(&pdev->dev, irq, fec_pps_interrupt,
				       0, pdev->name, ndev);
		if (ret < 0)
			dev_warn(&pdev->dev, "request for pps irq failed(%d)\n",
				 ret);
	}

	fep->ptp_clock = ptp_clock_register(&fep->ptp_caps, &pdev->dev);
	if (IS_ERR(fep->ptp_clock)) {
		fep->ptp_clock = NULL;
		dev_err(&pdev->dev, "ptp_clock_register failed\n");
	}

	schedule_delayed_work(&fep->time_keep, HZ);
}

void fec_ptp_stop(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct fec_enet_private *fep = netdev_priv(ndev);

	cancel_delayed_work_sync(&fep->time_keep);
	if (fep->ptp_clock)
		ptp_clock_unregister(fep->ptp_clock);
}
