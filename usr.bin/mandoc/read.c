/*	$Id: read.c,v 1.47 2014/07/09 11:30:07 schwarze Exp $ */
/*
 * Copyright (c) 2008, 2009, 2010, 2011 Kristaps Dzonsons <kristaps@bsd.lv>
 * Copyright (c) 2010-2014 Ingo Schwarze <schwarze@openbsd.org>
 * Copyright (c) 2010, 2012 Joerg Sonnenberger <joerg@netbsd.org>
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
#include <sys/stat.h>
#include <sys/mman.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mandoc.h"
#include "mandoc_aux.h"
#include "libmandoc.h"
#include "mdoc.h"
#include "man.h"

#define	REPARSE_LIMIT	1000

struct	buf {
	char		 *buf; /* binary input buffer */
	size_t		  sz; /* size of binary buffer */
};

struct	mparse {
	enum mandoclevel  file_status; /* status of current parse */
	enum mandoclevel  wlevel; /* ignore messages below this */
	int		  line; /* line number in the file */
	int		  options; /* parser options */
	struct man	 *pman; /* persistent man parser */
	struct mdoc	 *pmdoc; /* persistent mdoc parser */
	struct man	 *man; /* man parser */
	struct mdoc	 *mdoc; /* mdoc parser */
	struct roff	 *roff; /* roff parser (!NULL) */
	char		 *sodest; /* filename pointed to by .so */
	int		  reparse_count; /* finite interp. stack */
	mandocmsg	  mmsg; /* warning/error message handler */
	const char	 *file;
	struct buf	 *secondary;
	const char	 *defos; /* default operating system */
};

static	void	  resize_buf(struct buf *, size_t);
static	void	  mparse_buf_r(struct mparse *, struct buf, int);
static	void	  pset(const char *, int, struct mparse *);
static	int	  read_whole_file(struct mparse *, const char *, int,
				struct buf *, int *);
static	void	  mparse_end(struct mparse *);
static	void	  mparse_parse_buffer(struct mparse *, struct buf,
			const char *);

static	const enum mandocerr	mandoclimits[MANDOCLEVEL_MAX] = {
	MANDOCERR_OK,
	MANDOCERR_WARNING,
	MANDOCERR_WARNING,
	MANDOCERR_ERROR,
	MANDOCERR_FATAL,
	MANDOCERR_MAX,
	MANDOCERR_MAX
};

static	const char * const	mandocerrs[MANDOCERR_MAX] = {
	"ok",

	"generic warning",

	/* related to the prologue */
	"missing .TH macro, using \"unknown 1\"",
	"lower case character in document title",
	"unknown manual section",
	"unknown manual volume or arch",
	"missing date, using today's date",
	"cannot parse date, using it verbatim",
	"prologue macros out of order",
	"duplicate prologue macro",
	"incomplete prologue, terminated by",
	"skipping prologue macro in body",

	/* related to document structure */
	".so is fragile, better use ln(1)",
	"no document body",
	"content before first section header",
	"first section is not \"NAME\"",
	"bad NAME section contents",
	"sections out of conventional order",
	"duplicate section title",
	"unexpected section",

	/* related to macros and nesting */
	"obsolete macro",
	"skipping paragraph macro",
	"moving paragraph macro out of list",
	"skipping no-space macro",
	"blocks badly nested",
	"nested displays are not portable",
	"moving content out of list",
	".Vt block has child macro",
	"fill mode already enabled, skipping .fi",
	"fill mode already disabled, skipping .nf",
	"line scope broken",

	/* related to missing macro arguments */
	"skipping empty request",
	"conditional request controls empty scope",
	"skipping empty macro",
	"empty argument, using 0n",
	"argument count wrong",
	"missing display type, using -ragged",
	"list type is not the first argument",
	"missing -width in -tag list, using 8n",
	"empty head in list item",
	"empty list item",
	"missing font type, using \\fR",
	"unknown font type, using \\fR",
	"missing -std argument, adding it",

	/* related to bad macro arguments */
	"skipping argument",
	"unterminated quoted argument",
	"duplicate argument",
	"skipping duplicate display type",
	"skipping duplicate list type",
	"unknown AT&T UNIX version",
	"invalid content in Rs block",
	"invalid Boolean argument",
	"unknown font, skipping request",

	/* related to plain text */
	"blank line in fill mode, using .sp",
	"tab in filled text",
	"whitespace at end of input line",
	"bad comment style",
	"invalid escape sequence",
	"undefined string, using \"\"",

	"generic error",

	/* related to equations */
	"unexpected equation scope closure",
	"equation scope open on exit",
	"overlapping equation scopes",
	"unexpected end of equation",
	"equation syntax error",

	/* related to tables */
	"bad table syntax",
	"bad table option",
	"bad table layout",
	"no table layout cells specified",
	"no table data cells specified",
	"ignore data in cell",
	"data block still open",
	"ignoring extra data cells",

	/* related to document structure and macros */
	"input stack limit exceeded, infinite loop?",
	"skipping bad character",
	"skipping unknown macro",
	"skipping column outside column list",
	"skipping end of block that is not open",
	"inserting missing end of block",
	"appending missing end of block",

	/* related to request and macro arguments */
	"escaped character not allowed in a name",
	"manual name not yet set",
	"argument count wrong",
	"unknown standard specifier",
	"uname(3) system call failed",
	"request requires a numeric argument",
	"missing list type, using -item",
	"skipping all arguments",
	"skipping excess arguments",

	"generic fatal error",

	"input too large",
	"not a manual",
	"column syntax is inconsistent",
	"NOT IMPLEMENTED: .Bd -file",
	"child violates parent syntax",
	"argument count wrong, violates syntax",
	"NOT IMPLEMENTED: .so with absolute path or \"..\"",
	".so request failed",
	"no document prologue",
	"static buffer exhausted",

	/* system errors */
	NULL,
	"cannot stat file",
	"cannot read file",
};

