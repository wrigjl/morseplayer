/*
 * Copyright (c) 2003-2005 Jason L. Wright (jason@thought.net)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * A morse code player based on ARRL timing.
 *
 * References:
 * "A Standard for Morse Timing Using the Farnsworth Technique", QEX,
 *  April 1990 (http://www.arrl.org/files/infoserv/tech/code-std.txt)
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/audioio.h>
#include <sys/queue.h>
#include <sys/poll.h>
#include <string.h>
#include <fcntl.h>
#include <err.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <sysexits.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#ifndef BYTE_ORDER
#error "no byte order defined"
#endif

struct a_sound {
	u_int len;
	void *buf;
};

struct play_list {
	SIMPLEQ_ENTRY(play_list) pl_nxt;
	struct a_sound *pl_snd;
	int8_t *pl_ptr;
	u_int pl_res;
};

struct play_head {
	SIMPLEQ_HEAD(, play_list)	l;
	int nent;
	int nsamps;
};

struct s_params {
	u_int sp_rate;		/* sample rate */
	double sp_hz;		/* audio frequency */
	u_int sp_ditlen;	/* dit length */
	u_int sp_dahlen;	/* dah length */
	u_int sp_inCharlen;	/* interCharacter length */
	u_int sp_sampthresh;	/* threshold for queuing more audio */
	u_int sp_blocksize;	/* audio block size */
	u_int sp_channels;	/* number of channels */
	u_int sp_precision;	/* sample precision (bits) */
	int sp_fd;		/* audio file descriptor */
	int sp_seenspace;	/* seen a space character? */
};

int diagmode;
double overallwpm = 20.0;
double charwpm = 20.0;
struct a_sound dit, dah, inChar, inWord, quietBlock;
struct play_head playhead, pl_freelist;

int build_dit(struct a_sound *, struct s_params *);
int build_dah(struct a_sound *, struct s_params *);
int build_snd(struct a_sound *, struct s_params *, double);
int build_silence(struct a_sound *, struct s_params *);
int build_inChar(struct a_sound *, struct s_params *);
int build_inWord(struct a_sound *, struct s_params *);
int build_quiet(struct a_sound *, struct s_params *);

void playlist_init(void);
void playlist_destroy(void);
void playlist_enqueue(struct play_list *);
void pl_free(struct play_list *);
struct play_list *pl_alloc(void);
int enqueue_sound(struct a_sound *, u_int);
void init_sounds(void);
void destroy_sound(struct a_sound *);
void destroy_sounds(void);
int build_sounds(struct s_params *);
void play_string(const char *);
void convert_char(unsigned char, struct s_params *);
int fetch_chars(int, struct s_params *);
int feed_audio(struct s_params *);
int main_loop(struct s_params *);
void time_check(struct s_params *);
void test_times(struct s_params *);
int getfloat(const char *, float *);
void check_chars(void);

#define	x_malloc(x)	xx_malloc((x), __FILE__, __LINE__)
#define	x_free(x)	xx_free((x), __FILE__, __LINE__)
void *xx_malloc(size_t, const char *, const int);
void xx_free(void *, const char *, const int);

void *
xx_malloc(size_t sz, const char *fname, const int lineno)
{
	void *m;

	m = malloc(sz);
	if (m == NULL)
		err(1, "malloc(%s:%d:%ld", fname, lineno, (long)sz);
	if (diagmode > 2)
		printf("M:%p:%lu:%s:%d\n", m, (unsigned long)sz,
		    fname, lineno);
	return (m);
}

void
xx_free(void *m, const char *fname, const int lineno)
{
	if (diagmode > 2)
		printf("F:%p:%s:%d\n", m, fname, lineno);
	free(m);
}

void
playlist_init(void)
{
	SIMPLEQ_INIT(&playhead.l);
	SIMPLEQ_INIT(&pl_freelist.l);
	playhead.nent = 0;
	playhead.nsamps = 0;
}

