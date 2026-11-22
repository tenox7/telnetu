/* wsim.c - a stand-in for w3b1.c, for tests on a Mac. There is no window:
 * the keys come from a script, and the screen goes to PNG and text files.
 *
 * TNSIM is the script, words with spaces between them:
 *	wN	wait N ms
 *	kTEXT	type TEXT: \r \n \e \t \s (space) \\ \xHH, ^X is ctrl-X
 *	Kname	a 3B1 key: up down left right home end pgup pgdn ins del
 *		btab exit help redraw f1 ... f16
 *	sFILE	the screen as a PNG, 720 x 288 (SFILE: twice as big)
 *	tFILE	the screen as text (UTF-8)
 *	q	the end (also the end of the script): Exit, Exit
 * TNSIMLOG, if set, gets the title, prompt and bell lines.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <zlib.h>
#include "tn.h"

int mx, my, mb;

static char *script, *sp;
static long waitend;
static char word[600];
static int wpos, wlen, inword;
static FILE *logf;

static long now(void)
{
	struct timeval tv;

	gettimeofday(&tv, 0);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

int d_open(void)
{
	char *l = getenv("TNSIMLOG");

	script = getenv("TNSIM");
	if (!script)
		script = "";
	sp = script;
	if (l && *l)
		logf = fopen(l, "w");
	return 0;
}

void d_close(void)
{
	if (logf)
		fclose(logf);
	logf = 0;
}

void d_blit(int y, int h)
{
	(void)y;
	(void)h;
}

/* ---- pictures ---- */

static void be32(unsigned char *p, unsigned long v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void chunk(FILE *f, char *type, unsigned char *d, unsigned long n)
{
	unsigned char h[8];
	unsigned long c;

	be32(h, n);
	memcpy(h + 4, type, 4);
	fwrite(h, 1, 8, f);
	fwrite(d, 1, n, f);
	c = crc32(0, h + 4, 4);
	c = crc32(c, d, n);
	be32(h, c);
	fwrite(h, 1, 4, f);
}

static void png(char *name, int scale)
{
	int w = COLS * CW * scale, h = PXH * scale, x, y;
	unsigned long rl = w * 3 + 1, n = rl * h, zn;
	unsigned char *raw = malloc(n), *z, hd[13];
	FILE *f;

	for (y = 0; y < h; y++) {
		unsigned char *p = raw + y * rl;

		*p++ = 0;
		for (x = 0; x < w; x++) {
			int sx = x / scale, sy = y / scale;
			int on = bm[sy][sx >> 4] >> (sx & 15) & 1;

			*p++ = on ? 0xe0 : 0x10;
			*p++ = on ? 0xff : 0x10;
			*p++ = on ? 0xe0 : 0x10;
		}
	}
	zn = compressBound(n);
	z = malloc(zn);
	compress2(z, &zn, raw, n, 9);
	f = fopen(name, "wb");
	if (f) {
		fwrite("\211PNG\r\n\032\n", 1, 8, f);
		be32(hd, w);
		be32(hd + 4, h);
		hd[8] = 8;
		hd[9] = 2;
		hd[10] = hd[11] = hd[12] = 0;
		chunk(f, "IHDR", hd, 13);
		chunk(f, "IDAT", z, zn);
		chunk(f, "IEND", hd, 0);
		fclose(f);
	}
	free(raw);
	free(z);
}

static void utf8(FILE *f, unsigned c)
{
	if (c < 0x80)
		putc(c, f);
	else if (c < 0x800) {
		putc(0xc0 | c >> 6, f);
		putc(0x80 | (c & 0x3f), f);
	} else {
		putc(0xe0 | c >> 12, f);
		putc(0x80 | (c >> 6 & 0x3f), f);
		putc(0x80 | (c & 0x3f), f);
	}
}

static void text(char *name)
{
	FILE *f = fopen(name, "w");
	int x, y, e;

	if (!f)
		return;
	for (y = 0; y < ROWS; y++) {
		for (e = COLS; e > 0 && (SCR[y][e - 1].ch == ' ' ||
			SCR[y][e - 1].ch == 0); e--)
			;
		for (x = 0; x < e; x++)
			if (SCR[y][x].ch)
				utf8(f, SCR[y][x].ch);
		putc('\n', f);
	}
	fclose(f);
}

/* ---- the script ---- */

static int nextword(void)
{
	int n = 0;

	while (*sp == ' ' || *sp == '\n' || *sp == '\t')
		sp++;
	if (!*sp)
		return 0;
	while (*sp && *sp != ' ' && *sp != '\n' && *sp != '\t' &&
		n < (int)sizeof word - 1)
		word[n++] = *sp++;
	word[n] = 0;
	wlen = n;
	wpos = 1;
	return 1;
}

static int hex(int c)
{
	return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0';
}

static int named(char *s)
{
	static struct { char *n; int k; } t[] = {
		{"up", K_UP}, {"down", K_DOWN}, {"left", K_LEFT},
		{"right", K_RIGHT}, {"home", K_HOME}, {"end", K_END},
		{"pgup", K_PGUP}, {"pgdn", K_PGDN}, {"ins", K_INS},
		{"del", K_DEL}, {"btab", K_BTAB}, {"exit", K_EXIT},
		{"help", K_HELP}, {"redraw", K_REDRAW}, {0, 0}
	};
	int i;

	if (s[0] == 'f' && s[1] >= '1' && s[1] <= '9')
		return K_F1 + atoi(s + 1) - 1;
	for (i = 0; t[i].n; i++)
		if (!strcmp(t[i].n, s))
			return t[i].k;
	return -1;
}

int d_key(int wait)
{
	int c;

	for (;;) {
		if (waitend) {
			long left = waitend - now();

			if (left > 0) {
				if (wait)
					usleep((left > 100 ? 100 : left) * 1000);
				return -1;
			}
			waitend = 0;
		}
		if (inword && word[0] == 'k' && wpos < wlen) {
			c = word[wpos++] & 0xff;
			if (c == '\\' && wpos < wlen) {
				c = word[wpos++];
				switch (c) {
				case 'r': return '\r';
				case 'n': return '\n';
				case 'e': return 033;
				case 't': return '\t';
				case 's': return ' ';
				case 'x':
					c = hex(word[wpos]) * 16 + hex(word[wpos + 1]);
					wpos += 2;
					return c;
				}
				return c;
			}
			if (c == '^' && wpos < wlen) {
				c = word[wpos++];
				return c == '?' ? 0177 : c & 037;
			}
			return c;
		}
		inword = 0;
		if (!nextword())
			return K_EXIT;
		inword = 1;
		switch (word[0]) {
		case 'w':
			waitend = now() + atol(word + 1);
			inword = 0;
			continue;
		case 'k':
			continue;
		case 'K':
			inword = 0;
			c = named(word + 1);
			if (c >= 0)
				return c;
			continue;
		case 's':
		case 'S':
			r_draw(0);
			png(word + 1, word[0] == 'S' ? 2 : 1);
			inword = 0;
			continue;
		case 't':
			text(word + 1);
			inword = 0;
			continue;
		case 'q':
			return K_EXIT;
		}
		inword = 0;
	}
}

void d_title(char *s)
{
	if (logf)
		fprintf(logf, "title: %s\n", s);
}

void d_prompt(char *s)
{
	if (logf)
		fprintf(logf, "prompt: %s\n", s);
}

void d_bell(void)
{
	if (logf)
		fprintf(logf, "bell\n");
}

int d_shell(void)
{
	return -1;
}