static	const char * const	mandoclevels[MANDOCLEVEL_MAX] = {
	"SUCCESS",
	"RESERVED",
	"WARNING",
	"ERROR",
	"FATAL",
	"BADARG",
	"SYSERR"
};


static void
resize_buf(struct buf *buf, size_t initial)
{

	buf->sz = buf->sz > initial/2 ? 2 * buf->sz : initial;
	buf->buf = mandoc_realloc(buf->buf, buf->sz);
}

static void
pset(const char *buf, int pos, struct mparse *curp)
{
	int		 i;

	/*
	 * Try to intuit which kind of manual parser should be used.  If
	 * passed in by command-line (-man, -mdoc), then use that
	 * explicitly.  If passed as -mandoc, then try to guess from the
	 * line: either skip dot-lines, use -mdoc when finding `.Dt', or
	 * default to -man, which is more lenient.
	 *
	 * Separate out pmdoc/pman from mdoc/man: the first persists
	 * through all parsers, while the latter is used per-parse.
	 */

	if ('.' == buf[0] || '\'' == buf[0]) {
		for (i = 1; buf[i]; i++)
			if (' ' != buf[i] && '\t' != buf[i])
				break;
		if ('\0' == buf[i])
			return;
	}

	if (MPARSE_MDOC & curp->options) {
		if (NULL == curp->pmdoc)
			curp->pmdoc = mdoc_alloc(
			    curp->roff, curp, curp->defos,
			    MPARSE_QUICK & curp->options ? 1 : 0);
		assert(curp->pmdoc);
		curp->mdoc = curp->pmdoc;
		return;
	} else if (MPARSE_MAN & curp->options) {
		if (NULL == curp->pman)
			curp->pman = man_alloc(curp->roff, curp,
			    MPARSE_QUICK & curp->options ? 1 : 0);
		assert(curp->pman);
		curp->man = curp->pman;
		return;
	}

	if (pos >= 3 && 0 == memcmp(buf, ".Dd", 3))  {
		if (NULL == curp->pmdoc)
			curp->pmdoc = mdoc_alloc(
			    curp->roff, curp, curp->defos,
			    MPARSE_QUICK & curp->options ? 1 : 0);
		assert(curp->pmdoc);
		curp->mdoc = curp->pmdoc;
		return;
	}

	if (NULL == curp->pman)
		curp->pman = man_alloc(curp->roff, curp,
		    MPARSE_QUICK & curp->options ? 1 : 0);
	assert(curp->pman);
	curp->man = curp->pman;
}

