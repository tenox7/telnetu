/* ttyb.c - telnet on a plain terminal: the serial port, an rlogin or a
 * telnet session into the 3B1, or -t in a window. No emulation here: the
 * host's output goes to the terminal as it comes, as with the old telnet.
 * With -a (charset ascii) UTF-8 becomes ASCII, for terminals that do not
 * know it: lines become + - |, letters lose their accents.
 */
#include <stdio.h>
#ifdef __APPLE__
#include <termios.h>
#include <unistd.h>
#define termio termios
#define TGET(t) tcgetattr(0, t)
#define TSET(t) tcsetattr(0, TCSADRAIN, t)
#else
#include <termio.h>
#define TGET(t) ioctl(0, TCGETA, t)
#define TSET(t) ioctl(0, TCSETAW, t)
#endif
#include "tn.h"

static struct termio tsave;
static int saved, raw = -1, twait = -1;

static void set(int r, int wait)
{
	struct termio t;

	if (!saved || (r == raw && wait == twait))
		return;
	t = tsave;
	if (r) {
		t.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON | IXOFF | ISTRIP);
		t.c_oflag &= ~OPOST;
		t.c_lflag &= ~(ICANON | ECHO | ISIG);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = wait ? 1 : 0;
	}
	TSET(&t);
	raw = r;
	twait = wait;
}

int t_open(void)
{
	if (TGET(&tsave) == 0)
		saved = 1;
	raw = twait = -1;
	set(1, 0);
	return 0;
}

void t_raw(int r)
{
	set(r, twait < 0 ? 0 : twait);
}

void t_close(void)
{
	set(0, 0);
}

int t_key(int wait)
{
	unsigned char c;

	set(1, wait);
	if (read(0, (char *)&c, 1) != 1)
		return -1;
	return c;
}

static void put(char *s, int n)
{
	int r;

	while (n > 0) {
		r = write(1, s, n);
		if (r <= 0)
			return;
		s += r;
		n -= r;
	}
}

/* -a: UTF-8 to ASCII. Bytes that are not UTF-8 are taken as CP437, as a
 * BBS sends them, so its boxes become + - | too */
static unsigned char pend[4];
static int npend, need;
static unsigned long cp;

static void one(char *out, int *k, unsigned long c)
{
	int a = c < 0x10000 ? r_ascii((unsigned)c) : '?';

	if (a > 0)
		out[(*k)++] = a;
	if (a > 0 && vt_wide(c))	/* it takes two columns */
		out[(*k)++] = ' ';
}

static void bad(char *out, int *k)
{
	int i;

	vt.bad += npend;		/* as in vt.c: a host that sends */
	if (vt.bad >= 4 && vt.bad > 2 * vt.good)	/* CP437, not UTF-8 */
		vt.is437 = 1;
	for (i = 0; i < npend; i++)
		one(out, k, vt_cp437(pend[i]));
	npend = need = 0;
}

static void ascii(unsigned char *s, int n)
{
	char out[600];
	int k = 0, c;

	while (n-- > 0) {
		c = *s++;
		if (need && (c & 0xc0) == 0x80) {
			pend[npend++] = c;
			cp = cp << 6 | (c & 0x3f);
			if (--need == 0) {
				if ((npend == 3 && cp < 0x800) || (npend == 4 &&
					(cp < 0x10000 || cp > 0x10ffff)) ||
					(cp >= 0xd800 && cp <= 0xdfff))
					bad(out, &k);
				else {
					one(out, &k, cp);
					npend = 0;
					vt.good++;
				}
			}
		} else {
			if (need)
				bad(out, &k);
			if (c < 0x20 && vt.is437 && vt_c0(c))
				one(out, &k, vt_c0(c));	/* no stray ^E */
			else if (c < 0x80)
				out[k++] = c;
			else if (vt.is437)
				one(out, &k, vt_cp437(c));
			else if (c >= 0xc2 && c <= 0xf4) {
				need = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
				cp = c & (0x3f >> need);
				pend[0] = c;
				npend = 1;
			} else {
				pend[0] = c;
				npend = 1;
				bad(out, &k);
			}
		}
		if (k >= (int)sizeof out - 16) {
			put(out, k);
			k = 0;
		}
	}
	put(out, k);
}

void t_write(char *s, int n)
{
	if (vt.charset == CS_ASCII)
		ascii((unsigned char *)s, n);
	else
		put(s, n);
}