void
playlist_destroy(void)
{
	struct play_list *e;

	while (!SIMPLEQ_EMPTY(&playhead.l)) {
		e = SIMPLEQ_FIRST(&playhead.l);
		SIMPLEQ_REMOVE_HEAD(&playhead.l, pl_nxt);
		pl_free(e);
	}
	while (!SIMPLEQ_EMPTY(&pl_freelist.l)) {
		e = SIMPLEQ_FIRST(&pl_freelist.l);
		SIMPLEQ_REMOVE_HEAD(&pl_freelist.l, pl_nxt);
		x_free(e);
	}
}

void
playlist_enqueue(struct play_list *e)
{
	SIMPLEQ_INSERT_TAIL(&playhead.l, e, pl_nxt);
	playhead.nent++;
	playhead.nsamps += e->pl_snd->len;
}

struct play_list *
pl_alloc(void)
{
	struct play_list *e;

	if (SIMPLEQ_EMPTY(&pl_freelist.l)) {
		e = x_malloc(sizeof(*e));
		if (e == NULL)
			return (NULL);
		SIMPLEQ_INSERT_TAIL(&pl_freelist.l, e, pl_nxt);
	}
	e = SIMPLEQ_FIRST(&pl_freelist.l);
	SIMPLEQ_REMOVE_HEAD(&pl_freelist.l, pl_nxt);
	return (e);
}

void
pl_free(struct play_list *e)
{
	SIMPLEQ_INSERT_TAIL(&pl_freelist.l, e, pl_nxt);
}

int
enqueue_sound(struct a_sound *snd, u_int len)
{
	struct play_list *e;

	e = pl_alloc();
	if (e == NULL)
		return (-1);
	e->pl_snd = snd;
	e->pl_ptr = snd->buf;
	e->pl_res = len;
	playlist_enqueue(e);
	return (0);
}

const struct morse_char {
	unsigned char c;
	char *m;
} morse_chars[] = {
	{ 'a', ".-" },
	{ 'b', "-..." },
	{ 'c', "-.-." },
	{ 'd', "-.." },
	{ 'e', "." },
	{ 'f', "..-." },
	{ 'g', "--." },
	{ 'h', "...." },
	{ 'i', ".." },
	{ 'j', ".---" },
	{ 'k', "-.-" },
	{ 'l', ".-.." },
	{ 'm', "--" },
	{ 'n', "-." },
	{ 'o', "---" },
	{ 'p', ".--." },
	{ 'q', "--.-" },
	{ 'r', ".-." },
	{ 's', "..." },
	{ 't', "-" },
	{ 'u', "..-" },
	{ 'v', "...-" },
	{ 'w', ".--" },
	{ 'x', "-..-" },
	{ 'y', "-.--" },
	{ 'z', "--.." },
	{ '0', "-----" },
	{ '1', ".----" },
	{ '2', "..---" },
	{ '3', "...--" },
	{ '4', "....-" },
	{ '5', "....." },
	{ '6', "-...." },
	{ '7', "--..." },
	{ '8', "---.." },
	{ '9', "----." },
	{ '/', "-..-." },
	{ '?', "..--.." },
	{ ',', "--..--" },
	{ '.', ".-.-.-" },
	{ '*', "...-.-" },	/* SK */
	{ '+', ".-.-." },	/* AR */
	{ '=', "-...-" },	/* BT */
	{ '|', ".-..." },	/* AS */
	{ '\0', NULL },
};

void
check_chars(void)
{
	const struct morse_char *mc = morse_chars;
	const char *p;

	for (mc = morse_chars; mc->c != '\0'; mc++) {
		for (p = mc->m; *p != '\0'; p++) {
			if (*p == '.' || *p == '-')
				continue;
			fprintf(stderr, "invalid char 0x%02x in %c\n",
			    *p, mc->c);
			break;
		}
	}
}

void
play_string(const char *str)
{
	const char *p;

	for (p = str; *p; p++) {
		switch (*p) {
		case '.':
			enqueue_sound(&dit, dit.len);
			break;
		case '-':
			enqueue_sound(&dah, dah.len);
			break;
		default:
			errx(1, "invalid character %c in mtable", *p);
		}
	}
	enqueue_sound(&inChar, inChar.len);
}