/*
 * Main parse routine for an opened file.  This is called for each
 * opened file and simply loops around the full input file, possibly
 * nesting (i.e., with `so').
 */
static void
mparse_buf_r(struct mparse *curp, struct buf blk, int start)
{
	const struct tbl_span	*span;
	struct buf	 ln;
	enum rofferr	 rr;
	int		 i, of, rc;
	int		 pos; /* byte number in the ln buffer */
	int		 lnn; /* line number in the real file */
	unsigned char	 c;

	memset(&ln, 0, sizeof(struct buf));

	lnn = curp->line;
	pos = 0;

	for (i = 0; i < (int)blk.sz; ) {
		if (0 == pos && '\0' == blk.buf[i])
			break;

		if (start) {
			curp->line = lnn;
			curp->reparse_count = 0;
		}

		while (i < (int)blk.sz && (start || '\0' != blk.buf[i])) {

			/*
			 * When finding an unescaped newline character,
			 * leave the character loop to process the line.
			 * Skip a preceding carriage return, if any.
			 */

			if ('\r' == blk.buf[i] && i + 1 < (int)blk.sz &&
			    '\n' == blk.buf[i + 1])
				++i;
			if ('\n' == blk.buf[i]) {
				++i;
				++lnn;
				break;
			}

			/*
			 * Make sure we have space for at least
			 * one backslash and one other character
			 * and the trailing NUL byte.
			 */

			if (pos + 2 >= (int)ln.sz)
				resize_buf(&ln, 256);

			/*
			 * Warn about bogus characters.  If you're using
			 * non-ASCII encoding, you're screwing your
			 * readers.  Since I'd rather this not happen,
			 * I'll be helpful and replace these characters
			 * with "?", so we don't display gibberish.
			 * Note to manual writers: use special characters.
			 */

			c = (unsigned char) blk.buf[i];

			if ( ! (isascii(c) &&
			    (isgraph(c) || isblank(c)))) {
				mandoc_msg(MANDOCERR_BADCHAR, curp,
				    curp->line, pos, NULL);
				i++;
				ln.buf[pos++] = '?';
				continue;
			}

			/* Trailing backslash = a plain char. */

			if ('\\' != blk.buf[i] || i + 1 == (int)blk.sz) {
				ln.buf[pos++] = blk.buf[i++];
				continue;
			}

			/*
			 * Found escape and at least one other character.
			 * When it's a newline character, skip it.
			 * When there is a carriage return in between,
			 * skip that one as well.
			 */

			if ('\r' == blk.buf[i + 1] && i + 2 < (int)blk.sz &&
			    '\n' == blk.buf[i + 2])
				++i;
			if ('\n' == blk.buf[i + 1]) {
				i += 2;
				++lnn;
				continue;
			}

			if ('"' == blk.buf[i + 1] || '#' == blk.buf[i + 1]) {
				i += 2;
				/* Comment, skip to end of line */
				for (; i < (int)blk.sz; ++i) {
					if ('\n' == blk.buf[i]) {
						++i;
						++lnn;
						break;
					}
				}

				/* Backout trailing whitespaces */
				for (; pos > 0; --pos) {
					if (ln.buf[pos - 1] != ' ')
						break;
					if (pos > 2 && ln.buf[pos - 2] == '\\')
						break;
				}
				break;
			}

			/* Catch escaped bogus characters. */

			c = (unsigned char) blk.buf[i+1];

			if ( ! (isascii(c) &&
			    (isgraph(c) || isblank(c)))) {
				mandoc_msg(MANDOCERR_BADCHAR, curp,
				    curp->line, pos, NULL);
				i += 2;
				ln.buf[pos++] = '?';
				continue;
			}

			/* Some other escape sequence, copy & cont. */

			ln.buf[pos++] = blk.buf[i++];
			ln.buf[pos++] = blk.buf[i++];
		}

		if (pos >= (int)ln.sz)
			resize_buf(&ln, 256);

		ln.buf[pos] = '\0';

		/*
		 * A significant amount of complexity is contained by
		 * the roff preprocessor.  It's line-oriented but can be
		 * expressed on one line, so we need at times to
		 * readjust our starting point and re-run it.  The roff
		 * preprocessor can also readjust the buffers with new
		 * data, so we pass them in wholesale.
		 */

		of = 0;

		/*
		 * Maintain a lookaside buffer of all parsed lines.  We
		 * only do this if mparse_keep() has been invoked (the
		 * buffer may be accessed with mparse_getkeep()).
		 */

		if (curp->secondary) {
			curp->secondary->buf = mandoc_realloc(
			    curp->secondary->buf,
			    curp->secondary->sz + pos + 2);
			memcpy(curp->secondary->buf +
			    curp->secondary->sz,
			    ln.buf, pos);
			curp->secondary->sz += pos;
			curp->secondary->buf
				[curp->secondary->sz] = '\n';
			curp->secondary->sz++;
			curp->secondary->buf
				[curp->secondary->sz] = '\0';
		}
rerun:
		rr = roff_parseln(curp->roff, curp->line,
		    &ln.buf, &ln.sz, of, &of);

		switch (rr) {
		case ROFF_REPARSE:
			if (REPARSE_LIMIT >= ++curp->reparse_count)
				mparse_buf_r(curp, ln, 0);
			else
				mandoc_msg(MANDOCERR_ROFFLOOP, curp,
				    curp->line, pos, NULL);
			pos = 0;
			continue;
		case ROFF_APPEND:
			pos = (int)strlen(ln.buf);
			continue;
		case ROFF_RERUN:
			goto rerun;
		case ROFF_IGN:
			pos = 0;
			continue;
		case ROFF_ERR:
			assert(MANDOCLEVEL_FATAL <= curp->file_status);
			break;
		case ROFF_SO:
			if (0 == (MPARSE_SO & curp->options) &&
			    (i >= (int)blk.sz || '\0' == blk.buf[i])) {
				curp->sodest = mandoc_strdup(ln.buf + of);
				free(ln.buf);
				return;
			}
			/*
			 * We remove `so' clauses from our lookaside
			 * buffer because we're going to descend into
			 * the file recursively.
			 */
			if (curp->secondary)
				curp->secondary->sz -= pos + 1;
			mparse_readfd(curp, -1, ln.buf + of);
			if (MANDOCLEVEL_FATAL <= curp->file_status) {
				mandoc_vmsg(MANDOCERR_SO_FAIL,
				    curp, curp->line, pos,
				    ".so %s", ln.buf + of);
				break;
			}
			pos = 0;
			continue;
		default:
			break;
		}

		/*
		 * If we encounter errors in the recursive parse, make
		 * sure we don't continue parsing.
		 */

		if (MANDOCLEVEL_FATAL <= curp->file_status)
			break;

		/*
		 * If input parsers have not been allocated, do so now.
		 * We keep these instanced between parsers, but set them
		 * locally per parse routine since we can use different
		 * parsers with each one.
		 */

		if ( ! (curp->man || curp->mdoc))
			pset(ln.buf + of, pos - of, curp);

		/*
		 * Lastly, push down into the parsers themselves.  One
		 * of these will have already been set in the pset()
		 * routine.
		 * If libroff returns ROFF_TBL, then add it to the
		 * currently open parse.  Since we only get here if
		 * there does exist data (see tbl_data.c), we're
		 * guaranteed that something's been allocated.
		 * Do the same for ROFF_EQN.
		 */

		rc = -1;

		if (ROFF_TBL == rr)
			while (NULL != (span = roff_span(curp->roff))) {
				rc = curp->man ?
				    man_addspan(curp->man, span) :
				    mdoc_addspan(curp->mdoc, span);
				if (0 == rc)
					break;
			}
		else if (ROFF_EQN == rr)
			rc = curp->mdoc ?
			    mdoc_addeqn(curp->mdoc,
				roff_eqn(curp->roff)) :
			    man_addeqn(curp->man,
				roff_eqn(curp->roff));
		else if (curp->man || curp->mdoc)
			rc = curp->man ?
			    man_parseln(curp->man,
				curp->line, ln.buf, of) :
			    mdoc_parseln(curp->mdoc,
				curp->line, ln.buf, of);

		if (0 == rc) {
			assert(MANDOCLEVEL_FATAL <= curp->file_status);
			break;
		} else if (2 == rc)
			break;

		/* Temporary buffers typically are not full. */

		if (0 == start && '\0' == blk.buf[i])
			break;

		/* Start the next input line. */

		pos = 0;
	}

	free(ln.buf);
}

