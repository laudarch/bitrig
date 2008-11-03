/*	$OpenBSD: aproc.c,v 1.13 2008/11/03 22:25:13 ratchov Exp $	*/
/*
 * Copyright (c) 2008 Alexandre Ratchov <alex@caoua.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/*
 * aproc structures are simple audio processing units. They are
 * interconnected by abuf structures and form a kind of circuit. aproc
 * structure have call-backs that do the actual processing.
 *
 * This module implements the following processing units:
 *
 *  - rpipe: read end of an unix file (pipe, socket, device...)
 *
 *  - wpipe: write end of an unix file (pipe, socket, device...)
 *
 *  - mix: mix N inputs -> 1 output
 *
 *  - sub: from 1 input -> extract/copy N outputs
 *
 *  - conv: converts/resamples/remaps a single stream
 *
 * TODO
 *
 * 	(easy) split the "conv" into 2 converters: one for input (that
 *	convers anything to 16bit signed) and one for the output (that
 *	converts 16bit signed to anything)
 *
 *	(hard) add a lowpass filter for the resampler. Quality is
 *	not acceptable as is.
 *
 */
#include <err.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "aparams.h"
#include "abuf.h"
#include "aproc.h"
#include "file.h"

struct aproc *
aproc_new(struct aproc_ops *ops, char *name)
{
	struct aproc *p;

	p = malloc(sizeof(struct aproc));
	if (p == NULL)
		err(1, name);
	LIST_INIT(&p->ibuflist);
	LIST_INIT(&p->obuflist);
	p->name = name;
	p->ops = ops;
	return p;
}

void
aproc_del(struct aproc *p)
{
	struct abuf *i;

	DPRINTF("aproc_del: %s(%s): terminating...\n", p->ops->name, p->name);

	if (p->ops->done)
		p->ops->done(p);

	while (!LIST_EMPTY(&p->ibuflist)) {
		i = LIST_FIRST(&p->ibuflist);
		abuf_hup(i);
	}
	while (!LIST_EMPTY(&p->obuflist)) {
		i = LIST_FIRST(&p->obuflist);
		abuf_eof(i);
	}
	DPRINTF("aproc_del: %s(%s): freed\n", p->ops->name, p->name);
	free(p);
}

void
aproc_setin(struct aproc *p, struct abuf *ibuf)
{
	LIST_INSERT_HEAD(&p->ibuflist, ibuf, ient);
	ibuf->rproc = p;
	if (p->ops->newin)
		p->ops->newin(p, ibuf);
}

void
aproc_setout(struct aproc *p, struct abuf *obuf)
{
	LIST_INSERT_HEAD(&p->obuflist, obuf, oent);
	obuf->wproc = p;
	if (p->ops->newout)
		p->ops->newout(p, obuf);
}

void
aproc_ipos(struct aproc *p, struct abuf *ibuf, int delta)
{
	struct abuf *obuf;

	DPRINTFN(3, "aproc_ipos: %s: delta = %d\n", p->name, delta);

	LIST_FOREACH(obuf, &p->obuflist, oent) {
		abuf_ipos(obuf, delta);
	}
}

void
aproc_opos(struct aproc *p, struct abuf *obuf, int delta)
{
	struct abuf *ibuf;

	DPRINTFN(3, "aproc_opos: %s: delta = %d\n", p->name, delta);

	LIST_FOREACH(ibuf, &p->ibuflist, ient) {
		abuf_opos(ibuf, delta);
	}
}

int
rpipe_in(struct aproc *p, struct abuf *ibuf_dummy)
{
	struct abuf *obuf = LIST_FIRST(&p->obuflist);
	struct file *f = p->u.io.file;
	unsigned char *data;
	unsigned count;

	DPRINTFN(3, "rpipe_in: %s\n", p->name);

	if (ABUF_FULL(obuf) || !(f->state & FILE_ROK))
		return 0;
	data = abuf_wgetblk(obuf, &count, 0);
	count = file_read(f, data, count);
	if (count == 0)
		return 0;
	abuf_wcommit(obuf, count);
	if (!abuf_flush(obuf))
		return 0;
	return 1;
}