void
convert_char(unsigned char c, struct s_params *pars)
{
	int i;

	if (c < 0x80 && isupper(c))
		c = tolower(c);
	for (i = 0; morse_chars[i].c; i++) {
		if (morse_chars[i].c == c) {
			play_string(morse_chars[i].m);
			return;
		}
	}
}

int
fetch_chars(int fd, struct s_params *pars)
{
	unsigned char c[64];
	ssize_t r, i;

	r = read(fd, c, sizeof(c));
	if (r == -1)
		err(1, "read");
	if (r == 0)
		return (1);
	for (i = 0; i < r; i++) {
		if (c[i] >= 0x80 || !isspace(c[i])) {
			convert_char(c[i], pars);
			pars->sp_seenspace = 0;
		} else if (pars->sp_seenspace == 0) {
			enqueue_sound(&inWord, inWord.len);
			pars->sp_seenspace = 1;
		}
	}
	return (0);
}

int
feed_audio(struct s_params *pars)
{
	ssize_t r;
	u_int res;

	for (res = pars->sp_blocksize; res != 0;) {
		struct play_list *e;

		if (SIMPLEQ_EMPTY(&playhead.l)) {
			r = write(pars->sp_fd, quietBlock.buf, res);
			if (r == -1)
				err(1, "write");
			break;
		}

		e = SIMPLEQ_FIRST(&playhead.l);
		if (e->pl_res > res) {
			r = write(pars->sp_fd, e->pl_ptr, res);
			if (r == -1)
				err(1, "write");
			e->pl_ptr += res;
			e->pl_res -= res;
			playhead.nsamps -= res;
			break;
		}

		r = write(pars->sp_fd, e->pl_ptr, e->pl_res);
		if (r == -1)
			err(1, "write");
		playhead.nsamps -= e->pl_res;
		res -= e->pl_res;
		playhead.nent--;
		SIMPLEQ_REMOVE_HEAD(&playhead.l, pl_nxt);
		pl_free(e);
	}
	return (0);
}

int
main_loop(struct s_params *pars)
{
	struct pollfd fds[2];
	ssize_t r;
	int iseof = 0;

	/*
	 * first, play one block of silence... /dev/audio
	 * isn't poll-able until it's been kicked.
	 */
	r = write(pars->sp_fd, quietBlock.buf, quietBlock.len);
	if (r == -1)
		err(1, "write");
	
	fds[0].fd = pars->sp_fd;
	fds[0].events = POLLOUT;
	fds[1].events = POLLIN;

	for (;;) {
		if (playhead.nsamps < pars->sp_sampthresh && !iseof)
			fds[1].fd = fileno(stdin);
		else
			fds[1].fd = -1;

		if (poll(fds, sizeof(fds)/sizeof(fds[0]), INFTIM) == -1)
			err(1, "poll");

		if (fds[1].revents & POLLIN) {
			/* go grab some data from stdin and queue it */
			if (fetch_chars(fds[1].fd, pars))
				iseof = 1;
		}

		if (fds[0].revents & POLLOUT) {
			/* feed the audio device */
			feed_audio(pars);
			if (iseof && SIMPLEQ_EMPTY(&playhead.l)) {
				if (ioctl(fds[0].fd, AUDIO_DRAIN, NULL) == -1)
					err(1, "audio_drain");
				break;
			}
		}
	}

	return (0);
}

int
build_inChar(struct a_sound *snd, struct s_params *pars)
{
	float samplen, charu;
	u_int dit_samps;

	if (overallwpm >= charwpm) {
		float u;

		u = 1.2 / overallwpm;
		samplen = 3.0 * u * (float)pars->sp_rate;
	} else {
		float Ta, Tc;

		Ta = ((60.0 * charwpm) - (37.2 * overallwpm)) /
		    (charwpm * overallwpm);
		Tc = (3.0 * Ta) / 19.0;
		samplen = Tc * (float)pars->sp_rate;
	}
	dit_samps = pars->sp_ditlen /
	    (pars->sp_channels * (pars->sp_precision / 8));
	charu = ((float)dit_samps) / 2.0;
	snd->len = rintf(samplen - charu) *
	    pars->sp_channels * (pars->sp_precision / 8);
	pars->sp_inCharlen = snd->len;
	return (build_silence(snd, pars));
}