static int
read_whole_file(struct mparse *curp, const char *file, int fd,
		struct buf *fb, int *with_mmap)
{
	struct stat	 st;
	size_t		 off;
	ssize_t		 ssz;

	if (-1 == fstat(fd, &st)) {
		curp->file_status = MANDOCLEVEL_SYSERR;
		if (curp->mmsg)
			(*curp->mmsg)(MANDOCERR_SYSSTAT, curp->file_status,
			    file, 0, 0, strerror(errno));
		return(0);
	}

	/*
	 * If we're a regular file, try just reading in the whole entry
	 * via mmap().  This is faster than reading it into blocks, and
	 * since each file is only a few bytes to begin with, I'm not
	 * concerned that this is going to tank any machines.
	 */

	if (S_ISREG(st.st_mode)) {
		if (st.st_size >= (1U << 31)) {
			curp->file_status = MANDOCLEVEL_FATAL;
			if (curp->mmsg)
				(*curp->mmsg)(MANDOCERR_TOOLARGE,
				    curp->file_status, file, 0, 0, NULL);
			return(0);
		}
		*with_mmap = 1;
		fb->sz = (size_t)st.st_size;
		fb->buf = mmap(NULL, fb->sz, PROT_READ, MAP_SHARED, fd, 0);
		if (fb->buf != MAP_FAILED)
			return(1);
	}