int
rpipe_out(struct aproc *p, struct abuf *obuf)
{
	struct file *f = p->u.io.file;
	unsigned char *data;
	unsigned count;

	if (f->refs > 0)
		return 0;
	DPRINTFN(3, "rpipe_out: %s\n", p->name);
	
	if (ABUF_FULL(obuf) || !(f->state & FILE_ROK))
		return 0;
	data = abuf_wgetblk(obuf, &count, 0);
	count = file_read(f, data, count);
	if (count == 0)
		return 0;
	abuf_wcommit(obuf, count);
	return 1;
}

void
rpipe_done(struct aproc *p)
{
	struct file *f = p->u.io.file;

	f->rproc = NULL;
	if (f->wproc == NULL)
		file_del(f);
}

void
rpipe_eof(struct aproc *p, struct abuf *ibuf_dummy)
{
	DPRINTFN(3, "rpipe_eof: %s\n", p->name);
	aproc_del(p);
}

void
rpipe_hup(struct aproc *p, struct abuf *obuf)
{
	DPRINTFN(3, "rpipe_hup: %s\n", p->name);
	aproc_del(p);
}

struct aproc_ops rpipe_ops = {
	"rpipe",
	rpipe_in,
	rpipe_out,
	rpipe_eof,
	rpipe_hup,
	NULL, /* newin */
	NULL, /* newout */
	aproc_ipos,
	aproc_opos,
	rpipe_done
};

struct aproc *
rpipe_new(struct file *f)
{
	struct aproc *p;

	p = aproc_new(&rpipe_ops, f->name);
	p->u.io.file = f;
	f->rproc = p;	
	return p;
}

void
wpipe_done(struct aproc *p)
{
	struct file *f = p->u.io.file;

	f->wproc = NULL;
	if (f->rproc == NULL)
		file_del(f);
}

int
wpipe_in(struct aproc *p, struct abuf *ibuf)
{
	struct file *f = p->u.io.file;
	unsigned char *data;
	unsigned count;

	if (f->refs > 0)
		return 0;
	DPRINTFN(3, "wpipe_in: %s\n", p->name);

	if (ABUF_EMPTY(ibuf) || !(f->state & FILE_WOK))
		return 0;
	data = abuf_rgetblk(ibuf, &count, 0);
	count = file_write(f, data, count);
	if (count == 0)
		return 0;
	abuf_rdiscard(ibuf, count);
	return 1;
}

int
wpipe_out(struct aproc *p, struct abuf *obuf_dummy)
{
	struct abuf *ibuf = LIST_FIRST(&p->ibuflist);
	struct file *f = p->u.io.file;
	unsigned char *data;
	unsigned count;

	DPRINTFN(3, "wpipe_out: %s\n", p->name);

	if (!abuf_fill(ibuf)) {
		DPRINTFN(3, "wpipe_out: fill failed\n");       
		return 0;
	}
	if (ABUF_EMPTY(ibuf) || !(f->state & FILE_WOK))
		return 0;
	data = abuf_rgetblk(ibuf, &count, 0);
	if (count == 0) {
		DPRINTF("wpipe_out: %s: underrun\n", p->name);
		return 0;
	}
	count = file_write(f, data, count);
	if (count == 0)
		return 0;
	abuf_rdiscard(ibuf, count);
	return 1;
}

void
wpipe_eof(struct aproc *p, struct abuf *ibuf)
{
	DPRINTFN(3, "wpipe_eof: %s\n", p->name);
	aproc_del(p);
}

void
wpipe_hup(struct aproc *p, struct abuf *obuf_dummy)
{
	DPRINTFN(3, "wpipe_hup: %s\n", p->name);
	aproc_del(p);
}

struct aproc_ops wpipe_ops = {
	"wpipe",
	wpipe_in,
	wpipe_out,
	wpipe_eof,
	wpipe_hup,
	NULL, /* newin */
	NULL, /* newout */
	aproc_ipos,
	aproc_opos,
	wpipe_done
};

struct aproc *
wpipe_new(struct file *f)
{
	struct aproc *p;

	p = aproc_new(&wpipe_ops, f->name);
	p->u.io.file = f;
	f->wproc = p;
	return p;
}

/*
 * Fill an output block with silence.
 */