int
build_inWord(struct a_sound *snd, struct s_params *pars)
{
	float samplen, m;
	float inCharSamp, ditsamp;

	if (overallwpm >= charwpm) {
		float u;

		u = 1.2 / overallwpm;
		samplen = 7.0 * u * (float)pars->sp_rate;
	} else {
		float Ta, Tc;

		Ta = ((60.0 * charwpm) - (37.2 * overallwpm)) /
		    (charwpm * overallwpm);
		Tc = (7.0 * Ta) / 19.0;
		samplen = Tc * (float)pars->sp_rate;
	}
	ditsamp = ((float)pars->sp_ditlen / 2.0) /
	    (pars->sp_channels * (pars->sp_precision / 8));
	inCharSamp = pars->sp_inCharlen /
	    (pars->sp_channels * (pars->sp_precision / 8));
	m = inCharSamp + ditsamp;
	snd->len = (u_int)rintf(samplen - m) *
	    pars->sp_channels * (pars->sp_precision / 8);
	return (build_silence(snd, pars));
}

int
build_quiet(struct a_sound *snd, struct s_params *pars)
{
	snd->len = pars->sp_blocksize;
	return (build_silence(snd, pars));
}

int
build_dit(struct a_sound *snd, struct s_params *pars)
{
	int r;

	r = build_snd(snd, pars, 1.0);
	if (r != 0)
		return (r);
	pars->sp_ditlen = snd->len;
	return (r);
}

int
build_dah(struct a_sound *snd, struct s_params *pars)
{
	int r;

	r = build_snd(snd, pars, 3.0);
	if (r != 0)
		return (r);
	pars->sp_dahlen = snd->len;
	return (r);
}

int
build_snd(struct a_sound *snd, struct s_params *pars, double units)
{
	u_int i, j, attack1, attack2, attack3, nsamps, idx;
	float u, m, prec;

	switch (pars->sp_precision) {
	case 8:
		prec = 127.0;
		break;
	case 16:
		prec = 32767.0;
		break;
	default:
		return (-1);
	}

	u = 1.2 / charwpm;
	nsamps = rintf((units + 1.0) * u * (float)pars->sp_rate);
	snd->len = nsamps * pars->sp_channels * (pars->sp_precision / 8);
	snd->buf = (int8_t *)x_malloc(snd->len);
	if (snd->buf == NULL)
		return (-1);
	attack2 = (u_int)rintf(units * u * (float)pars->sp_rate);

	u = (1.2 / charwpm) * 0.2;

	if (u > 0.006)
		u = 0.006;

	u *= (float)pars->sp_rate;
	attack1 = (u_int)u;
	attack3 = attack1 + attack2;


	for (i = 0, idx = 0; i < nsamps; i++) {
		double d;
		float t = (double)i / (double)pars->sp_rate;

		if (i < attack1) {
			float T = (double)attack1 / (double)pars->sp_rate;
			float RC = T / 5.0;

			m = 1.0 * (1 - expf(-t / RC));
			m = 1.0;
		} else if (i > attack2 && i < attack3) {
			float T = (double)attack1 / (double)pars->sp_rate;
			float RC = T / 5.0;
			float q = (double)(i - attack2) / (double)pars->sp_rate;

			m = 1.0 * expf(-q / RC);
		} else if (i >= attack3)
			m = 0.0;
		else
			m = 1.0;

		d = rintf(m * prec * sinf(t * 2 * M_PI * pars->sp_hz));

		if (pars->sp_precision == 8) {
			for (j = 0; j < pars->sp_channels; j++) {
				int8_t *bp = (int8_t *)snd->buf;

				bp[idx++] = d;
			}
		} else if (pars->sp_precision == 16) {
			for (j = 0; j < pars->sp_channels; j++) {
				int16_t *bp = (int16_t *)snd->buf;
				bp[idx++] = d;
			}
		}
	}
	return (0);
}