	/*
	 * If this isn't a regular file (like, say, stdin), then we must
	 * go the old way and just read things in bit by bit.
	 */

	*with_mmap = 0;
	off = 0;
	fb->sz = 0;
	fb->buf = NULL;
	for (;;) {
		if (off == fb->sz) {
			if (fb->sz == (1U << 31)) {
				curp->file_status = MANDOCLEVEL_FATAL;
				if (curp->mmsg)
					(*curp->mmsg)(MANDOCERR_TOOLARGE,
					    curp->file_status,
					    file, 0, 0, NULL);
				break;
			}
			resize_buf(fb, 65536);
		}
		ssz = read(fd, fb->buf + (int)off, fb->sz - off);
		if (ssz == 0) {
			fb->sz = off;
			return(1);
		}
		if (ssz == -1) {
			curp->file_status = MANDOCLEVEL_SYSERR;
			if (curp->mmsg)
				(*curp->mmsg)(MANDOCERR_SYSREAD,
				    curp->file_status, file, 0, 0,
				    strerror(errno));
			break;
		}
		off += (size_t)ssz;
	}

	free(fb->buf);
	fb->buf = NULL;
	return(0);
}

static void
mparse_end(struct mparse *curp)
{

	if (MANDOCLEVEL_FATAL <= curp->file_status)
		return;

	if (curp->mdoc && ! mdoc_endparse(curp->mdoc)) {
		assert(MANDOCLEVEL_FATAL <= curp->file_status);
		return;
	}

	if (curp->man && ! man_endparse(curp->man)) {
		assert(MANDOCLEVEL_FATAL <= curp->file_status);
		return;
	}

	if ( ! (curp->mdoc || curp->man || curp->sodest)) {
		mandoc_msg(MANDOCERR_NOTMANUAL, curp, 0, 0, NULL);
		curp->file_status = MANDOCLEVEL_FATAL;
		return;
	}

	roff_endparse(curp->roff);
}

static void
mparse_parse_buffer(struct mparse *curp, struct buf blk, const char *file)
{
	const char	*svfile;
	static int	 recursion_depth;

	if (64 < recursion_depth) {
		mandoc_msg(MANDOCERR_ROFFLOOP, curp, curp->line, 0, NULL);
		return;
	}

	/* Line number is per-file. */
	svfile = curp->file;
	curp->file = file;
	curp->line = 1;
	recursion_depth++;

	mparse_buf_r(curp, blk, 1);

	if (0 == --recursion_depth && MANDOCLEVEL_FATAL > curp->file_status)
		mparse_end(curp);

	curp->file = svfile;
}