void
mix_bzero(struct aproc *p)
{
	struct abuf *obuf = LIST_FIRST(&p->obuflist);
	short *odata;
	unsigned ocount;

	DPRINTFN(4, "mix_bzero: used = %u, todo = %u\n",
	    obuf->used, obuf->mixtodo);
	odata = (short *)abuf_wgetblk(obuf, &ocount, obuf->mixtodo);
	ocount -= ocount % obuf->bpf;
	if (ocount == 0)
		return;
	memset(odata, 0, ocount);
	obuf->mixtodo += ocount;
	DPRINTFN(4, "mix_bzero: ocount %u, todo %u\n", ocount, obuf->mixtodo);
}

/*
 * Mix an input block over an output block.
 */
void
mix_badd(struct abuf *ibuf, struct abuf *obuf)
{
	short *idata, *odata;
	int vol = ibuf->mixvol;
	unsigned i, j, icnt, onext, ostart;
	unsigned scount, icount, ocount;

	DPRINTFN(4, "mix_badd: todo = %u, done = %u\n",
	    obuf->mixtodo, ibuf->mixdone);

	idata = (short *)abuf_rgetblk(ibuf, &icount, 0);
	icount /= ibuf->bpf;
	if (icount == 0)
		return;

	odata = (short *)abuf_wgetblk(obuf, &ocount, ibuf->mixdone);
	ocount /= obuf->bpf;
	if (ocount == 0)
		return;

	ostart = ibuf->cmin - obuf->cmin; 
	onext = obuf->cmax - ibuf->cmax + ostart;
	icnt = ibuf->cmax - ibuf->cmin + 1;
	odata += ostart;
	scount = (icount < ocount) ? icount : ocount;
	for (i = scount; i > 0; i--) {
		for (j = icnt; j > 0; j--) {
			*odata += (*idata * vol) >> ADATA_SHIFT;
			idata++;
			odata++;
		}
		odata += onext;
	}
	abuf_rdiscard(ibuf, scount * ibuf->bpf);
	ibuf->mixdone += scount * obuf->bpf;

	DPRINTFN(4, "mix_badd: added %u, done = %u, todo = %u\n",
	    scount, ibuf->mixdone, obuf->mixtodo);
}

int
mix_in(struct aproc *p, struct abuf *ibuf)
{
	struct abuf *i, *inext, *obuf = LIST_FIRST(&p->obuflist);
	unsigned ocount;

	DPRINTFN(4, "mix_in: used = %u, done = %u, todo = %u\n",
	    ibuf->used, ibuf->mixdone, obuf->mixtodo);
		
	if (!ABUF_ROK(ibuf) || ibuf->mixdone == obuf->mixtodo)
		return 0;

	mix_badd(ibuf, obuf);
	ocount = obuf->mixtodo;
	LIST_FOREACH(i, &p->ibuflist, ient) {
		if (ocount > i->mixdone)
			ocount = i->mixdone;
	}
	if (ocount == 0)
		return 0;

	abuf_wcommit(obuf, ocount);
	p->u.mix.lat += ocount / obuf->bpf;
	obuf->mixtodo -= ocount;
	if (!abuf_flush(obuf))
		return 0; /* hup */
	mix_bzero(p);
	for (i = LIST_FIRST(&p->ibuflist); i != NULL; i = inext) {
		inext = LIST_NEXT(i, ient);
		i->mixdone -= ocount;
		if (i->mixdone < obuf->mixtodo)
			mix_badd(i, obuf);
		if (!abuf_fill(i))
			continue;
	}
	return 1;
}

int
mix_out(struct aproc *p, struct abuf *obuf)
{
	struct abuf *i, *inext;
	unsigned ocount, drop;

	DPRINTFN(4, "mix_out: used = %u, todo = %u\n",
	    obuf->used, obuf->mixtodo);

	if (!ABUF_WOK(obuf))
		return 0;

	mix_bzero(p);
	ocount = obuf->mixtodo;
	for (i = LIST_FIRST(&p->ibuflist); i != NULL; i = inext) {
		inext = LIST_NEXT(i, ient);
		if (!abuf_fill(i))
			continue;
		if (!ABUF_ROK(i)) {
			if ((p->u.mix.flags & MIX_DROP) && i->mixdone == 0) {
				if (i->xrun == XRUN_ERROR) {
					abuf_hup(i);
					continue;
				}
				drop = obuf->mixtodo;
				i->mixdone += drop;
				if (i->xrun == XRUN_SYNC)
					i->drop += drop;
				else {
					abuf_opos(i, -(int)(drop / i->bpf));
					if (i->duplex) {
						DPRINTF("mix_out: duplex %u\n",
						    drop);
						i->duplex->drop += drop * 
						    i->duplex->bpf / i->bpf;
						abuf_ipos(i->duplex,
						    -(int)(drop / i->bpf));
					}
				}
				DPRINTF("mix_out: drop = %u\n", i->drop);
			}
		} else
			mix_badd(i, obuf);
		if (ocount > i->mixdone)
			ocount = i->mixdone;
	}
	if (ocount == 0)
		return 0;
	if (LIST_EMPTY(&p->ibuflist) && (p->u.mix.flags & MIX_AUTOQUIT)) {
		DPRINTF("mix_out: nothing more to do...\n");
		aproc_del(p);
		return 0;
	}
	abuf_wcommit(obuf, ocount);
	p->u.mix.lat += ocount / obuf->bpf;
	obuf->mixtodo -= ocount;
	LIST_FOREACH(i, &p->ibuflist, ient) {
		i->mixdone -= ocount;
	}
	return 1;
}

