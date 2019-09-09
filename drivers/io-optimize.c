/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#include "io-optimize.h"
#include "tapdisk-log.h"
#include "debug.h"

#define DEBUG
#if (!defined(TEST) && defined(DEBUG))
#define DBG(ctx, f, a...) tlog_write(TLOG_DBG, f, ##a)
#elif defined(TEST)
#define DBG(ctx, f, a...) printf(f, ##a)
#else
#define DBG(ctx, f, a...) ((void)0)
#endif

void
opio_free(struct opioctx *ctx)
{
	free(ctx->opios);
	ctx->opios = NULL;

	free(ctx->free_opios);
	ctx->free_opios = NULL;

	free(ctx->iocb_queue);
	ctx->iocb_queue = NULL;

	free(ctx->event_queue);
	ctx->event_queue = NULL;
}

int
opio_init(struct opioctx *ctx, int num_iocbs)
{
	int i;

	memset(ctx, 0, sizeof(struct opioctx));

	ctx->num_opios     = num_iocbs;
	ctx->free_opio_cnt = num_iocbs;
	ctx->opios         = calloc(1, sizeof(struct opio) * num_iocbs);
	ctx->free_opios    = calloc(1, sizeof(struct opio *) * num_iocbs);
	ctx->iocb_queue    = calloc(1, sizeof(struct iocb *) * num_iocbs);
	ctx->event_queue   = calloc(1, sizeof(struct io_event) * num_iocbs);

	if (!ctx->opios || !ctx->free_opios ||
	    !ctx->iocb_queue || !ctx->event_queue)
		goto fail;

	for (i = 0; i < num_iocbs; i++)
		ctx->free_opios[i] = &ctx->opios[i];

	return 0;

 fail:
	opio_free(ctx);
	return -ENOMEM;
}

static inline struct opio *
alloc_opio(struct opioctx *ctx)
{
	if (ctx->free_opio_cnt <= 0)
		return NULL;
	return ctx->free_opios[--ctx->free_opio_cnt];
}

static inline void
free_opio(struct opioctx *ctx, struct opio *op)
{
	memset(op, 0, sizeof(struct opio));
	ctx->free_opios[ctx->free_opio_cnt++] = op;
}

static inline void
restore_iocb(struct opio *op)
{
        ASSERT(op);
        ASSERT(op->opio_magic == OPIO_MAGIC);
	struct iocb *io = op->iocb;
	ASSERT(io);

	*io = op->orig_iocb;
}

static struct opio*
opio_cast(void *p)
{
    struct opio *opio = p;
    ASSERT(opio);
    ASSERT(opio->opio_magic == OPIO_MAGIC);
    return opio;
}

static int iocb_vectorized(io_iocb_cmd_t op)
{
	switch (op) {
		case IO_CMD_PREAD:
			return IO_CMD_PREADV;
		case IO_CMD_PWRITE:
			return IO_CMD_PWRITEV;
		default:
			return op;
	}
}

static inline int
iocb_optimized(struct opioctx *ctx, struct iocb *io)
{
	return iocb_vectorized(io->aio_lio_opcode) == io->aio_lio_opcode;
}

static inline size_t
iocb_nbytes(const struct iocb* io)
{
    if (iocb_vectorized(io->aio_lio_opcode) == io->aio_lio_opcode) {
	size_t sum = 0, iovcnt = io->u.c.nbytes, i;
	const struct iovec *iovec = io->u.c.buf;
	for(i=0;i<iovcnt;i++) {
                ASSERT(iovec[i].iov_len > 0);
		sum += iovec[i].iov_len;
	}
        return sum;
    }
    return io->u.c.nbytes;
}

static inline int
contiguous_sectors(struct iocb *l, struct iocb *r)
{
	return (l->u.c.offset + iocb_nbytes(l) == r->u.c.offset);
}

static inline int
contiguous_iocbs(struct iocb *l, struct iocb *r)
{
	return ((l->aio_fildes == r->aio_fildes) &&
		contiguous_sectors(l, r));
}

static inline void
init_opio_list(struct opio *op)
{
	op->list.head = op->list.tail = op;
}

