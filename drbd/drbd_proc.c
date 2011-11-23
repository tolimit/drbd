/*
   drbd_proc.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/autoconf.h>
#include <linux/module.h>

#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/drbd.h>
#include "drbd_int.h"

STATIC int drbd_proc_open(struct inode *inode, struct file *file);
STATIC int drbd_proc_release(struct inode *inode, struct file *file);


struct proc_dir_entry *drbd_proc;
const struct file_operations drbd_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= drbd_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= drbd_proc_release,
};

void seq_printf_with_thousands_grouping(struct seq_file *seq, long v)
{
	/* v is in kB/sec. We don't expect TiByte/sec yet. */
	if (unlikely(v >= 1000000)) {
		/* cool: > GiByte/s */
		seq_printf(seq, "%ld,", v / 1000000);
		v %= 1000000;
		seq_printf(seq, "%03ld,%03ld", v/1000, v % 1000);
	} else if (likely(v >= 1000))
		seq_printf(seq, "%ld,%03ld", v/1000, v % 1000);
	else
		seq_printf(seq, "%ld", v);
}

/* you must have an "get_ldev" reference */
static void drbd_get_syncer_progress(struct drbd_device *device,
		unsigned long *bits_left, unsigned int *per_mil_done)
{
	struct drbd_peer_device *peer_device = first_peer_device(device);

	/* this is to break it at compile time when we change that, in case we
	 * want to support more than (1<<32) bits on a 32bit arch. */
	typecheck(unsigned long, device->rs_total);

	/* note: both rs_total and rs_left are in bits, i.e. in
	 * units of BM_BLOCK_SIZE.
	 * for the percentage, we don't care. */

	if (peer_device->repl_state == L_VERIFY_S || peer_device->repl_state == L_VERIFY_T)
		*bits_left = device->ov_left;
	else
		*bits_left = drbd_bm_total_weight(device) - device->rs_failed;
	/* >> 10 to prevent overflow,
	 * +1 to prevent division by zero */
	if (*bits_left > device->rs_total) {
		/* doh. maybe a logic bug somewhere.
		 * may also be just a race condition
		 * between this and a disconnect during sync.
		 * for now, just prevent in-kernel buffer overflow.
		 */
		smp_rmb();
		drbd_warn(device, "cs:%s rs_left=%lu > rs_total=%lu (rs_failed %lu)\n",
				drbd_conn_str(peer_device->repl_state),
				*bits_left, device->rs_total, device->rs_failed);
		*per_mil_done = 0;
	} else {
		/* Make sure the division happens in long context.
		 * We allow up to one petabyte storage right now,
		 * at a granularity of 4k per bit that is 2**38 bits.
		 * After shift right and multiplication by 1000,
		 * this should still fit easily into a 32bit long,
		 * so we don't need a 64bit division on 32bit arch.
		 * Note: currently we don't support such large bitmaps on 32bit
		 * arch anyways, but no harm done to be prepared for it here.
		 */
		unsigned int shift = device->rs_total > UINT_MAX ? 16 : 10;
		unsigned long left = *bits_left >> shift;
		unsigned long total = 1UL + (device->rs_total >> shift);
		unsigned long tmp = 1000UL - left * 1000UL/total;
		*per_mil_done = tmp;
	}
}


/*lge
 * progress bars shamelessly adapted from driver/md/md.c
 * output looks like
 *	[=====>..............] 33.5% (23456/123456)
 *	finish: 2:20:20 speed: 6,345 (6,456) K/sec
 */
