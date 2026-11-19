// SPDX-License-Identifier: GPL-2.0
/*
 * Ring buffer bookkeeping for the WMT driver.
 *
 * MediaTek's source drop lacks this file. It was written again from the
 * functions in the wmt_drv.ko built from the complete sources; each function
 * does the same arithmetic, in the same order, as its disassembly.
 */

#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/string.h>

#include "ring.h"

void ring_init(void *base, unsigned int max_size, unsigned int read,
	       unsigned int write, struct ring *ring)
{
	WARN_ON(!base);
	RING_VALIDATE_SIZE(max_size);
	WARN_ON(read > write);

	ring->base = base;
	ring->write = write;
	ring->read = read;
	ring->max_size = max_size;
}

void ring_dump(const char *title, struct ring *ring)
{
	pr_info("[%s] ring:{write=%d, read=%d, max_size=%d}\n",
		title, ring->write, ring->read, ring->max_size);
}

void ring_dump_segment(const char *title, struct ring_segment *seg)
{
	pr_info("[%s] seg:{ring_pt=0x%p, data_pos=%d, sz=%d, remain=%d}\n",
		title, seg->ring_pt, seg->data_pos, seg->sz, seg->remain);
}

/*
 * Start reading up to sz. What can really be read is returned and left in
 * seg->remain for the RING_READ_FOR_EACH* loops to walk.
 */
unsigned int ring_read_prepare(unsigned int sz, struct ring_segment *seg,
			       struct ring *ring)
{
	unsigned int wt = ring->write;
	unsigned int rd = ring->read;

	memset(seg, 0, sizeof(struct ring_segment));
	if (sz > wt - rd)
		sz = wt - rd;
	seg->remain = sz;
	return seg->remain;
}

/* Start writing up to sz, limited to the room left in the ring */
unsigned int ring_write_prepare(unsigned int sz, struct ring_segment *seg,
				struct ring *ring)
{
	unsigned int wt = ring->write;
	unsigned int rd = ring->read;

	memset(seg, 0, sizeof(struct ring_segment));
	if (sz > ring->max_size - (wt - rd))
		sz = ring->max_size - (wt - rd);
	seg->remain = sz;
	return seg->remain;
}

/* Start writing sz, dropping the oldest data when there is no room for it */
unsigned int ring_overwrite_prepare(unsigned int sz, struct ring_segment *seg,
				    struct ring *ring)
{
	unsigned int wt = ring->write;
	unsigned int rd = ring->read;

	memset(seg, 0, sizeof(struct ring_segment));
	if (sz > ring->max_size - (wt - rd))
		ring->read += sz - (ring->max_size - (wt - rd));
	seg->remain = sz;
	return seg->remain;
}

/*
 * The next segment: at most sz from position "from", ending at the latest
 * where the buffer wraps.
 */
static void __ring_segment_prepare(unsigned int from, unsigned int sz,
				   struct ring_segment *seg, struct ring *ring)
{
	unsigned int ring_pos = from & (ring->max_size - 1);

	seg->ring_pt = ring->base + ring_pos;
	seg->data_pos = (seg->sz ? seg->data_pos + seg->sz : 0);
	if (ring_pos + sz <= ring->max_size)
		seg->sz = sz;
	else
		seg->sz = ring->max_size - ring_pos;
	seg->remain -= seg->sz;
}

void _ring_segment_prepare(unsigned int from, struct ring_segment *seg,
			   struct ring *ring)
{
	__ring_segment_prepare(from, seg->remain, seg, ring);
}

void _ring_segment_prepare_item(unsigned int from, struct ring_segment *seg,
				struct ring *ring)
{
	unsigned int size = (seg->remain ? 1 : 0);

	__ring_segment_prepare(from, size, seg, ring);
}

void _ring_read_commit(struct ring_segment *seg, struct ring *ring)
{
	ring->read += seg->sz;
}

void _ring_write_commit(struct ring_segment *seg, struct ring *ring)
{
	ring->write += seg->sz;
}