static struct opio *
opio_iocb_init(struct opioctx *ctx, struct iocb *io)
{
	struct opio *op;

	op = alloc_opio(ctx);
	if (!op)
		return NULL;

        op->opio_magic = OPIO_MAGIC;
	op->orig_iocb = *io;
	op->iocb   = io;
	io->data   = op;

	init_opio_list(op);

	return op;
}

static inline struct opio *
opio_get(struct opioctx *ctx, struct iocb *io)
{
	if (iocb_optimized(ctx, io))
		return opio_cast(io->data);
	else
	        return opio_iocb_init(ctx, io);
}

static int
merge_tail(struct opioctx *ctx, struct iocb *head, struct iocb *io)
{
	struct opio *ophead, *opio;

	ophead = opio_get(ctx, head);
	if (!ophead)
		return -ENOMEM;

	opio = opio_get(ctx, io);
	if (!opio)
		return -ENOMEM;

	opio->head        = ophead;

	if (iocb_vectorized(head->aio_lio_opcode) != head->aio_lio_opcode) {
		struct iovec *iovec = &ophead->iov[0];
		iovec->iov_base = head->u.c.buf;
		iovec->iov_len = head->u.c.nbytes;
                ASSERT(iovec->iov_len > 0);
                void *data = head->data;

		switch(io->aio_lio_opcode) {
			case IO_CMD_PREAD:
				io_prep_preadv(head, head->aio_fildes, iovec, 1, head->u.c.offset);
				break;
			case IO_CMD_PWRITE:
				io_prep_pwritev(head, head->aio_fildes, iovec, 1, head->u.c.offset);
				break;
			default:
				ASSERT(0);
		}
                head->data = data;
		ASSERT(data == ophead);
	}
	ASSERT(!iocb_optimized(ctx, io));
	struct iovec *iovec = &ophead->iov[head->u.c.nbytes++];
	iovec->iov_base = io->u.c.buf;
	iovec->iov_len = io->u.c.nbytes;
        ASSERT(iovec->iov_len > 0);
	ophead->list.tail = ophead->list.tail->next = opio;
	return 0;
}

static int
merge(struct opioctx *ctx, struct iocb *head, struct iocb *io)
{
	if (iocb_vectorized(head->aio_lio_opcode) != iocb_vectorized(io->aio_lio_opcode))
		return -EINVAL;

	if (!contiguous_iocbs(head, io))
		return -EINVAL;

        /* otherwise we overflow and overwrite other values in the record */
        if(iocb_optimized(ctx, head) && head->u.c.nbytes == UIO_FASTIOV)
            return -EINVAL;

	return merge_tail(ctx, head, io);
}

#if (defined(TEST) || defined(DEBUG))
/******************************************************************************
debug print functions
******************************************************************************/
static inline void
__print_iocb(struct opioctx *ctx, struct iocb *io, char *prefix)
{
	if(iocb_vectorized(io->aio_lio_opcode) == io->aio_lio_opcode) {
		const struct iovec *iov = io->u.c.buf;
		unsigned iovcnt = io->u.c.nbytes;
		unsigned i;

		DBG(ctx, "%soff: %08llx, type: %s, data: %08lx, optimized: 1\n",
		    prefix, io->u.c.offset, io->aio_lio_opcode == IO_CMD_PREADV ? "preadv" : "pwritev",
		    (unsigned long)io->data);
		for(i=0;i<iovcnt;i++) {
			DBG(ctx, "%s\tnbytes: %04lx, buf: %p\n", prefix, iov[i].iov_len, iov[i].iov_base);
		}
	} else
		DBG(ctx, "%soff: %08llx, nbytes: %04lx, buf: %p, type: %s, data: %08lx,"
				" optimized: %d\n", prefix, io->u.c.offset, io->u.c.nbytes, 
				io->u.c.buf, (io->aio_lio_opcode == IO_CMD_PREAD ? "read" : "write"),
				(unsigned long)io->data, iocb_optimized(ctx, io));
}

#define print_iocb(ctx, io) __print_iocb(ctx, io, "")