STATIC void drbd_syncer_progress(struct drbd_device *device, struct seq_file *seq)
{
	struct drbd_peer_device *peer_device = first_peer_device(device);
	unsigned long db, dt, dbdt, rt, rs_left;
	unsigned int res;
	int i, x, y;
	int stalled = 0;

	drbd_get_syncer_progress(device, &rs_left, &res);

	x = res/50;
	y = 20-x;
	seq_printf(seq, "\t[");
	for (i = 1; i < x; i++)
		seq_printf(seq, "=");
	seq_printf(seq, ">");
	for (i = 0; i < y; i++)
		seq_printf(seq, ".");
	seq_printf(seq, "] ");

	if (peer_device->repl_state == L_VERIFY_S || peer_device->repl_state == L_VERIFY_T)
		seq_printf(seq, "verified:");
	else
		seq_printf(seq, "sync'ed:");
	seq_printf(seq, "%3u.%u%% ", res / 10, res % 10);

	/* if more than a few GB, display in MB */
	if (device->rs_total > (4UL << (30 - BM_BLOCK_SHIFT)))
		seq_printf(seq, "(%lu/%lu)M",
			    (unsigned long) Bit2KB(rs_left >> 10),
			    (unsigned long) Bit2KB(device->rs_total >> 10));
	else
		seq_printf(seq, "(%lu/%lu)K",
			    (unsigned long) Bit2KB(rs_left),
			    (unsigned long) Bit2KB(device->rs_total));

	seq_printf(seq, "\n\t");

	/* see drivers/md/md.c
	 * We do not want to overflow, so the order of operands and
	 * the * 100 / 100 trick are important. We do a +1 to be
	 * safe against division by zero. We only estimate anyway.
	 *
	 * dt: time from mark until now
	 * db: blocks written from mark until now
	 * rt: remaining time
	 */
	/* Rolling marks. last_mark+1 may just now be modified.  last_mark+2 is
	 * at least (DRBD_SYNC_MARKS-2)*DRBD_SYNC_MARK_STEP old, and has at
	 * least DRBD_SYNC_MARK_STEP time before it will be modified. */
	/* ------------------------ ~18s average ------------------------ */
	i = (device->rs_last_mark + 2) % DRBD_SYNC_MARKS;
	dt = (jiffies - device->rs_mark_time[i]) / HZ;
	if (dt > (DRBD_SYNC_MARK_STEP * DRBD_SYNC_MARKS))
		stalled = 1;

	if (!dt)
		dt++;
	db = device->rs_mark_left[i] - rs_left;
	rt = (dt * (rs_left / (db/100+1)))/100; /* seconds */

	seq_printf(seq, "finish: %lu:%02lu:%02lu",
		rt / 3600, (rt % 3600) / 60, rt % 60);

	dbdt = Bit2KB(db/dt);
	seq_printf(seq, " speed: ");
	seq_printf_with_thousands_grouping(seq, dbdt);
	seq_printf(seq, " (");
	/* ------------------------- ~3s average ------------------------ */
	if (proc_details >= 1) {
		/* this is what drbd_rs_should_slow_down() uses */
		i = (device->rs_last_mark + DRBD_SYNC_MARKS-1) % DRBD_SYNC_MARKS;
		dt = (jiffies - device->rs_mark_time[i]) / HZ;
		if (!dt)
			dt++;
		db = device->rs_mark_left[i] - rs_left;
		dbdt = Bit2KB(db/dt);
		seq_printf_with_thousands_grouping(seq, dbdt);
		seq_printf(seq, " -- ");
	}

	/* --------------------- long term average ---------------------- */
	/* mean speed since syncer started
	 * we do account for PausedSync periods */
	dt = (jiffies - device->rs_start - device->rs_paused) / HZ;
	if (dt == 0)
		dt = 1;
	db = device->rs_total - rs_left;
	dbdt = Bit2KB(db/dt);
	seq_printf_with_thousands_grouping(seq, dbdt);
	seq_printf(seq, ")");

	if (peer_device->repl_state == L_SYNC_TARGET ||
	    peer_device->repl_state == L_VERIFY_S) {
		seq_printf(seq, " want: ");
		seq_printf_with_thousands_grouping(seq, device->c_sync_rate);
	}
	seq_printf(seq, " K/sec%s\n", stalled ? " (stalled)" : "");

	if (proc_details >= 1) {
		/* 64 bit:
		 * we convert to sectors in the display below. */
		unsigned long bm_bits = drbd_bm_bits(device);
		unsigned long bit_pos;
		if (peer_device->repl_state == L_VERIFY_S ||
		    peer_device->repl_state == L_VERIFY_T)
			bit_pos = bm_bits - device->ov_left;
		else
			bit_pos = device->bm_resync_fo;
		/* Total sectors may be slightly off for oddly
		 * sized devices. So what. */
		seq_printf(seq,
			"\t%3d%% sector pos: %llu/%llu\n",
			(int)(bit_pos / (bm_bits/100+1)),
			(unsigned long long)bit_pos * BM_SECT_PER_BIT,
			(unsigned long long)bm_bits * BM_SECT_PER_BIT);
	}
}

STATIC void resync_dump_detail(struct seq_file *seq, struct lc_element *e)
{
	struct bm_extent *bme = lc_entry(e, struct bm_extent, lce);

	seq_printf(seq, "%5d %s %s\n", bme->rs_left,
		   bme->flags & BME_NO_WRITES ? "NO_WRITES" : "---------",
		   bme->flags & BME_LOCKED ? "LOCKED" : "------"
		   );
}