int
build_silence(struct a_sound *snd, struct s_params *pars)
{
	snd->buf = x_malloc(snd->len);
	if (snd->buf == NULL)
		return (-1);
	memset(snd->buf, '\0', snd->len);
	return (0);
}

void
init_sounds(void)
{
	dit.len = 0; dit.buf = NULL;
	dah.len = 0; dah.buf = NULL;
	inChar.len = 0; inChar.buf = NULL;
	inWord.len = 0; inWord.buf = NULL;
	quietBlock.len = 0; quietBlock.buf = NULL;
}

void
destroy_sound(struct a_sound *snd)
{
	snd->len = 0;
	if (snd->buf != NULL) {
		x_free(snd->buf);
		snd->buf = NULL;
	}
}

void
destroy_sounds(void)
{
	destroy_sound(&dit);
	destroy_sound(&dah);
	destroy_sound(&inChar);
	destroy_sound(&inWord);
	destroy_sound(&quietBlock);
}

int
build_sounds(struct s_params *pars)
{
	int r;

	if ((r = build_dit(&dit, pars)) != 0) {
		destroy_sounds();
		return (r);
	}
	if ((r = build_dah(&dah, pars)) != 0) {
		destroy_sounds();
		return (r);
	}
	if ((r = build_inChar(&inChar, pars)) != 0) {
		destroy_sounds();
		return (r);
	}
	if ((r = build_inWord(&inWord, pars)) != 0) {
		destroy_sounds();
		return (r);
	}
	if ((r = build_quiet(&quietBlock, pars)) != 0) {
		destroy_sounds();
		return (r);
	}
	return (0);
}

void
time_check(struct s_params *pars)
{
	float sampmin;
	float perword, e, m;

	perword = 0;
	perword += 10 * dit.len / pars->sp_channels / (pars->sp_precision / 8);
	perword += 4 * dah.len / pars->sp_channels / (pars->sp_precision / 8);
	perword += 5 * inChar.len / pars->sp_channels / (pars->sp_precision / 8);
	perword += 1 * inWord.len / pars->sp_channels / (pars->sp_precision / 8);
	sampmin = (float)pars->sp_rate * 60.0;
	m = sampmin/perword;
	e = (fabsf(m - overallwpm)/overallwpm) * 100;
	if (e > 1.0) {
		printf("dit %u dah %u inChar %u inWord %u\n",
		    dit.len, dah.len, inChar.len, inWord.len);
		printf("sampmin %f / perword %f = %f wpm (target %f), "
		    "error %.2f%%\n", sampmin, perword, m, overallwpm, e);
	}
}

void
test_times(struct s_params *pars)
{
	float c, o, maxwpm = 100.0;
	double old_owpm, old_cwpm;

	old_owpm = overallwpm;
	old_cwpm = charwpm;
	init_sounds();
	for (o = 1.0; o <= maxwpm; o++) {
		for (c = o; c <= maxwpm; c++) {
			overallwpm = o;
			charwpm = c;
			build_sounds(pars);
			time_check(pars);
			destroy_sounds();
		}
	}
	overallwpm = old_owpm;
	charwpm = old_cwpm;
}

int
getfloat(const char *s, float *fp)
{
	char *ep;
	double d;

	errno = 0;
	d = strtod(s, &ep);
	if (s[0] == '\0' || *ep != '\0')
		return (-1);
	if (errno == ERANGE && (d = HUGE_VAL || d == -HUGE_VAL))
		return (-1);
	if (d > FLT_MAX || d < FLT_MIN || isnan(d) || isinf(d))
		return (-1);
	*fp = (float)d;
	return (0);
}