static void
print_events(struct opioctx *ctx, struct io_event *events, int num_events)
{
	int i;
	struct iocb *io;

	for (i = 0; i < num_events; i++) {
		io = events[i].obj;
		print_iocb(ctx, io);
	}
}

/******************************************************************************
end debug print functions
******************************************************************************/

static void
print_optimized_iocbs(struct opioctx *ctx, struct opio *op, int *cnt)
{
	char pref[10];

	while (op) {
		snprintf(pref, 10, "  %d: ", (*cnt)++);
		__print_iocb(ctx, op->iocb, pref);
		op = op->next;
	}
}

static void
print_merged_iocbs(struct opioctx *ctx, struct iocb **iocbs, int num_iocbs)
{
	int i, cnt;
	char pref[10];
	struct iocb *io;
	struct opio *op;

	DBG(ctx, "merged iocbs:\n");
	for (i = 0, cnt = 0; i < num_iocbs; i++) {
		io = iocbs[i];
		snprintf(pref, 10, "%d: ", cnt++);
		__print_iocb(ctx, io, pref);

		if (iocb_optimized(ctx, io)) {
			op = opio_cast(io->data);
			print_optimized_iocbs(ctx, op->next, &cnt);
		}
	}
}
#else
#define print_optimized_iocbs(...)
#define print_merged_iocbs(...)
#endif

int
io_merge(struct opioctx *ctx, struct iocb **queue, int num)
{
	int i, on_queue;
	struct iocb *io, **q;

	if (!num)
		return 0;

	on_queue = 0;
	q = ctx->iocb_queue;
	memcpy(q, queue, num * sizeof(struct iocb *));

	for (i = 1; i < num; i++) {
		io = q[i];
		if (merge(ctx, queue[on_queue], io) != 0)
			queue[++on_queue] = io;
	}

	print_merged_iocbs(ctx, queue, on_queue + 1);

	return ++on_queue;
}

static int
expand_iocb(struct opioctx *ctx, struct iocb **queue, struct iocb *io)
{
	int idx;
	struct opio *op, *next;

	idx = 0;
	op  = opio_cast(io->data);
	while (op) {
		next = op->next;
		restore_iocb(op);
		queue[idx++] = op->iocb;
		free_opio(ctx, op);
		op   = next;
	}

	return idx;
}

int
io_expand_iocbs(struct opioctx *ctx, struct iocb **queue, int idx, int num)
{
	int i, on_queue;
	struct iocb *io, **q;

	if (!num)
		return 0;

	on_queue = 0;
	q = ctx->iocb_queue;
	memcpy(q, queue, num * sizeof(struct iocb *));

	for (i = idx; i < num; i++) {
		io = q[i];
		if (!iocb_optimized(ctx, io))
			queue[on_queue++] = io;
		else
			on_queue += expand_iocb(ctx, queue + on_queue, io);
	}

	return on_queue;
}

static int
expand_event(struct opioctx *ctx,
	     struct io_event *event, struct io_event *queue, int idx)
{
	int err;
	struct iocb *io;
	struct io_event *ep;
	struct opio *ophead, *op, *next;

	io     = event->obj;
	ophead = opio_cast(io->data);
	op     = ophead;

        size_t nbytes = iocb_nbytes(io);

	DBG(ctx, "events->res: %lu, nbytes: %lu", event->res, (unsigned long)nbytes);
	if (event->res == nbytes)
		err = 0;
	else if ((int)event->res < 0)
		err = (int)event->res;
	else
		err = -EIO;

	while (op) {
		next    = op->next;
		ep      = &queue[idx++];
		ep->obj = op->iocb;
		ep->res = (err ? err : op->orig_iocb.u.c.nbytes);
		DBG(ctx, "ep->res: %lu", ep->res);
		restore_iocb(op);
		free_opio(ctx, op);
		op      = next;
	}

	return idx;
}