STATIC int drbd_seq_show(struct seq_file *seq, void *v)
{
	int i, prev_i = -1;
	const char *sn;
	struct drbd_device *device;
	struct net_conf *nc;
	char wp;

	static char write_ordering_chars[] = {
		[WO_none] = 'n',
		[WO_drain_io] = 'd',
		[WO_bdev_flush] = 'f',
		[WO_bio_barrier] = 'b',
	};

	seq_printf(seq, "version: " REL_VERSION " (api:%d/proto:%d-%d)\n%s\n",
		   API_VERSION, PRO_VERSION_MIN, PRO_VERSION_MAX, drbd_buildtag());

	/*
	  cs .. connection state
	  ro .. node role (local/remote)
	  ds .. disk state (local/remote)
	     protocol
	     various flags
	  ns .. network send
	  nr .. network receive
	  dw .. disk write
	  dr .. disk read
	  al .. activity log write count
	  bm .. bitmap update write count
	  pe .. pending (waiting for ack or data reply)
	  ua .. unack'd (still need to send ack or data reply)
	  ap .. application requests accepted, but not yet completed
	  ep .. number of epochs currently "on the fly", P_BARRIER_ACK pending
	  wo .. write ordering mode currently in use
	 oos .. known out-of-sync kB
	*/

	rcu_read_lock();
	idr_for_each_entry(&drbd_devices, device, i) {
		struct drbd_peer_device *peer_device = first_peer_device(device);

		if (prev_i != i - 1)
			seq_printf(seq, "\n");
		prev_i = i;

		sn = drbd_conn_str(combined_conn_state(peer_device));

		if (peer_device->repl_state == L_STANDALONE &&
		    device->disk_state == D_DISKLESS &&
		    device->resource->role == R_SECONDARY) {
			seq_printf(seq, "%2d: cs:Unconfigured\n", i);
		} else {
			struct drbd_peer_device *peer_device;
			unsigned int send_cnt = 0;
			unsigned int recv_cnt = 0;

			for_each_peer_device(peer_device, device) {
				send_cnt += peer_device->send_cnt;
				recv_cnt += peer_device->recv_cnt;
			}

			nc = rcu_dereference(peer_device->connection->net_conf);
			wp = nc ? nc->wire_protocol - DRBD_PROT_A + 'A' : ' ';
			seq_printf(seq,
			   "%2d: cs:%s ro:%s/%s ds:%s/%s %c %c%c%c%c%c%c\n"
			   "    ns:%u nr:%u dw:%u dr:%u al:%u bm:%u "
			   "lo:%d pe:%d ua:%d ap:%d ep:%d wo:%c",
			   i, sn,
			   drbd_role_str(device->resource->role),
			   drbd_role_str(first_connection(device->resource)->peer_role),
			   drbd_disk_str(device->disk_state),
			   drbd_disk_str(peer_device->disk_state),
			   wp,
			   drbd_suspended(device) ? 's' : 'r',
			   peer_device->resync_susp_dependency ? 'a' : '-',
			   peer_device->resync_susp_peer ? 'p' : '-',
			   peer_device->resync_susp_user ? 'u' : '-',
			   device->congestion_reason ?: '-',
			   test_bit(AL_SUSPENDED, &device->flags) ? 's' : '-',
			   send_cnt/2,
			   recv_cnt/2,
			   device->writ_cnt/2,
			   device->read_cnt/2,
			   device->al_writ_cnt,
			   device->bm_writ_cnt,
			   atomic_read(&device->local_cnt),
			   atomic_read(&device->ap_pending_cnt) +
			   atomic_read(&peer_device->rs_pending_cnt),
			   atomic_read(&device->unacked_cnt),
			   atomic_read(&device->ap_bio_cnt),
			   first_peer_device(device)->connection->epochs,
			   write_ordering_chars[device->resource->write_ordering]
			);
			seq_printf(seq, " oos:%llu\n",
				   Bit2KB((unsigned long long)
					   drbd_bm_total_weight(device)));
		}
		if (peer_device->repl_state == L_SYNC_SOURCE ||
		    peer_device->repl_state == L_SYNC_TARGET ||
		    peer_device->repl_state == L_VERIFY_S ||
		    peer_device->repl_state == L_VERIFY_T)
			drbd_syncer_progress(device, seq);

		if (proc_details >= 1 && get_ldev_if_state(device, D_FAILED)) {
			lc_seq_printf_stats(seq, device->resync);
			lc_seq_printf_stats(seq, device->act_log);
			put_ldev(device);
		}

		if (proc_details >= 2) {
			if (device->resync) {
				lc_seq_dump_details(seq, device->resync, "rs_left",
					resync_dump_detail);
			}
		}
	}
	rcu_read_unlock();

	return 0;
}

STATIC int drbd_proc_open(struct inode *inode, struct file *file)
{
	if (try_module_get(THIS_MODULE))
		return single_open(file, drbd_seq_show, PDE(inode)->data);
	return -ENODEV;
}

STATIC int drbd_proc_release(struct inode *inode, struct file *file)
{
	module_put(THIS_MODULE);
	return single_release(inode, file);
}

/* PROC FS stuff end */