void
mix_eof(struct aproc *p, struct abuf *ibuf)
{
	struct abuf *obuf = LIST_FIRST(&p->obuflist);

	DPRINTF("mix_eof: %s: detached\n", p->name);
	mix_setmaster(p);

	/*
	 * If there's no more inputs, abuf_run() will trigger the eof
	 * condition and propagate it, so no need to handle it here.
	 */
	abuf_run(obuf);
	DPRINTF("mix_eof: done\n");
}

void
mix_hup(struct aproc *p, struct abuf *obuf)
{
	struct abuf *ibuf;

	while (!LIST_EMPTY(&p->ibuflist)) {
		ibuf = LIST_FIRST(&p->ibuflist);
		abuf_hup(ibuf);
	}
	DPRINTF("mix_hup: %s: done\n", p->name);
	aproc_del(p);
}

void
mix_newin(struct aproc *p, struct abuf *ibuf)
{
	struct abuf *obuf = LIST_FIRST(&p->obuflist);

	if (!obuf || ibuf->cmin < obuf->cmin || ibuf->cmax > obuf->cmax) {
		fprintf(stderr, "mix_newin: channel ranges mismatch\n");
		abort();
	}
	ibuf->mixdone = 0;
	ibuf->mixvol = ADATA_UNIT;
	ibuf->xrun = XRUN_IGNORE;
	mix_setmaster(p);
}

void
mix_newout(struct aproc *p, struct abuf *obuf)
{
	obuf->mixtodo = 0;
	mix_bzero(p);
}

void
mix_opos(struct aproc *p, struct abuf *obuf, int delta)
{
	DPRINTFN(3, "mix_opos: lat = %d/%d\n", p->u.mix.lat, p->u.mix.maxlat);
	p->u.mix.lat -= delta;
	aproc_opos(p, obuf, delta);
}

struct aproc_ops mix_ops = {
	"mix",
	mix_in,
	mix_out,
	mix_eof,
	mix_hup,
	mix_newin,
	mix_newout,
	aproc_ipos,
	mix_opos,
	NULL
};

struct aproc *
mix_new(char *name, int maxlat)
{
	struct aproc *p;

	p = aproc_new(&mix_ops, name);
	p->u.mix.flags = 0;
	p->u.mix.lat = 0;
	p->u.mix.maxlat = maxlat;
	return p;
}

void
mix_pushzero(struct aproc *p)
{
	struct abuf *obuf = LIST_FIRST(&p->obuflist);

	abuf_wcommit(obuf, obuf->mixtodo);
	p->u.mix.lat += obuf->mixtodo / obuf->bpf;
	obuf->mixtodo = 0;
	abuf_run(obuf);
	mix_bzero(p);
}

/*
 * Normalize input levels
 */
void
mix_setmaster(struct aproc *p)
{
	unsigned n;
	struct abuf *buf;

	n = 0;
	LIST_FOREACH(buf, &p->ibuflist, ient)
	    n++;
	LIST_FOREACH(buf, &p->ibuflist, ient)
	    buf->mixvol = ADATA_UNIT / n;
}

/*
 * Copy data from ibuf to obuf.
 */