int
io_split(struct opioctx *ctx, struct io_event *events, int num)
{
	int on_queue;
	struct iocb *io;
	struct io_event *ep, *q;

	if (!num)
		return 0;

	on_queue = 0;
	q = ctx->event_queue;
	memcpy(q, events, num * sizeof(struct io_event));

	for (ep = q; num-- > 0; ep++) {
		io = ep->obj;
		if (!iocb_optimized(ctx, io))
			events[on_queue++] = *ep;
		else
			on_queue = expand_event(ctx, ep, events, on_queue);
	}
	print_events(ctx, events, on_queue);

	return on_queue;
}

#if defined(TEST)

#define hmask 0x80000000UL
#define smask 0x40000000UL
#define make_data(idx, is_head, sparse) \
         (void *)((idx) | ((is_head) ? hmask : 0) | ((sparse) ? smask : 0))
#define data_idx(data)          (int)((unsigned long)(data) & (0x0fffffff))
#define data_is_head(data)      (((unsigned long)(data) & hmask) ? 1 : 0)
#define data_is_sparse(data)    (((unsigned long)(data) & smask) ? 1 : 0)

static void
usage(void)
{
	fprintf(stderr, "usage: io_optimize [-n num_runs] "
		"[-i num_iocbs] [-s num_secs] [-r random_seed]\n");
	exit(-1);
}

static int xalloc_cnt, xfree_cnt;
static inline char *
xalloc(int size)
{
	char *buf = malloc(size);
	if (!buf) {
		fprintf(stderr, "xalloc failed\n");
		exit(ENOMEM);
	}
	xalloc_cnt++;
	return buf;
}

static inline void
xfree(void *buf)
{
	free(buf);
	xfree_cnt++;
}

static void
randomize_iocbs(struct iocb **iocbs, int num_iocbs, int num_secs)
{
	int i, j;

	i = 0;
	while (i < num_iocbs) {
		char *buf;
		short type;
		int segs, sparse_mem;
		uint64_t offset, nbytes;
		
		type   = (random() % 10 < 5 ? IO_CMD_PREAD : IO_CMD_PWRITE);
		offset = ((random() % num_secs) << 9);

		if (random() % 10 < 4) {
			segs   = 1;
			nbytes = (((random() % 7) + 1) << 9);
		} else {
			segs   = (random() % 10) + 1;
			nbytes = 4096;
		}

		if (i + segs > num_iocbs)
			segs = (num_iocbs - i);

		sparse_mem = (random() % 10 < 2 ? 1 : 0);

		if (sparse_mem)
			buf = xalloc(nbytes);
		else
			buf = xalloc(segs * nbytes);

		for (j = 0; j < segs; j++) {
			struct iocb *io    = iocbs[i + j];
			io->aio_lio_opcode = type;
			io->u.c.nbytes     = nbytes;
			io->u.c.offset     = offset;
			io->u.c.buf        = buf;
			offset            += nbytes;

			io->data = make_data(i + j, (j == 0), sparse_mem);

			if (j + 1 < segs && sparse_mem)
				buf  = xalloc(nbytes);
			else
				buf += nbytes;
		}

		i += segs;
	}
}

static int
simulate_io(struct iocb **iocbs, struct io_event *events, int num_iocbs)
{
	int i, done;
	struct iocb *io;
	struct io_event *ep;

	if (num_iocbs > 1)
		done = (random() % (num_iocbs - 1)) + 1;
	else
		done = num_iocbs;

	for (i = 0; i < done; i++) {
		io      = iocbs[i];
		ep      = &events[i];
		ep->obj = io;
		ep->res = (random() % 10 < 8 ? iocb_nbytes(io) : 0);
	}

	return done;
}

static inline void
process_events(struct opioctx *ctx, 
	       struct iocb *iocb_list, struct io_event *events, int num)
{
	int i;
	struct iocb *io;

	for (i = 0; i < num; i++) {
		io = events[i].obj;
		print_iocb(ctx, io);
		if (data_idx(io->data) != (io - iocb_list)) {
			printf("corrupt data! data_idx = %d, io = %ld\n",
			       data_idx(io->data), (io - iocb_list));
			exit(-1);
		}
		if (data_is_head(io->data) || data_is_sparse(io->data))
			xfree(io->u.c.buf);
		memset(io, 0, sizeof(struct iocb));
	}
}

