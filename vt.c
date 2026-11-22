/* vt.c - the terminal. It takes what the host sends and keeps the screen
 * as cells: a letter (Unicode), its attributes and its colours.
 *
 * It knows the VT100, VT220 and xterm codes that hosts use today:
 * cursor moves, scrolling regions, insert and delete, the alternate
 * screen, DEC line drawing (ESC ( 0), SGR with 16, 256 and 24-bit colours,
 * cursor reports (DSR) and device attributes (DA). Codes it does not know
 * are read to the end and dropped, so they never show as garbage.
 *
 * Text is UTF-8. A byte that is not part of UTF-8 is taken as CP437, as
 * BBS art is. "charset" in the program chooses UTF-8 only, Latin-1 or
 * CP437 instead.
 */
#include "tn.h"

Vt vt;

enum { S_GND, S_ESC, S_ESCI, S_CSI, S_STR, S_STRESC };

/* DEC special graphics: 0x5f to 0x7e */
static unsigned short decgr[32] = {
	0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
	0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
	0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
	0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7
};

/* CP437's pictures for control codes, as a BBS sends them. Not for NUL,
 * BEL, BS, HT, LF, VT, FF, CR, SO, SI and ESC: those stay controls */
static unsigned short cp437c0[32] = {
	0, 0x263a, 0x263b, 0x2665, 0x2666, 0x2663, 0x2660, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0x25ba, 0x25c4, 0x2195, 0x203c, 0x00b6, 0x00a7, 0x25ac, 0x21a8,
	0x2191, 0x2193, 0x2192, 0, 0x221f, 0x2194, 0x25b2, 0x25bc
};

/* CP437: 0x80 to 0xff */
static unsigned short cp437[128] = {
	0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
	0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
	0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
	0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
	0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
	0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
	0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
	0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
	0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
	0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
	0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
	0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
	0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
	0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0
};