void
sub_bcopy(struct abuf *ibuf, struct abuf *obuf)
{
	short *idata, *odata;
	unsigned i, j, ocnt, inext, istart;
	unsigned icount, ocount, scount;

	idata = (short *)abuf_rgetblk(ibuf, &icount, obuf->subdone);
	icount /= ibuf->bpf;
	if (icount == 0)
		return;
	odata = (short *)abuf_wgetblk(obuf, &ocount, 0);
	ocount /= obuf->bpf;
	if (ocount == 0)
		return;
	istart = obuf->cmin - ibuf->cmin;
	inext = ibuf->cmax - obuf->cmax + istart;
	ocnt = obuf->cmax - obuf->cmin + 1;
	scount = (icount < ocount) ? icount : ocount;
	idata += istart;
	for (i = scount; i > 0; i--) {
		for (j = ocnt; j > 0; j--) {
			*odata = *idata;
			odata++;
			idata++;
		}
		idata += inext;
	}
	abuf_wcommit(obuf, scount * obuf->bpf);	
	obuf->subdone += scount * ibuf->bpf;
	DPRINTFN(4, "sub_bcopy: %u frames\n", scount);
}

int
sub_in(struct aproc *p, struct abuf *ibuf)
{
	struct abuf *i, *inext;
	unsigned done, drop;
	
	if (!ABUF_ROK(ibuf))
		return 0;
	done = ibuf->used;
	for (i = LIST_FIRST(&p->obuflist); i != NULL; i = inext) {
		inext = LIST_NEXT(i, oent);
		if (!ABUF_WOK(i)) {
			if ((p->u.sub.flags & SUB_DROP) && i->subdone == 0) {
				if (i->xrun == XRUN_ERROR) {
					abuf_eof(i);
					continue;
				}
				drop = ibuf->used;
				if (i->xrun == XRUN_SYNC)
					i->silence += drop;
				else {
					abuf_ipos(i, -(int)(drop / i->bpf));
					if (i->duplex) {
						DPRINTF("sub_in: duplex %u\n",
						    drop);
						i->duplex->silence += drop *
						    i->duplex->bpf / i->bpf;
						abuf_opos(i->duplex, 
						    -(int)(drop / i->bpf));
					}
				}
				i->subdone += drop;
				DPRINTF("sub_in: silence = %u\n", i->silence);
			}
		} else
			sub_bcopy(ibuf, i);
		if (done > i->subdone)
			done = i->subdone;
		if (!abuf_flush(i))
			continue;
	}
	LIST_FOREACH(i, &p->obuflist, oent) {
		i->subdone -= done;
	}
	abuf_rdiscard(ibuf, done);
	p->u.sub.lat -= done / ibuf->bpf;
	return 1;
}

int
sub_out(struct aproc *p, struct abuf *obuf)
{
	struct abuf *ibuf = LIST_FIRST(&p->ibuflist);
	struct abuf *i, *inext;
	unsigned done;

	if (!ABUF_WOK(obuf))
		return 0;
	if (!abuf_fill(ibuf)) {
		return 0;
	}
	if (obuf->subdone == ibuf->used)
		return 0;

	done = ibuf->used;
	for (i = LIST_FIRST(&p->obuflist); i != NULL; i = inext) {
		inext = LIST_NEXT(i, oent);
		if (!abuf_flush(i))
			continue;
		sub_bcopy(ibuf, i);
		if (done > i->subdone)
			done = i->subdone;
	}
	LIST_FOREACH(i, &p->obuflist, oent) {
		i->subdone -= done;
	}
	abuf_rdiscard(ibuf, done);
	p->u.sub.lat -= done / ibuf->bpf;
	return 1;
}

void
sub_eof(struct aproc *p, struct abuf *ibuf)
{
	struct abuf *obuf;

	while (!LIST_EMPTY(&p->obuflist)) {
		obuf = LIST_FIRST(&p->obuflist);
		abuf_eof(obuf);
	}
	aproc_del(p);
}

void
sub_hup(struct aproc *p, struct abuf *obuf)
{
	struct abuf *ibuf = LIST_FIRST(&p->ibuflist);

	DPRINTF("sub_hup: %s: detached\n", p->name);
	abuf_run(ibuf);
	DPRINTF("sub_hup: done\n");
}

void
sub_newout(struct aproc *p, struct abuf *obuf)
{
	struct abuf *ibuf = LIST_FIRST(&p->ibuflist);

	if (!ibuf || obuf->cmin < ibuf->cmin || obuf->cmax > ibuf->cmax) {
		fprintf(stderr, "sub_newout: channel ranges mismatch\n");
		abort();
	}
	obuf->subdone = 0;
	obuf->xrun = XRUN_IGNORE;
}