static inline void
init_optest(struct iocb *iocb_list, 
	    struct iocb **iocbs, struct io_event *events, int num)
{
	int i;

	memset(iocb_list, 0, num * sizeof(struct iocb));
	memset(events, 0, num * sizeof(struct io_event));

	for (i = 0; i < num; i++)
		iocbs[i]  = &iocb_list[i];
}

static void
print_iocbs(struct opioctx *ctx, struct iocb **iocbs, int num_iocbs)
{
	int i;
	char pref[10];
	struct iocb *io;

	DBG(ctx, "iocbs:\n");
	for (i = 0; i < num_iocbs; i++) {
		io = iocbs[i];
		snprintf(pref, 10, "%d: ", i);
		__print_iocb(ctx, io, pref);
	}
}

int
main(int argc, char **argv)
{
	uint64_t num_secs;
	struct opioctx ctx;
	struct io_event *events;
	int i, c, num_runs, num_iocbs, seed;
	struct iocb *iocb_list, **iocbs, **ioqueue;

	num_runs  = 1;
	num_iocbs = 300;
	seed      = time(NULL);
	num_secs  = ((4ULL << 20) >> 9); /* 4GB disk */

	while ((c = getopt(argc, argv, "n:i:s:r:h")) != -1) {
		switch (c) {
		case 'n':
			num_runs  = atoi(optarg);
			break;
		case 'i':
			num_iocbs = atoi(optarg);
			break;
		case 's':
			num_secs  = strtoull(optarg, NULL, 10);
			break;
		case 'r':
			seed      = atoi(optarg);
			break;
		case 'h':
			usage();
		case '?':
			fprintf(stderr, "Unrecognized option: -%c\n", optopt);
			usage();
		}
	}

	printf("Running %d tests with %d iocbs on %lu sectors, seed = %d\n",
	       num_runs, num_iocbs, num_secs, seed);

	srand(seed);

	iocb_list = malloc(num_iocbs * sizeof(struct iocb));
	iocbs     = malloc(num_iocbs * sizeof(struct iocb *));
	events    = malloc(num_iocbs * sizeof(struct io_event));
	
	if (!iocb_list || !iocbs || !events || opio_init(&ctx, num_iocbs)) {
		fprintf(stderr, "initialization failed\n");
		exit(ENOMEM);
	}

	for (i = 0; i < num_runs; i++) {
		int op_rem, op_done, num_split, num_events, num_done;

		ioqueue = iocbs;
		init_optest(iocb_list, ioqueue, events, num_iocbs);
		randomize_iocbs(ioqueue, num_iocbs, num_secs);
		print_iocbs(&ctx, ioqueue, num_iocbs);

		op_done  = 0;
		num_done = 0;
		op_rem   = io_merge(&ctx, ioqueue, num_iocbs);
		print_iocbs(&ctx, ioqueue, op_rem);
		print_merged_iocbs(&ctx, ioqueue, op_rem);
		
		while (num_done < num_iocbs) {
			DBG(&ctx, "optimized remaining: %d\n", op_rem);

			DBG(&ctx, "simulating\n");
			num_events = simulate_io(ioqueue + op_done, events, op_rem);
			print_events(&ctx, events, num_events);

			DBG(&ctx, "splitting %d\n", num_events);
			num_split = io_split(&ctx, events, num_events);
			print_events(&ctx, events, num_split);

			DBG(&ctx, "processing %d\n", num_split);
			process_events(&ctx, iocb_list, events, num_split);

			op_rem   -= num_events;
			op_done  += num_events;
			num_done += num_split;
		}

		DBG(&ctx, "run %d: processed: %d, xallocs: %d, xfrees: %d\n", 
		    i, num_done, xalloc_cnt, xfree_cnt);
		if (xalloc_cnt != xfree_cnt)
			exit(-1);
		xalloc_cnt = xfree_cnt = 0;
	}

	free(iocbs);
	free(events);
	free(iocb_list);
	opio_free(&ctx);

	return 0;
}
#endif