/* the xterm palette, for 256 and 24-bit colours: nearest of the 16 */
static unsigned char pal16[16][3] = {
	{0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0},
	{0, 0, 238}, {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
	{127, 127, 127}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
	{92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}
};

static void dirty(int y)
{
	if (y >= 0 && y < ROWS)
		vt.dirty[y] = 1;
}

void vt_dirtyall(void)
{
	int y;

	for (y = 0; y < ROWS; y++)
		vt.dirty[y] = 1;
}

/* blanks with the current background (xterm's bce) */
static void blank(Cell *c, int n)
{
	while (n-- > 0) {
		c->ch = ' ';
		c->at = 0;
		c->fg = C_DEF;
		c->bg = vt.bg;
		c++;
	}
}

static void cpcells(Cell *d, Cell *s, int n)
{
	if (d < s)
		while (n-- > 0)
			*d++ = *s++;
	else {
		d += n;
		s += n;
		while (n-- > 0)
			*--d = *--s;
	}
}

/* lines top..bot move up n; blank lines come in at the bottom */
static void scrollup(int top, int bot, int n)
{
	int y;

	if (n > bot - top + 1)
		n = bot - top + 1;
	if (n <= 0)
		return;
	for (y = top; y + n <= bot; y++)
		cpcells(SCR[y], SCR[y + n], COLS);
	for (; y <= bot; y++)
		blank(SCR[y], COLS);
	for (y = top; y <= bot; y++)
		dirty(y);
}

static void scrolldown(int top, int bot, int n)
{
	int y;

	if (n > bot - top + 1)
		n = bot - top + 1;
	if (n <= 0)
		return;
	for (y = bot; y - n >= top; y--)
		cpcells(SCR[y], SCR[y - n], COLS);
	for (; y >= top; y--)
		blank(SCR[y], COLS);
	for (y = top; y <= bot; y++)
		dirty(y);
}

/* LF: down a line, scrolling at the bottom margin */
static void down1(void)
{
	if (vt.y == vt.bot)
		scrollup(vt.top, vt.bot, 1);
	else if (vt.y < ROWS - 1)
		vt.y++;
}

static void up1(void)
{
	if (vt.y == vt.top)
		scrolldown(vt.top, vt.bot, 1);
	else if (vt.y > 0)
		vt.y--;
}

static void moveto(int x, int y)
{
	int lo = 0, hi = ROWS - 1;

	if (vt.origin) {
		y += vt.top;
		lo = vt.top;
		hi = vt.bot;
	}
	if (y < lo)
		y = lo;
	if (y > hi)
		y = hi;
	if (x < 0)
		x = 0;
	if (x >= COLS)
		x = COLS - 1;
	vt.x = x;
	vt.y = y;
	vt.wrap = 0;
}

static void savecur(void)
{
	Saved *s = &vt.save[vt.alt];
	int i;

	s->x = vt.x;
	s->y = vt.y;
	s->at = vt.at;
	s->fg = vt.fg;
	s->bg = vt.bg;
	s->wrap = vt.wrap;
	s->origin = vt.origin;
	s->gl = vt.gl;
	for (i = 0; i < 4; i++)
		s->g[i] = vt.g[i];
}

static void restcur(void)
{
	Saved *s = &vt.save[vt.alt];
	int i;

	vt.x = s->x;
	vt.y = s->y;
	vt.at = s->at;
	vt.fg = s->fg;
	vt.bg = s->bg;
	vt.wrap = s->wrap;
	vt.origin = s->origin;
	vt.gl = s->gl;
	for (i = 0; i < 4; i++)
		vt.g[i] = s->g[i];
	if (vt.x >= COLS)
		vt.x = COLS - 1;
	if (vt.y >= ROWS)
		vt.y = ROWS - 1;
}

static void tabreset(void)
{
	int i;

	for (i = 0; i < COLS; i++)
		vt.tabs[i] = i % 8 == 0 && i > 0;
}

static void softreset(void)
{
	vt.top = 0;
	vt.bot = ROWS - 1;
	vt.origin = 0;
	vt.awrap = 1;
	vt.insert = 0;
	vt.lnm = 0;
	vt.curon = 1;
	vt.appcur = 0;
	vt.appkey = 0;
	vt.at = 0;
	vt.fg = vt.bg = C_DEF;
	vt.g[0] = vt.g[1] = vt.g[2] = vt.g[3] = 'B';
	vt.gl = 0;
	vt.ss = 0;
	vt.wrap = 0;
	vt.save[0].x = vt.save[0].y = 0;
	vt.save[1] = vt.save[0];
}

void vt_reset(void)
{
	int y;

	vt.alt = 0;
	softreset();
	vt.rvscreen = 0;
	vt.curshape = 0;
	vt.mouse = vt.msgr = 0;
	vt.x = vt.y = 0;
	vt.st = S_GND;
	vt.uneed = 0;
	vt.last = ' ';
	for (y = 0; y < ROWS; y++) {
		blank(vt.buf[0][y], COLS);
		blank(vt.buf[1][y], COLS);
	}
	savecur();
	vt.save[1] = vt.save[0];
	tabreset();
	vt_dirtyall();
}

void vt_init(void)
{
	vt.charset = CS_AUTO;
	vt.cols = 80;
	vt.rows = 24;
	vt_reset();
}

static void clr(Cell *c, int n)
{
	while (n-- > 0) {
		c->ch = ' ';
		c->at = 0;
		c->fg = c->bg = C_DEF;
		c++;
	}
}

/* a new size. The cells stay where they are, but if the cursor would be
 * under the new last line, the lines go up so that it stays on it. The
 * scrolling region becomes the whole screen, as in xterm. */
void vt_resize(Vt *v, int c, int r)
{
	int s, y, d;

	c = c < MINCOLS ? MINCOLS : c > MAXCOLS ? MAXCOLS : c;
	r = r < MINROWS ? MINROWS : r > MAXROWS ? MAXROWS : r;
	d = v->y - (r - 1);
	if (d > 0) {
		for (y = 0; y + d < v->rows; y++)
			cpcells(v->buf[v->alt][y], v->buf[v->alt][y + d], MAXCOLS);
		for (; y < v->rows; y++)
			clr(v->buf[v->alt][y], MAXCOLS);
		v->y -= d;
	}
	for (s = 0; s < 2; s++)
		for (y = 0; y < r; y++) {
			if (y >= v->rows)
				clr(v->buf[s][y], c);
			else if (c > v->cols)
				clr(&v->buf[s][y][v->cols], c - v->cols);
			/* no wide letter cut in half at the right edge */
			if (v->buf[s][y][c - 1].ch && c < MAXCOLS &&
				v->buf[s][y][c].ch == 0 && vt_wide(v->buf[s][y][c - 1].ch))
				v->buf[s][y][c - 1].ch = ' ';
		}
	for (y = v->cols; y < c; y++)
		v->tabs[y] = y % 8 == 0;
	v->cols = c;
	v->rows = r;
	if (v->x >= c)
		v->x = c - 1;
	if (v->y >= r)
		v->y = r - 1;
	for (s = 0; s < 2; s++) {
		if (v->save[s].x >= c)
			v->save[s].x = c - 1;
		if (v->save[s].y >= r)
			v->save[s].y = r - 1;
	}
	v->top = 0;
	v->bot = r - 1;
	v->wrap = 0;
	for (y = 0; y < r; y++)
		v->dirty[y] = 1;
}

static int zerowidth(unsigned long c)
{
	return (c >= 0x300 && c <= 0x36f) || (c >= 0x483 && c <= 0x489) ||
		(c >= 0x591 && c <= 0x5bd) || (c >= 0x610 && c <= 0x61a) ||
		(c >= 0x64b && c <= 0x65f) || (c >= 0x1ab0 && c <= 0x1aff) ||
		(c >= 0x1dc0 && c <= 0x1dff) || (c >= 0x200b && c <= 0x200f) ||
		(c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x2064) ||
		(c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe00 && c <= 0xfe0f) ||
		(c >= 0xfe20 && c <= 0xfe2f) || c == 0xfeff ||
		(c >= 0xe0000 && c <= 0xe01ef);
}

static int wide(unsigned long c)
{
	return (c >= 0x1100 && c <= 0x115f) || (c >= 0x231a && c <= 0x231b) ||
		(c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) ||
		(c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
		(c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) ||
		(c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1f64f) ||
		(c >= 0x1f680 && c <= 0x1f6ff) || (c >= 0x1f900 && c <= 0x1f9ff) ||
		(c >= 0x1fa70 && c <= 0x1faff) || (c >= 0x20000 && c <= 0x3fffd);
}

int vt_wide(unsigned long c)
{
	return wide(c);
}

unsigned vt_c0(int b)
{
	return b >= 0 && b < 0x20 ? cp437c0[b] : 0;
}

unsigned vt_cp437(int b)
{
	return b < 0x80 ? (unsigned)b : (unsigned)cp437[b - 0x80];
}

/* a combining mark: on the letter before it, if there is such a letter */
static void combine(unsigned long m)
{
	int x = vt.wrap ? vt.x : vt.x - 1;
	Cell *c;
	unsigned n;

	if (x < 0 || m > 0xffff)
		return;
	c = &SCR[vt.y][x];
	if (c->ch == 0 && x > 0)
		c--;
	n = r_compose(c->ch, (unsigned)m);
	if (n) {
		c->ch = n;
		dirty(vt.y);
	}
}

static void put(unsigned long cp)
{
	Cell *c;
	int w = 1, set;

	if (zerowidth(cp)) {
		combine(cp);
		return;
	}
	if (cp < 0x80) {
		set = vt.ss ? vt.g[vt.ss] : vt.g[vt.gl];
		if (set == '0' && cp >= 0x5f && cp <= 0x7e)
			cp = decgr[cp - 0x5f];
		else if (set == 'A' && cp == '#')
			cp = 0xa3;
	}
	vt.ss = 0;
	if (wide(cp))
		w = 2;
	if (cp > 0xffff)
		cp = w == 2 ? 0x25a1 : 0xfffd;
	if (vt.wrap && vt.awrap) {
		vt.x = 0;
		down1();
	}
	vt.wrap = 0;
	if (w == 2 && vt.x == COLS - 1) {
		if (!vt.awrap)
			return;
		blank(&SCR[vt.y][vt.x], 1);
		vt.x = 0;
		down1();
	}
	c = &SCR[vt.y][vt.x];
	if (vt.insert)
		cpcells(c + w, c, COLS - vt.x - w);
	/* half of a wide letter is overwritten: the other half goes */
	if (c->ch == 0 && vt.x > 0)
		c[-1].ch = ' ';
	if (vt.x + w < COLS && c[w].ch == 0)
		c[w].ch = ' ';
	c->ch = (unsigned short)cp;
	c->at = vt.at;
	c->fg = vt.fg;
	c->bg = vt.bg;
	if (w == 2) {
		c[1] = c[0];
		c[1].ch = 0;
	}
	dirty(vt.y);
	vt.last = cp;
	if (vt.x + w >= COLS) {
		vt.x = COLS - 1;
		vt.wrap = 1;
	} else
		vt.x += w;
}

static void reply(char *s)
{
	int n = 0;

	while (s[n])
		n++;
	tn_reply(s, n);
}

static void sendnum(char *pre, int a, int b, char *post)
{
	char buf[40];
	char *p = buf, *q;
	int v, i, k, d;

	for (q = pre; *q; )
		*p++ = *q++;
	for (k = 0; k < 2; k++) {
		v = k ? b : a;
		if (v < 0)
			continue;
		if (k && a >= 0)
			*p++ = ';';
		for (d = 1; d * 10 <= v; d *= 10)
			;
		for (; d > 0; d /= 10) {
			i = v / d % 10;
			*p++ = '0' + i;
		}
	}
	for (q = post; *q; )
		*p++ = *q++;
	*p = 0;
	reply(buf);
}

static int nearest(int r, int g, int b)
{
	long best = 0x7fffffffL, d;
	int i, k = 0, dr, dg, db;

	for (i = 0; i < 16; i++) {
		dr = r - pal16[i][0];
		dg = g - pal16[i][1];
		db = b - pal16[i][2];
		d = (long)dr * dr * 3 + (long)dg * dg * 4 + (long)db * db * 2;
		if (d < best) {
			best = d;
			k = i;
		}
	}
	return k;
}

static int col256(int n)
{
	static int lev[6] = {0, 95, 135, 175, 215, 255};

	if (n < 16)
		return n < 0 ? C_DEF : n;
	if (n < 232) {
		n -= 16;
		return nearest(lev[n / 36], lev[n / 6 % 6], lev[n % 6]);
	}
	if (n < 256) {
		n = 8 + 10 * (n - 232);
		return nearest(n, n, n);
	}
	return C_DEF;
}

/* 38 and 48 (and 58): 5;n or 2;r;g;b, with ; or with : */
static int extcolor(int *i, int *col)
{
	int k = *i, colon = vt.sub & (1 << (k + 1));

	if (k + 1 >= vt.np)
		return 0;
	if (vt.par[k + 1] == 5 && k + 2 < vt.np) {
		*col = col256(vt.par[k + 2]);
		*i = k + 2;
		return 1;
	}
	if (vt.par[k + 1] == 2) {
		/* 38:2::r:g:b has a colour space id; 38;2;r;g;b has not */
		if (colon && k + 5 < vt.np && (vt.sub & (1 << (k + 5))))
			k++;
		if (k + 4 < vt.np) {
			*col = nearest(vt.par[k + 2], vt.par[k + 3],
				vt.par[k + 4]);
			*i = k + 4;
			return 1;
		}
	}
	*i = vt.np;
	return 0;
}

static void sgr(void)
{
	int i, p, c;

	if (vt.np == 0)
		vt.np = 1, vt.par[0] = 0;
	for (i = 0; i < vt.np; i++) {
		p = vt.par[i];
		switch (p) {
		case 0:
			vt.at = 0;
			vt.fg = vt.bg = C_DEF;
			break;
		case 1: vt.at |= AT_BOLD; break;
		case 2: vt.at |= AT_DIM; break;
		case 3: vt.at |= AT_ITAL; break;
		case 4:
			/* 4:0 is no underline */
			if (i + 1 < vt.np && (vt.sub & (1 << (i + 1)))) {
				if (vt.par[++i] == 0)
					vt.at &= ~AT_UNDER;
				else
					vt.at |= AT_UNDER;
				while (i + 1 < vt.np && (vt.sub & (1 << (i + 1))))
					i++;
			} else
				vt.at |= AT_UNDER;
			break;
		case 5: case 6: vt.at |= AT_BLINK; break;
		case 7: vt.at |= AT_REV; break;
		case 8: vt.at |= AT_INVIS; break;
		case 9: vt.at |= AT_STRIKE; break;
		case 21: vt.at |= AT_UNDER; break;
		case 22: vt.at &= ~(AT_BOLD | AT_DIM); break;
		case 23: vt.at &= ~AT_ITAL; break;
		case 24: vt.at &= ~AT_UNDER; break;
		case 25: vt.at &= ~AT_BLINK; break;
		case 27: vt.at &= ~AT_REV; break;
		case 28: vt.at &= ~AT_INVIS; break;
		case 29: vt.at &= ~AT_STRIKE; break;
		case 38:
			if (extcolor(&i, &c))
				vt.fg = c;
			break;
		case 39: vt.fg = C_DEF; break;
		case 48:
			if (extcolor(&i, &c))
				vt.bg = c;
			break;
		case 49: vt.bg = C_DEF; break;
		case 58:
			extcolor(&i, &c);
			break;
		default:
			if (p >= 30 && p <= 37)
				vt.fg = p - 30;
			else if (p >= 40 && p <= 47)
				vt.bg = p - 40;
			else if (p >= 90 && p <= 97)
				vt.fg = p - 90 + 8;
			else if (p >= 100 && p <= 107)
				vt.bg = p - 100 + 8;
			break;
		}
	}
}

static void altscreen(int on, int clear)
{
	int y;

	if (on == vt.alt)
		return;
	vt.alt = on;
	if (on && clear)
		for (y = 0; y < ROWS; y++)
			blank(vt.buf[1][y], COLS);
	vt_dirtyall();
}

static void decmode(int p, int on)
{
	switch (p) {
	case 1: vt.appcur = on; break;
	case 3:			/* 80/132 columns: we stay at 80 */
		scrollup(0, ROWS - 1, ROWS);
		vt.top = 0;
		vt.bot = ROWS - 1;
		moveto(0, 0);
		break;
	case 5:
		vt.rvscreen = on;
		vt_dirtyall();
		break;
	case 6:
		vt.origin = on;
		moveto(0, 0);
		break;
	case 7: vt.awrap = on; break;
	case 25: vt.curon = on; break;
	case 47:
		altscreen(on, 0);
		break;
	case 1047:
		if (!on && vt.alt) {
			int y;

			for (y = 0; y < ROWS; y++)
				blank(vt.buf[1][y], COLS);
		}
		altscreen(on, 0);
		break;
	case 1048:
		if (on)
			savecur();
		else
			restcur();
		break;
	case 1049:
		if (on) {
			savecur();
			altscreen(1, 1);
			/* the alternate screen starts with the same cursor */
			vt.save[1] = vt.save[0];
		} else {
			altscreen(0, 0);
			restcur();
		}
		break;
	case 9: case 1000: case 1002: case 1003:
		vt.mouse = on ? p : 0;
		break;
	case 1006:
		vt.msgr = on;
		break;
	default:
		break;
	}
}

static void erase(int x0, int y0, int x1, int y1)
{
	int y;

	for (y = y0; y <= y1; y++) {
		if (y == y0 && y == y1)
			blank(&SCR[y][x0], x1 - x0 + 1);
		else if (y == y0)
			blank(&SCR[y][x0], COLS - x0);
		else if (y == y1)
			blank(&SCR[y][0], x1 + 1);
		else
			blank(SCR[y], COLS);
		dirty(y);
	}
}

static void csi(int f)
{
	int a = vt.np > 0 ? vt.par[0] : 0;
	int b = vt.np > 1 ? vt.par[1] : 0;
	int n = a > 0 ? a : 1;
	int i, x;
	Cell *c;

	if (vt.priv == '?') {
		switch (f) {
		case 'h':
		case 'l':
			for (i = 0; i < vt.np; i++)
				decmode(vt.par[i], f == 'h');
			break;
		case 'n':
			if (a == 6)
				sendnum("\033[?", vt.y - (vt.origin ? vt.top : 0) + 1,
					vt.x + 1, "R");
			else if (a == 15)
				reply("\033[?13n");
			else if (a == 25)
				reply("\033[?21n");
			else if (a == 26)
				reply("\033[?27;1n");
			break;
		case 'J':
			f = 'J';
			vt.priv = 0;
			csi(f);
			break;
		case 'K':
			vt.priv = 0;
			csi('K');
			break;
		}
		return;
	}
	if (vt.priv == '>') {
		if (f == 'c')
			reply("\033[>1;10;0c");
		return;
	}
	if (vt.priv)
		return;
	if (vt.inter) {
		if (vt.inter == '!' && f == 'p') {		/* DECSTR */
			softreset();
			vt_dirtyall();
		} else if (vt.inter == ' ' && f == 'q')		/* DECSCUSR */
			vt.curshape = a >= 3;
		return;
	}
	switch (f) {
	case '@':
		if (n > COLS - vt.x)
			n = COLS - vt.x;
		c = SCR[vt.y];
		cpcells(c + vt.x + n, c + vt.x, COLS - vt.x - n);
		blank(c + vt.x, n);
		dirty(vt.y);
		vt.wrap = 0;
		break;
	case 'A':
		i = vt.y >= vt.top ? vt.top : 0;
		vt.y = vt.y - n < i ? i : vt.y - n;
		vt.wrap = 0;
		break;
	case 'B':
	case 'e':
		i = vt.y <= vt.bot ? vt.bot : ROWS - 1;
		vt.y = vt.y + n > i ? i : vt.y + n;
		vt.wrap = 0;
		break;
	case 'C':
	case 'a':
		vt.x = vt.x + n >= COLS ? COLS - 1 : vt.x + n;
		vt.wrap = 0;
		break;
	case 'D':
		vt.x = vt.x - n < 0 ? 0 : vt.x - n;
		vt.wrap = 0;
		break;
	case 'E':
		i = vt.y <= vt.bot ? vt.bot : ROWS - 1;
		vt.y = vt.y + n > i ? i : vt.y + n;
		vt.x = 0;
		vt.wrap = 0;
		break;
	case 'F':
		i = vt.y >= vt.top ? vt.top : 0;
		vt.y = vt.y - n < i ? i : vt.y - n;
		vt.x = 0;
		vt.wrap = 0;
		break;
	case 'G':
	case '`':
		moveto(n - 1, vt.y - (vt.origin ? vt.top : 0));
		break;
	case 'H':
	case 'f':
		moveto((b > 0 ? b : 1) - 1, n - 1);
		break;
	case 'I':
		while (n-- > 0) {
			for (x = vt.x + 1; x < COLS - 1 && !vt.tabs[x]; x++)
				;
			vt.x = x < COLS ? x : COLS - 1;
		}
		vt.wrap = 0;
		break;
	case 'Z':
		while (n-- > 0) {
			for (x = vt.x - 1; x > 0 && !vt.tabs[x]; x--)
				;
			vt.x = x < 0 ? 0 : x;
		}
		vt.wrap = 0;
		break;
	case 'J':
		if (a == 0)
			erase(vt.x, vt.y, COLS - 1, ROWS - 1);
		else if (a == 1)
			erase(0, 0, vt.x, vt.y);
		else if (a == 2)
			erase(0, 0, COLS - 1, ROWS - 1);
		break;
	case 'K':
		if (a == 0)
			erase(vt.x, vt.y, COLS - 1, vt.y);
		else if (a == 1)
			erase(0, vt.y, vt.x, vt.y);
		else if (a == 2)
			erase(0, vt.y, COLS - 1, vt.y);
		break;
	case 'L':
		if (vt.y >= vt.top && vt.y <= vt.bot) {
			scrolldown(vt.y, vt.bot, n);
			vt.x = 0;
			vt.wrap = 0;
		}
		break;
	case 'M':
		if (vt.y >= vt.top && vt.y <= vt.bot) {
			scrollup(vt.y, vt.bot, n);
			vt.x = 0;
			vt.wrap = 0;
		}
		break;
	case 'P':
		if (n > COLS - vt.x)
			n = COLS - vt.x;
		c = SCR[vt.y];
		cpcells(c + vt.x, c + vt.x + n, COLS - vt.x - n);
		blank(c + COLS - n, n);
		dirty(vt.y);
		vt.wrap = 0;
		break;
	case 'S':
		if (vt.np <= 1)
			scrollup(vt.top, vt.bot, n);
		break;
	case 'T':
		if (vt.np <= 1)
			scrolldown(vt.top, vt.bot, n);
		break;
	case 'X':
		if (n > COLS - vt.x)
			n = COLS - vt.x;
		blank(&SCR[vt.y][vt.x], n);
		dirty(vt.y);
		vt.wrap = 0;
		break;
	case 'b':
		if (n > COLS * ROWS)
			n = COLS * ROWS;
		while (n-- > 0)
			put(vt.last);
		break;
	case 'c':
		if (a == 0)
			reply("\033[?1;2c");
		break;
	case 'd':
		moveto(vt.x, n - 1);
		break;
	case 'g':
		if (a == 0)
			vt.tabs[vt.x] = 0;
		else if (a == 3)
			for (i = 0; i < COLS; i++)
				vt.tabs[i] = 0;
		break;
	case 'h':
	case 'l':
		for (i = 0; i < vt.np; i++)
			if (vt.par[i] == 4)
				vt.insert = f == 'h';
			else if (vt.par[i] == 20)
				vt.lnm = f == 'h';
		break;
	case 'm':
		sgr();
		break;
	case 'n':
		if (a == 5)
			reply("\033[0n");
		else if (a == 6)
			sendnum("\033[", vt.y - (vt.origin ? vt.top : 0) + 1,
				vt.x + 1, "R");
		break;
	case 'r':
		a = a > 0 ? a : 1;
		b = b > 0 && b <= ROWS ? b : ROWS;
		if (a < b) {
			vt.top = a - 1;
			vt.bot = b - 1;
			moveto(0, 0);
		}
		break;
	case 's':
		if (vt.np == 0)
			savecur();
		break;
	case 'u':
		restcur();
		break;
	case 't':
		if (a == 18)
			sendnum("\033[8;", ROWS, COLS, "t");
		else if (a == 19)
			sendnum("\033[9;", ROWS, COLS, "t");
		else if (a == 14)
			sendnum("\033[4;", ROWS * CH, COLS * CW, "t");
		else if (a == 11)
			reply("\033[1t");
		break;
	default:
		break;
	}
}

static void esc(int f)
{
	if (vt.inter) {
		switch (vt.inter) {
		case '(': vt.g[0] = f; break;
		case ')': vt.g[1] = f; break;
		case '*': vt.g[2] = f; break;
		case '+': vt.g[3] = f; break;
		case '#':
			if (f == '8') {		/* DECALN: fill with E */
				int x, y;

				for (y = 0; y < ROWS; y++)
					for (x = 0; x < COLS; x++) {
						SCR[y][x].ch = 'E';
						SCR[y][x].at = 0;
						SCR[y][x].fg = SCR[y][x].bg = C_DEF;
					}
				vt_dirtyall();
			}
			break;
		case '%':
			if (f == 'G')
				vt.charset = CS_UTF8;
			else if (f == '@')
				vt.charset = CS_AUTO;
			break;
		}
		return;
	}
	switch (f) {
	case '7': savecur(); break;
	case '8': restcur(); break;
	case 'D': down1(); vt.wrap = 0; break;
	case 'E': vt.x = 0; down1(); vt.wrap = 0; break;
	case 'H': vt.tabs[vt.x] = 1; break;
	case 'M': up1(); vt.wrap = 0; break;
	case 'N': vt.ss = 2; break;
	case 'O': vt.ss = 3; break;
	case 'Z': reply("\033[?1;2c"); break;
	case 'c': vt_reset(); tn_title(""); break;
	case '=': vt.appkey = 1; break;
	case '>': vt.appkey = 0; break;
	default: break;
	}
}

static void strend(void)
{
	char *s = vt.str;

	vt.str[vt.ns] = 0;
	if (vt.strk != ']')
		return;
	if ((s[0] == '0' || s[0] == '2') && s[1] == ';')
		tn_title(s + 2);
}

static void ctrl(int c)
{
	switch (c) {
	case 7:
		tn_bell();
		break;
	case 8:
		if (vt.x > 0)
			vt.x--;
		vt.wrap = 0;
		break;
	case 9:
		{
			int x;

			for (x = vt.x + 1; x < COLS - 1 && !vt.tabs[x]; x++)
				;
			vt.x = x < COLS ? x : COLS - 1;
		}
		break;
	case 10:
	case 11:
	case 12:
		down1();
		if (vt.lnm)
			vt.x = 0;
		vt.wrap = 0;
		break;
	case 13:
		vt.x = 0;
		vt.wrap = 0;
		break;
	case 14:
		vt.gl = 1;
		break;
	case 15:
		vt.gl = 0;
		break;
	default:
		break;
	}
}

/* a letter that is not UTF-8: CP437 or Latin-1 */
static unsigned long eight(int b)
{
	if (vt.charset == CS_LATIN1)
		return b;
	return cp437[b - 0x80];
}

/* auto: many bytes that are not UTF-8, few that are: the host sends
 * CP437 (a BBS), and from now on all bytes above 127 are CP437 */
static void badbytes(int n)
{
	vt.bad += n;
	if (vt.charset == CS_AUTO && vt.bad >= 4 && vt.bad > 2 * vt.good)
		vt.is437 = 1;
}

/* the bytes of an unfinished UTF-8 letter, taken one by one */
static void flushutf(void)
{
	int i, n = vt.un;

	vt.uneed = vt.un = 0;
	for (i = 0; i < n; i++)
		put(vt.charset == CS_UTF8 ? 0xfffd : eight(vt.ub[i]));
	badbytes(n);
}

static void text(int b)
{
	if (b < 0x80) {
		if (vt.uneed)
			flushutf();
		put(b);
		return;
	}
	if (vt.charset == CS_LATIN1 || vt.charset == CS_CP437 ||
		(vt.is437 && vt.charset == CS_AUTO)) {
		put(eight(b));
		return;
	}
	if (vt.uneed) {
		if ((b & 0xc0) == 0x80) {
			vt.ub[vt.un++] = b;
			vt.ucp = vt.ucp << 6 | (b & 0x3f);
			if (--vt.uneed == 0) {
				/* too long a form, or a surrogate: not UTF-8 */
				if ((vt.un == 3 && vt.ucp < 0x800) ||
					(vt.un == 4 && (vt.ucp < 0x10000 ||
					vt.ucp > 0x10ffff)) ||
					(vt.ucp >= 0xd800 && vt.ucp <= 0xdfff)) {
					vt.uneed = 1;
					flushutf();
				} else {
					vt.un = 0;
					vt.good++;
					put(vt.ucp);
				}
			}
			return;
		}
		flushutf();
		if (vt.is437 && vt.charset == CS_AUTO) {
			put(eight(b));	/* it found the host sends CP437 */
			return;
		}
	}
	if (b >= 0xc2 && b <= 0xdf) {
		vt.uneed = 1;
		vt.ucp = b & 0x1f;
	} else if (b >= 0xe0 && b <= 0xef) {
		vt.uneed = 2;
		vt.ucp = b & 0x0f;
	} else if (b >= 0xf0 && b <= 0xf4) {
		vt.uneed = 3;
		vt.ucp = b & 0x07;
	} else {
		put(vt.charset == CS_UTF8 ? 0xfffd : eight(b));
		badbytes(1);
		return;
	}
	vt.un = 1;
	vt.ub[0] = b;
}

void vt_write(unsigned char *s, int n)
{
	int b;

	while (n-- > 0) {
		b = *s++;
		/* a CP437 host: most control codes are pictures */
		if (b < 0x20 && vt.st == S_GND && cp437c0[b] &&
			(vt.charset == CS_CP437 || (vt.is437 &&
			vt.charset == CS_AUTO))) {
			put(cp437c0[b]);
			continue;
		}
		/* C0 controls work in the middle of a sequence too */
		if (b < 0x20 && vt.st != S_STR && vt.st != S_STRESC) {
			if (b == 033) {
				if (vt.uneed)
					flushutf();
				vt.st = S_ESC;
				vt.inter = 0;
			} else if (b == 030 || b == 032)
				vt.st = S_GND;
			else {
				if (vt.uneed)
					flushutf();
				ctrl(b);
			}
			continue;
		}
		switch (vt.st) {
		case S_GND:
			if (b != 0x7f)
				text(b);
			break;
		case S_ESC:
			if (b >= 0x20 && b <= 0x2f) {
				vt.inter = b;
				vt.st = S_ESCI;
			} else if (b == '[') {
				vt.st = S_CSI;
				vt.np = 0;
				vt.sub = 0;
				vt.par[0] = 0;
				vt.priv = 0;
				vt.inter = 0;
			} else if (b == ']' || b == 'P' || b == 'X' ||
				b == '^' || b == '_') {
				vt.st = S_STR;
				vt.strk = b;
				vt.ns = 0;
			} else {
				vt.st = S_GND;
				if (b >= 0x30 && b <= 0x7e)
					esc(b);
			}
			break;
		case S_ESCI:
			if (b >= 0x20 && b <= 0x2f)
				vt.inter = b;
			else {
				vt.st = S_GND;
				if (b >= 0x30 && b <= 0x7e)
					esc(b);
			}
			break;
		case S_CSI:
			if (b >= '0' && b <= '9') {
				if (vt.np == 0)
					vt.np = 1;
				if (vt.np <= 16 && vt.par[vt.np - 1] < 10000)
					vt.par[vt.np - 1] = vt.par[vt.np - 1] * 10 +
						b - '0';
			} else if (b == ';' || b == ':') {
				if (vt.np == 0)
					vt.np = 1;
				if (vt.np < 16) {
					if (b == ':')
						vt.sub |= 1 << vt.np;
					vt.par[vt.np++] = 0;
				}
			} else if (b >= '<' && b <= '?') {
				vt.priv = b;
			} else if (b >= 0x20 && b <= 0x2f) {
				vt.inter = b;
			} else {
				vt.st = S_GND;
				if (vt.np > 16)
					vt.np = 16;
				if (b >= 0x40 && b <= 0x7e)
					csi(b);
				else if (b >= 0x80)
					text(b);
			}
			break;
		case S_STR:
			if (b == 7 && vt.strk == ']') {
				strend();
				vt.st = S_GND;
			} else if (b == 033)
				vt.st = S_STRESC;
			else if (b == 0x9c) {
				strend();
				vt.st = S_GND;
			} else if (b >= 0x20 && vt.ns < (int)sizeof vt.str - 1)
				vt.str[vt.ns++] = b;
			break;
		case S_STRESC:
			if (b == '\\') {
				strend();
				vt.st = S_GND;
			} else if (b == 033)
				vt.st = S_STRESC;
			else {
				/* not ST: the string ends and this is a new ESC */
				strend();
				vt.st = S_ESC;
				vt.inter = 0;
				s--;
				n++;
			}
			break;
		}
	}
}

/* text from the program itself: the command prompt, messages */
void vt_puts(char *s)
{
	int n = 0;

	while (s[n])
		n++;
	vt_write((unsigned char *)s, n);
}