void
sub_ipos(struct aproc *p, struct abuf *ibuf, int delta)
{
	p->u.sub.lat += delta;
	DPRINTFN(3, "sub_ipos: lat = %d/%d\n", p->u.sub.lat, p->u.sub.maxlat);
	aproc_ipos(p, ibuf, delta);
}

struct aproc_ops sub_ops = {
	"sub",
	sub_in,
	sub_out,
	sub_eof,
	sub_hup,
	NULL,
	sub_newout,
	sub_ipos,
	aproc_opos,
	NULL
};

struct aproc *
sub_new(char *name, int maxlat)
{
	struct aproc *p;

	p = aproc_new(&sub_ops, name);
	p->u.sub.flags = 0;
	p->u.sub.lat = 0;
	p->u.sub.maxlat = maxlat;
	return p;
}

/*
 * Convert one block.
 */
void
conv_bcopy(struct aconv *ist, struct aconv *ost,
    struct abuf *ibuf, struct abuf *obuf)
{
	int *ictx;
	unsigned inch, ibps;
	unsigned char *idata;
	int ibnext, isigbit;
	unsigned ishift;
	int isnext;
	unsigned ipos, orate;
	unsigned ifr;
	int *octx;
	unsigned onch, oshift;
	int osigbit;
	unsigned obps;
	unsigned char *odata;
	int obnext, osnext;
	unsigned opos, irate;
	unsigned ofr;
	unsigned c, i;
	int s, *ctx;
	unsigned icount, ocount;

	/*
	 * It's ok to have s uninitialized, but we dont want the compiler to
	 * complain about it.
	 */
	s = (int)0xdeadbeef;

	/*
	 * Calculate max frames readable at once from the input buffer.
	 */
	idata = abuf_rgetblk(ibuf, &icount, 0);
	ifr = icount / ibuf->bpf;
	icount = ifr * ibuf->bpf;

	odata = abuf_wgetblk(obuf, &ocount, 0);
	ofr = ocount / obuf->bpf;
	ocount = ofr * obuf->bpf;

	/*
	 * Partially copy structures into local variables, to avoid
	 * unnecessary indirections; this also allows the compiler to
	 * order local variables more "cache-friendly".
	 */
	ictx = ist->ctx + ist->cmin;
	octx = ist->ctx + ost->cmin;
	inch = ist->nch;
	ibps = ist->bps;
	ibnext = ist->bnext;
	isigbit = ist->sigbit;
	ishift = ist->shift;
	isnext = ist->snext;
	ipos = ist->pos;
	irate = ist->rate;
	onch = ost->nch;
	oshift = ost->shift;
	osigbit = ost->sigbit;
	obps = ost->bps;
	obnext = ost->bnext;
	osnext = ost->snext;
	opos = ost->pos;
	orate = ost->rate;

	/*
	 * Start conversion.
	 */
	idata += ist->bfirst;
	odata += ost->bfirst;
	DPRINTFN(4, "conv_bcopy: ifr=%d ofr=%d\n", ifr, ofr);
	for (;;) {
		if ((int)(ipos - opos) > 0) {
			if (ofr == 0)
				break;
			ctx = octx;
			for (c = onch; c > 0; c--) {
				s = *ctx++ << 16;
				s >>= oshift;
				s ^= osigbit;
				for (i = obps; i > 0; i--) {
					*odata = (unsigned char)s;
					s >>= 8;
					odata += obnext;
				}
				odata += osnext;
			}
			opos += irate;
			ofr--;
		} else {
			if (ifr == 0)
				break;
			ctx = ictx;
			for (c = inch; c > 0; c--) {
				for (i = ibps; i > 0; i--) {
					s <<= 8;
					s |= *idata;
					idata += ibnext;
				}
				s ^= isigbit;
				s <<= ishift;
				*ctx++ = (short)(s >> 16);
				idata += isnext;
			}
			ipos += orate;
			ifr--;
		}
	}
	ist->pos = ipos;
	ost->pos = opos;
	DPRINTFN(4, "conv_bcopy: done, ifr=%d ofr=%d\n", ifr, ofr);

	/*
	 * Update FIFO pointers.
	 */
	icount -= ifr * ist->bpf;
	ocount -= ofr * ost->bpf;
	abuf_rdiscard(ibuf, icount);
	abuf_wcommit(obuf, ocount);
}