enum mandoclevel
mparse_readfd(struct mparse *curp, int fd, const char *file)
{
	struct buf	 blk;
	int		 with_mmap;

	if (-1 == fd && -1 == (fd = open(file, O_RDONLY, 0))) {
		curp->file_status = MANDOCLEVEL_SYSERR;
		if (curp->mmsg)
			(*curp->mmsg)(MANDOCERR_SYSOPEN,
			    curp->file_status,
			    file, 0, 0, strerror(errno));
		goto out;
	}

	/*
	 * Run for each opened file; may be called more than once for
	 * each full parse sequence if the opened file is nested (i.e.,
	 * from `so').  Simply sucks in the whole file and moves into
	 * the parse phase for the file.
	 */

	if ( ! read_whole_file(curp, file, fd, &blk, &with_mmap))
		goto out;

	mparse_parse_buffer(curp, blk, file);

	if (with_mmap)
		munmap(blk.buf, blk.sz);
	else
		free(blk.buf);

	if (STDIN_FILENO != fd && -1 == close(fd))
		perror(file);
out:
	return(curp->file_status);
}

struct mparse *
mparse_alloc(int options, enum mandoclevel wlevel,
		mandocmsg mmsg, const char *defos)
{
	struct mparse	*curp;

	assert(wlevel <= MANDOCLEVEL_FATAL);

	curp = mandoc_calloc(1, sizeof(struct mparse));

	curp->options = options;
	curp->wlevel = wlevel;
	curp->mmsg = mmsg;
	curp->defos = defos;

	curp->roff = roff_alloc(curp, options);
	return(curp);
}

void
mparse_reset(struct mparse *curp)
{

	roff_reset(curp->roff);

	if (curp->mdoc)
		mdoc_reset(curp->mdoc);
	if (curp->man)
		man_reset(curp->man);
	if (curp->secondary)
		curp->secondary->sz = 0;

	curp->file_status = MANDOCLEVEL_OK;
	curp->mdoc = NULL;
	curp->man = NULL;

	free(curp->sodest);
	curp->sodest = NULL;
}

void
mparse_free(struct mparse *curp)
{

	if (curp->pmdoc)
		mdoc_free(curp->pmdoc);
	if (curp->pman)
		man_free(curp->pman);
	if (curp->roff)
		roff_free(curp->roff);
	if (curp->secondary)
		free(curp->secondary->buf);

	free(curp->secondary);
	free(curp->sodest);
	free(curp);
}

void
mparse_result(struct mparse *curp,
	struct mdoc **mdoc, struct man **man, char **sodest)
{

	if (sodest && NULL != (*sodest = curp->sodest)) {
		*mdoc = NULL;
		*man = NULL;
		return;
	}
	if (mdoc)
		*mdoc = curp->mdoc;
	if (man)
		*man = curp->man;
}

void
mandoc_vmsg(enum mandocerr t, struct mparse *m,
		int ln, int pos, const char *fmt, ...)
{
	char		 buf[256];
	va_list		 ap;

	va_start(ap, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	mandoc_msg(t, m, ln, pos, buf);
}

void
mandoc_msg(enum mandocerr er, struct mparse *m,
		int ln, int col, const char *msg)
{
	enum mandoclevel level;

	level = MANDOCLEVEL_FATAL;
	while (er < mandoclimits[level])
		level--;

	if (level < m->wlevel)
		return;

	if (m->mmsg)
		(*m->mmsg)(er, level, m->file, ln, col, msg);

	if (m->file_status < level)
		m->file_status = level;
}

const char *
mparse_strerror(enum mandocerr er)
{

	return(mandocerrs[er]);
}

const char *
mparse_strlevel(enum mandoclevel lvl)
{
	return(mandoclevels[lvl]);
}

void
mparse_keep(struct mparse *p)
{

	assert(NULL == p->secondary);
	p->secondary = mandoc_calloc(1, sizeof(struct buf));
}

const char *
mparse_getkeep(const struct mparse *p)
{

	assert(p->secondary);
	return(p->secondary->sz ? p->secondary->buf : NULL);
}