int
main(int argc, char *argv[])
{
	struct s_params pars;
	int f, c;
	audio_info_t ai;
	float cwpm = -1.0, owpm = -1.0, pitch = -1.0;
	char *afname = NULL;

	while ((c = getopt(argc, argv, "c:d:f:w:D")) != EOF) {
		switch (c) {
		case 'c':
			if (getfloat(optarg, &cwpm) ||
			    (cwpm < 1.0 || cwpm > 70.0)) {
				fprintf(stderr, "%s: invalid character rate "
				    "%s (1.0 < r < 70.0)\n", argv[0], optarg);
				return (1);
			}
			break;
		case 'w':
			if (getfloat(optarg, &owpm) ||
			    (owpm < 1.0 || owpm > 70.0)) {
				fprintf(stderr, "%s: invalid overall rate %s "
				    "(1.0 < r < 70.0)\n", argv[0], optarg);
				return (1);
			}
			break;
		case 'f':
			if (getfloat(optarg, &pitch) ||
			    (pitch < 1.0 || pitch > 20000.0)) {
				fprintf(stderr, "%s: invalid frequency %s "
				    "(1.0 < r < 20000.0)\n", argv[0], optarg);
				return (1);
			}
			break;
		case 'd':
			afname = optarg;
			break;
		case 'D':
			diagmode++;
			break;
		case '?':
		default:
			fprintf(stderr, "%s [-d /dev/audio] [-c cwpm] "
			    "[-w owpm] [-f freq] [-D]\n", argv[0]);
			return (1);
		}
	}

	if (pitch == -1.0)
		pitch = 720.0;

	if (owpm == -1.0 && cwpm == -1.0) {
		/* Neither specified, assume Element 1 rates */
		owpm = 5.0;
		cwpm = 18.0;
	} else if (cwpm == -1.0) {
		/* Overall set, assume ARRL Farnsworth rules */
		if (owpm > 18.0)
			cwpm = owpm;
		else
			cwpm = 18.0;
	} else if (owpm == -1.0) {
		/* Character set, assume overall == cwpm */
		owpm = cwpm;
	} else {
		/* both set, ensure sanity */
		if (owpm > cwpm) {
			fprintf(stderr, "%s: character rate %f < "
			    "overall rate %f\n", argv[0], cwpm, owpm);
			return (1);
		}
	}
	overallwpm = owpm;
	charwpm = cwpm;

	if (afname == NULL)
		afname = "/dev/audio";

	f = open(afname, O_WRONLY, 0);
	if (f == -1)
		err(1, "open %s", afname);

	AUDIO_INITINFO(&ai);
	/* ai.play.sample_rate = 22050; */
	ai.play.encoding = AUDIO_ENCODING_SLINEAR;
	ai.play.precision = 8;
	ai.play.channels = 1;
	if (ioctl(f, AUDIO_SETINFO, &ai) == -1) {
#if BYTE_ORDER == LITTLE_ENDIAN
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
#elif BYTE_ORDER == BIG_ENDIAN
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_BE;
#else
# error "oh please, get the pdp outta here."
#endif
		ai.play.precision = 16;
		ai.play.channels = 2;
		if (ioctl(f, AUDIO_SETINFO, &ai) == -1) {
			err(1, "setinfo");
		}
	}

	if (ioctl(f, AUDIO_GETINFO, &ai) == -1)
		err(1, "getinfo");
	pars.sp_rate = ai.play.sample_rate;
	pars.sp_hz = pitch;
	pars.sp_sampthresh = ai.blocksize * ai.hiwat;
	pars.sp_blocksize = ai.blocksize;
	pars.sp_seenspace = 0;
	pars.sp_fd = f;
	pars.sp_channels = ai.play.channels;
	pars.sp_precision = ai.play.precision;

	if (diagmode > 0) {
		check_chars();
		test_times(&pars);
		return (0);
	}

	playlist_init();
	init_sounds();
	build_sounds(&pars);
	main_loop(&pars);
	destroy_sounds();
	playlist_destroy();

	return (0);
}