int
conv_in(struct aproc *p, struct abuf *ibuf)
{
	struct abuf *obuf = LIST_FIRST(&p->obuflist);

	DPRINTFN(4, "conv_in: %s\n", p->name);

	if (!ABUF_WOK(obuf) || !ABUF_ROK(ibuf))
		return 0;
	conv_bcopy(&p->u.conv.ist, &p->u.conv.ost, ibuf, obuf);
	if (!abuf_flush(obuf))
		return 0;
	return 1;
}

int
conv_out(struct aproc *p, struct abuf *obuf)
{
	struct abuf *ibuf = LIST_FIRST(&p->ibuflist);

	DPRINTFN(4, "conv_out: %s\n", p->name);

	if (!abuf_fill(ibuf))
		return 0;
	if (!ABUF_WOK(obuf) || !ABUF_ROK(ibuf))
		return 0;
	conv_bcopy(&p->u.conv.ist, &p->u.conv.ost, ibuf, obuf);
	return 1;
}

void
conv_eof(struct aproc *p, struct abuf *ibuf)
{
	DPRINTFN(4, "conv_eof: %s\n", p->name);

	aproc_del(p);
}

void
conv_hup(struct aproc *p, struct abuf *obuf)
{
	DPRINTFN(4, "conv_hup: %s\n", p->name);

	aproc_del(p);
}

void
aconv_init(struct aconv *st, struct aparams *par, int input)
{
	unsigned i;

	st->bps = par->bps;
	st->sigbit = par->sig ? 0 : 1 << (par->bits - 1);
	if (par->msb) {
		st->shift = 32 - par->bps * 8;
	} else {
		st->shift = 32 - par->bits;
	}
	if ((par->le && input) || (!par->le && !input)) {
		st->bfirst = st->bps - 1;
		st->bnext = -1;
		st->snext = 2 * st->bps;
	} else {
		st->bfirst = 0;
		st->bnext = 1;
		st->snext = 0;
	}
	st->cmin = par->cmin;
	st->nch = par->cmax - par->cmin + 1;
	st->bpf = st->nch * st->bps;
	st->rate = par->rate;
	st->pos = 0;

	for (i = 0; i < NCHAN_MAX; i++)
		st->ctx[i] = 0;
}

void
conv_ipos(struct aproc *p, struct abuf *ibuf, int delta)
{
	struct abuf *obuf = LIST_FIRST(&p->obuflist);	
	long long ipos;
	int ifac, ofac;

	DPRINTFN(3, "conv_ipos: %d\n", delta);

	ifac = p->u.conv.ist.rate;
	ofac = p->u.conv.ost.rate;
	ipos = p->u.conv.idelta + (long long)delta * ofac;
	delta = (ipos + ifac - 1) / ifac;
	p->u.conv.idelta = ipos - (long long)delta * ifac;
	abuf_ipos(obuf, delta);
}

void
conv_opos(struct aproc *p, struct abuf *obuf, int delta)
{
	struct abuf *ibuf = LIST_FIRST(&p->ibuflist);
	long long opos;
	int ifac, ofac;

	DPRINTFN(3, "conv_opos: %d\n", delta);

	ifac = p->u.conv.ist.rate;
	ofac = p->u.conv.ost.rate;
	opos = p->u.conv.odelta + (long long)delta * ifac;
	delta = (opos + ofac - 1) / ofac;
	p->u.conv.odelta = opos - (long long)delta * ofac;
	abuf_opos(ibuf, delta);
}

struct aproc_ops conv_ops = {
	"conv",
	conv_in,
	conv_out,
	conv_eof,
	conv_hup,
	NULL,
	NULL,
	conv_ipos,
	conv_opos,
	NULL
};

struct aproc *
conv_new(char *name, struct aparams *ipar, struct aparams *opar)
{
	struct aproc *p;

	p = aproc_new(&conv_ops, name);
	aconv_init(&p->u.conv.ist, ipar, 1);
	aconv_init(&p->u.conv.ost, opar, 0);
	p->u.conv.idelta = 0;
	p->u.conv.odelta = 0;
	if (debug_level > 0) {
		DPRINTF("conv_new: %s: ", p->name);		
		aparams_print2(ipar, opar);
		DPRINTF("\n");
	}
	return p;
}
