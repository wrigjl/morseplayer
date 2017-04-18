/*
 * Copyright (c) 2003-2005,2017 Jason L. Wright (jason@thought.net)
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
#include "portaudio.h"
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
#include <float.h>

#ifndef BYTE_ORDER
#error "no byte order defined"
#endif

struct a_sound {
	u_int len;
	float *buf;
};

struct play_list {
	STAILQ_ENTRY(play_list) pl_nxt;
	struct a_sound *pl_snd;
	float *pl_ptr;
	u_int pl_res;
};

struct play_head {
	STAILQ_HEAD(, play_list)	l;
	int nent;
	int nsamps;
};

struct s_params {
	u_int sp_rate;		/* sample rate */
	float sp_hz;		/* audio frequency */
	u_int sp_ditlen;	/* dit length */
	u_int sp_dahlen;	/* dah length */
	u_int sp_inCharlen;	/* interCharacter length */
	u_int sp_sampthresh;	/* threshold for queuing more audio */
	u_int sp_blocksize;	/* audio block size */
	int sp_seenspace;	/* seen a space character? */
	PaStream *sp_stream;	/* audio stream */
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
int mp_callback(const void *inputBuffer, void *outputBuffer,
		unsigned long framesPerBuffer,
		const PaStreamCallbackTimeInfo *timeInfo,
		PaStreamCallbackFlags statusFlags, void *userData);
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
	STAILQ_INIT(&playhead.l);
	STAILQ_INIT(&pl_freelist.l);
	playhead.nent = 0;
	playhead.nsamps = 0;
}

void
playlist_destroy(void)
{
	struct play_list *e;

	while (!STAILQ_EMPTY(&playhead.l)) {
		e = STAILQ_FIRST(&playhead.l);
		STAILQ_REMOVE_HEAD(&playhead.l, pl_nxt);
		pl_free(e);
	}
	while (!STAILQ_EMPTY(&pl_freelist.l)) {
		e = STAILQ_FIRST(&pl_freelist.l);
		STAILQ_REMOVE_HEAD(&pl_freelist.l, pl_nxt);
		x_free(e);
	}
}

void
playlist_enqueue(struct play_list *e)
{
	STAILQ_INSERT_TAIL(&playhead.l, e, pl_nxt);
	playhead.nent++;
	playhead.nsamps += e->pl_snd->len;
}

struct play_list *
pl_alloc(void)
{
	struct play_list *e;

	if (STAILQ_EMPTY(&pl_freelist.l)) {
		e = x_malloc(sizeof(*e));
		if (e == NULL)
			return (NULL);
		STAILQ_INSERT_TAIL(&pl_freelist.l, e, pl_nxt);
	}
	e = STAILQ_FIRST(&pl_freelist.l);
	STAILQ_REMOVE_HEAD(&pl_freelist.l, pl_nxt);
	return (e);
}

void
pl_free(struct play_list *e)
{
	STAILQ_INSERT_TAIL(&pl_freelist.l, e, pl_nxt);
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
main_loop(struct s_params *pars)
{
	struct pollfd fds[2];
	int iseof = 0;

	fds[0].fd = -1;
	fds[0].events = POLLOUT;
	fds[1].events = POLLIN;

	for (;;) {
		if (playhead.nsamps < pars->sp_sampthresh && !iseof)
			fds[1].fd = fileno(stdin);
		else
			fds[1].fd = -1;

		if (poll(fds, sizeof(fds)/sizeof(fds[0]), 1000) == -1)
			err(1, "poll");

		if (fds[1].revents & POLLIN) {
			/* go grab some data from stdin and queue it */
			if (fetch_chars(fds[1].fd, pars))
				break;
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
	dit_samps = pars->sp_ditlen;
	charu = ((float)dit_samps) / 2.0;
	snd->len = rintf(samplen - charu);
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
	ditsamp = (float)pars->sp_ditlen / 2.0;
	inCharSamp = pars->sp_inCharlen;
	m = inCharSamp + ditsamp;
	snd->len = (u_int)rintf(samplen - m);
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
	u_int i, attack1, attack2, attack3, nsamps, idx;
	float u, m;

	u = 1.2 / charwpm;
	nsamps = rintf((units + 1.0) * u * (float)pars->sp_rate);
	snd->len = nsamps;
	snd->buf = (float *)x_malloc(snd->len * sizeof(float));
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
		float t = (float)i / (float)pars->sp_rate;

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

		snd->buf[idx] = m * sinf(t * 2.0f * M_PI * pars->sp_hz);
		idx++;
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
	perword += 10 * dit.len;
	perword += 4 * dah.len;
	perword += 5 * inChar.len;
	perword += 1 * inWord.len;
	
	sampmin = (float)pars->sp_rate * 60.0;
	m = sampmin/perword;
	e = (fabs(m - overallwpm)/overallwpm) * 100;
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
	int c;
	PaError error;
	float cwpm = -1.0, owpm = -1.0, pitch = -1.0;

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
	pars.sp_hz = pitch;

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

	error = Pa_Initialize();
	if (error != paNoError)
		err(1, "portaudio: %s", Pa_GetErrorText(error));

	pars.sp_rate = 44100;
	pars.sp_sampthresh = pars.sp_rate;
	error = Pa_OpenDefaultStream(&pars.sp_stream,
				     0, /* no input */
				     2, /* stereo output */
				     paFloat32, /* float output */
				     pars.sp_rate, /* sample rate */
				     paFramesPerBufferUnspecified,
				     mp_callback,
				     NULL);
	if (error != paNoError) {
		warn("portaudio: %s", Pa_GetErrorText(error));
		if ((error = Pa_Terminate()) != paNoError)
			err(1, "portaudio: %s", Pa_GetErrorText(error));
		exit(1);
	}

	if ((error = Pa_StartStream(pars.sp_stream)) != paNoError) {
		warn("portaudio: StartStream: %s", Pa_GetErrorText(error));
		Pa_CloseStream(pars.sp_stream);
		Pa_Terminate();
		exit(1);
	}

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

	if ((error = Pa_Terminate()) != paNoError)
		err(1, "portaudio: %s", Pa_GetErrorText(error));

	return (0);
}

int
mp_callback(const void *inputBuffer, void *outputBuffer,
	    unsigned long framesPerBuffer,
	    const PaStreamCallbackTimeInfo *timeInfo,
	    PaStreamCallbackFlags statusFlags,
	    void *userData) {
	float *out = outputBuffer;
	unsigned int i;

	for (i = 0; i < framesPerBuffer; i++) {
		struct play_list *e;
		
		if (STAILQ_EMPTY(&playhead.l)) {
			/* nothing queued, play silence */
			*out++ = 0.0f;
			*out++ = 0.0f;
			continue;
		}

		e = STAILQ_FIRST(&playhead.l);
		*out++ = *e->pl_ptr;
		*out++ = *e->pl_ptr;
		e->pl_ptr++;
		e->pl_res--;
		playhead.nsamps--;
		if (e->pl_res == 0) {
			playhead.nent--;
			STAILQ_REMOVE_HEAD(&playhead.l, pl_nxt);
			pl_free(e);
		}
	}
	return (0);
}
