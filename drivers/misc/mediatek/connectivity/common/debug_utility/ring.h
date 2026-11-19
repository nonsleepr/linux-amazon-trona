/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Ring buffer bookkeeping for the WMT driver.
 *
 * MediaTek's source drop lacks this file. It was written again from the
 * functions in the wmt_drv.ko built from the complete sources, and from the
 * code the macros below expanded to in their users there.
 */

#ifndef _RING_H_
#define _RING_H_

struct ring {
	/* where the buffer starts */
	void *base;
	/* next position to write; free running, only masked on use */
	unsigned int write;
	/* next position to read; free running, the ring is empty at == write */
	unsigned int read;
	/* a power of 2 */
	unsigned int max_size;
};

struct ring_segment {
	/* where in the ring to read or write */
	void *ring_pt;
	/* how much to read or write there */
	unsigned int sz;
	/* where that is in the caller's linear buffer */
	unsigned int data_pos;
	/* what is left to read or write after this segment */
	unsigned int remain;
};

void ring_init(void *base, unsigned int max_size, unsigned int read,
	       unsigned int write, struct ring *ring);
unsigned int ring_read_prepare(unsigned int sz, struct ring_segment *seg,
			       struct ring *ring);
#define ring_read_all_prepare(seg, ring) \
	ring_read_prepare((ring)->max_size, seg, ring)
unsigned int ring_write_prepare(unsigned int sz, struct ring_segment *seg,
				struct ring *ring);
unsigned int ring_overwrite_prepare(unsigned int sz, struct ring_segment *seg,
				    struct ring *ring);

/* making sure max_size is power of 2 */
#define RING_VALIDATE_SIZE(max_size) \
	WARN_ON(!(max_size) || ((max_size) & ((max_size) - 1)))

#define RING_EMPTY(ring) ((ring)->read == (ring)->write)
/* equation works even when write overflow */
#define RING_SIZE(ring) ((ring)->write - (ring)->read)
#define RING_FULL(ring) (RING_SIZE(ring) == (ring)->max_size)
#define RING_WRITE_REMAIN_SIZE(ring) ((ring)->max_size - RING_SIZE(ring))

#define RING_READ_FOR_EACH(_sz, _seg, _ring) \
	for (ring_read_prepare(_sz, &(_seg), _ring), \
		_ring_segment_prepare((_ring)->read, &(_seg), (_ring)); \
		(_seg).sz > 0; \
		_ring_read_commit(&(_seg), (_ring)), \
		_ring_segment_prepare((_ring)->read, &(_seg), (_ring)))

#define RING_READ_ALL_FOR_EACH(seg, ring) \
	RING_READ_FOR_EACH((ring)->max_size, seg, ring)

#define RING_READ_FOR_EACH_ITEM(_sz, _seg, _ring) \
	for (ring_read_prepare(_sz, &(_seg), _ring), \
		_ring_segment_prepare_item((_ring)->read, &(_seg), (_ring)); \
		(_seg).sz > 0; \
		_ring_read_commit(&(_seg), (_ring)), \
		_ring_segment_prepare_item((_ring)->read, &(_seg), (_ring)))

#define RING_WRITE_FOR_EACH(_sz, _seg, _ring) \
	for (ring_write_prepare(_sz, &(_seg), _ring), \
		_ring_segment_prepare((_ring)->write, &(_seg), (_ring)); \
		(_seg).sz > 0; \
		_ring_write_commit(&(_seg), (_ring)), \
		_ring_segment_prepare((_ring)->write, &(_seg), (_ring)))

#define RING_OVERWRITE_FOR_EACH(_sz, _seg, _ring) \
	for (ring_overwrite_prepare(_sz, &(_seg), _ring), \
		_ring_segment_prepare((_ring)->write, &(_seg), (_ring)); \
		(_seg).sz > 0; \
		_ring_write_commit(&(_seg), (_ring)), \
		_ring_segment_prepare((_ring)->write, &(_seg), (_ring)))

void ring_dump(const char *title, struct ring *ring);
void ring_dump_segment(const char *title, struct ring_segment *seg);

/* for the macros above only */
void _ring_segment_prepare(unsigned int from, struct ring_segment *seg,
			   struct ring *ring);
void _ring_segment_prepare_item(unsigned int from, struct ring_segment *seg,
				struct ring *ring);
void _ring_read_commit(struct ring_segment *seg, struct ring *ring);
void _ring_write_commit(struct ring_segment *seg, struct ring *ring);

#endif /* _RING_H_ */
