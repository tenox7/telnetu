/* glyph.c - draws the terminal's cells into a bitmap of the window: 80 x 24
 * cells of 9 x 12 pixels. In the bitmap 1 is a lit pixel and the low bit
 * of each 16-bit word is the leftmost one, as the 3B1's wrastop() takes it.
 *
 * Letters 0x20-0x7e come from the system font, /usr/lib/wfont/system.8.ft,
 * so the text looks as in any other window. The rest is drawn here: box
 * lines (single, heavy, double), blocks and shades, braille dots (MapSCII
 * draws maps with them), accented letters (the font's letter and an accent)
 * and a set of symbols. A letter it has no picture for is shown as
 * something close (a quote for a curly quote) or a '?'.
 *
 * One bit of colour: a cell's background is lit when its background colour
 * is light (or for reverse video), and its letter always shows against it.
 */
#include <fcntl.h>
#ifdef __APPLE__
#include <unistd.h>
#endif
#include "tn.h"
#include "syms.h"

unsigned short bm[PXH][WPR];

static unsigned short font[96][CH];	/* the system font: 9-bit rows */
static Cell shadow[ROWS][COLS];		/* what the bitmap shows */
static int curx = -1, cury = -1;	/* the cursor as drawn */

/* brightness of the 16 colours, 0-15; at 8 and above a background is lit */
static unsigned char lum[16] = {
	0, 4, 7, 11, 2, 5, 9, 14, 7, 5, 9, 14, 7, 6, 11, 15
};

/* ---- the system font ---- */

static int rd16(unsigned char *p)
{
	return p[0] << 8 | p[1];
}

int r_init(char *path)
{
	static unsigned char f[4096];
	unsigned char *c;
	int fd, n, i, r, w, hs, vs, ha, va, mr, x, y, base, rows;
	unsigned long bits;

	fd = open(path, 0);
	if (fd < 0)
		return -1;
	n = read(fd, (char *)f, sizeof f);
	close(fd);
	if (n < 32 + 96 * 8 || rd16(f + 2) != 0616)
		return -1;
	base = (signed char)f[7];
	for (i = 0; i < 96; i++) {
		for (y = 0; y < CH; y++)
			font[i][y] = 0;
		c = f + 32 + i * 8;
		hs = (signed char)c[0];
		vs = (signed char)c[1];
		ha = (signed char)c[2];
		va = (signed char)c[3];
		mr = (short)rd16(c + 6);
		if (hs <= 0 || vs <= 0)
			continue;
		w = (hs + 15) / 16;
		for (r = 0; r < vs; r++) {
			bits = 0;
			for (x = 0; x < w && x < 2; x++) {
				n = 32 + i * 8 + 6 + mr + (r * w + x) * 2;
				if (n + 1 < (int)sizeof f)
					bits |= (unsigned long)rd16(f + n) << (16 * x);
			}
			y = base + va + r;
			if (y < 0 || y >= CH)
				continue;
			rows = ha >= 0 ? (int)(bits << ha) : (int)(bits >> -ha);
			font[i][y] = rows & 0x1ff;
		}
	}
	return 0;
}

/* ---- box drawing: U+2500-257F ----
 * Each letter is its four arms: left, right, up, down; 0 none, 1 light,
 * 2 heavy, 3 double. Dashes and diagonals are special (d, x). */
static char *boxes =
	"1100" "2200" "0011" "0022" "d1h3" "d2h3" "d1v3" "d2v3"
	"d1h4" "d2h4" "d1v4" "d2v4" "0101" "0201" "0102" "0202"
	"1001" "2001" "1002" "2002" "0110" "0210" "0120" "0220"
	"1010" "2010" "1020" "2020" "0111" "0211" "0121" "0112"
	"0122" "0221" "0212" "0222" "1011" "2011" "1021" "1012"
	"1022" "2021" "2012" "2022" "1101" "2101" "1201" "2201"
	"1102" "2102" "1202" "2202" "1110" "2110" "1210" "2210"
	"1120" "2120" "1220" "2220" "1111" "2111" "1211" "2211"
	"1121" "1112" "1122" "2121" "1221" "2112" "1212" "2221"
	"2212" "2122" "1222" "2222" "d1h2" "d2h2" "d1v2" "d2v2"
	"3300" "0033" "0301" "0103" "0303" "3001" "1003" "3003"
	"0310" "0130" "0330" "3010" "1030" "3030" "0311" "0133"
	"0333" "3011" "1033" "3033" "3301" "1103" "3303" "3310"
	"1130" "3330" "3311" "1133" "3333" "0101" "1001" "1010"
	"0110" "x1.." "x2.." "x3.." "1000" "0010" "0100" "0001"
	"2000" "0020" "0200" "0002" "1200" "0012" "2100" "0021";

#define X1 2		/* double lines: columns 2-3 and 6-7 */
#define X2 6
#define Y1 4		/* rows 4 and 7 */
#define Y2 7
#define YC 5		/* a single horizontal line: row 5 (6 too if heavy) */

static void hline(unsigned short *g, int y, int x0, int x1)
{
	int x;

	for (x = x0; x <= x1; x++)
		if (x >= 0 && x < CW && y >= 0 && y < CH)
			g[y] |= 1 << x;
}

static void vline(unsigned short *g, int x, int y0, int y1)
{
	int y;

	for (y = y0; y <= y1; y++)
		if (x >= 0 && x < CW && y >= 0 && y < CH)
			g[y] |= 1 << x;
}

static void box(unsigned short *g, char *d)
{
	int L = d[0] - '0', R = d[1] - '0', U = d[2] - '0', D = d[3] - '0';
	int vh = U == 2 || D == 2, hh = L == 2 || R == 2;
	int vlo = vh ? 3 : 4, vhi = vh ? 6 : 5;	/* columns of a single | */
	int hlo = YC, hhi = hh ? YC + 1 : YC;	/* rows of a single - */
	int e, e2, t;

	/* horizontal arms */
	if (L == 3) {
		e = U == 3 ? X1 + 1 : D == 3 ? X2 + 1 : vhi;
		e2 = D == 3 ? X1 + 1 : U == 3 ? X2 + 1 : vhi;
		hline(g, Y1, 0, e);
		hline(g, Y2, 0, e2);
	} else if (L) {
		e = U == 3 && D == 3 && !R ? X1 + 1 : U == 3 || D == 3 ?
			X2 + 1 : vhi;
		for (t = YC; t <= (L == 2 ? YC + 1 : YC); t++)
			hline(g, t, 0, e);
	}
	if (R == 3) {
		e = U == 3 ? X2 : D == 3 ? X1 : vlo;
		e2 = D == 3 ? X2 : U == 3 ? X1 : vlo;
		hline(g, Y1, e, CW - 1);
		hline(g, Y2, e2, CW - 1);
	} else if (R) {
		e = U == 3 && D == 3 && !L ? X2 : U == 3 || D == 3 ? X1 : vlo;
		for (t = YC; t <= (R == 2 ? YC + 1 : YC); t++)
			hline(g, t, e, CW - 1);
	}
	/* vertical arms */
	if (U == 3) {
		e = L == 3 ? Y1 : R == 3 ? Y2 : hhi;
		e2 = R == 3 ? Y1 : L == 3 ? Y2 : hhi;
		vline(g, X1, 0, e);
		vline(g, X1 + 1, 0, e);
		vline(g, X2, 0, e2);
		vline(g, X2 + 1, 0, e2);
	} else if (U) {
		e = L == 3 && R == 3 && !D ? Y1 : L == 3 || R == 3 ? Y2 : hhi;
		for (t = U == 2 ? 3 : 4; t <= (U == 2 ? 6 : 5); t++)
			vline(g, t, 0, e);
	}
	if (D == 3) {
		e = L == 3 ? Y2 : R == 3 ? Y1 : hlo;
		e2 = R == 3 ? Y2 : L == 3 ? Y1 : hlo;
		vline(g, X1, e, CH - 1);
		vline(g, X1 + 1, e, CH - 1);
		vline(g, X2, e2, CH - 1);
		vline(g, X2 + 1, e2, CH - 1);
	} else if (D) {
		e = L == 3 && R == 3 && !U ? Y2 : L == 3 || R == 3 ? Y1 : hlo;
		for (t = D == 2 ? 3 : 4; t <= (D == 2 ? 6 : 5); t++)
			vline(g, t, e, CH - 1);
	}
}

/* dashed lines: d, weight, h or v, number of dashes */
static void dash(unsigned short *g, char *d)
{
	int heavy = d[1] == '2', n = d[3] - '0', i, t;

	if (d[2] == 'h') {
		for (i = 0; i < CW; i++)
			if (i % (n == 2 ? 5 : n == 3 ? 3 : 2) !=
				(n == 2 ? 4 : n == 3 ? 2 : 1))
				for (t = YC; t <= YC + heavy; t++)
					g[t] |= 1 << i;
	} else {
		for (i = 0; i < CH; i++)
			if (i % (12 / n) != 12 / n - 1)
				for (t = heavy ? 3 : 4; t <= (heavy ? 6 : 5); t++)
					g[i] |= 1 << t;
	}
}

static void diag(unsigned short *g, int which)
{
	int y, x;

	for (y = 0; y < CH; y++) {
		x = (CH - 1 - y) * CW / CH;
		if (which & 1)
			g[y] |= 3 << x >> 1 | 1 << x;
		x = y * CW / CH;
		if (which & 2)
			g[y] |= 3 << x >> 1 | 1 << x;
	}
	for (y = 0; y < CH; y++)
		g[y] &= 0x1ff;
}

/* ---- blocks: U+2580-259F ---- */
static void rect(unsigned short *g, int x0, int y0, int x1, int y1)
{
	int y;

	for (y = y0; y <= y1; y++)
		hline(g, y, x0, x1);
}

/* shades line up from cell to cell: odd columns start one pixel later */
static void shade(unsigned short *g, int k, int odd)
{
	int x, y, on, px;

	for (y = 0; y < CH; y++)
		for (x = 0; x < CW; x++) {
			px = x + odd;
			if (k == 1)
				on = (px & 1) == 0 && (y & 1) == 0;
			else if (k == 2)
				on = ((px + y) & 1) == 0;
			else
				on = !((px & 1) && (y & 1));
			if (on)
				g[y] |= 1 << x;
		}
}

static void block(unsigned short *g, int c, int odd)
{
	int n;

	if (c == 0x80) {
		rect(g, 0, 0, 8, 5);
	} else if (c >= 0x81 && c <= 0x88) {
		n = (c - 0x80) * CH / 8;
		rect(g, 0, CH - n, 8, 11);
	} else if (c >= 0x89 && c <= 0x8f) {
		n = (0x90 - c) * CW / 8;
		rect(g, 0, 0, n - 1, 11);
	} else if (c == 0x90) {
		rect(g, 4, 0, 8, 11);
	} else if (c >= 0x91 && c <= 0x93) {
		shade(g, c - 0x90, odd);
	} else if (c == 0x94) {
		rect(g, 0, 0, 8, 1);
	} else if (c == 0x95) {
		rect(g, 8, 0, 8, 11);
	} else {
		/* quadrants: bits upper left 1, upper right 2, lower left 4,
		 * lower right 8 */
		static unsigned char q[10] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};

		n = q[c - 0x96];
		if (n & 1)
			rect(g, 0, 0, 4, 5);
		if (n & 2)
			rect(g, 5, 0, 8, 5);
		if (n & 4)
			rect(g, 0, 6, 4, 11);
		if (n & 8)
			rect(g, 5, 6, 8, 11);
	}
}

/* braille: 2 x 4 dots of 2 x 2 pixels */
static void braille(unsigned short *g, int b)
{
	static unsigned char bit[8][2] = {
		{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {0, 3}, {1, 3}
	};
	int i, x, y;

	for (i = 0; i < 8; i++)
		if (b & 1 << i) {
			x = bit[i][0] ? 5 : 1;
			y = bit[i][1] * 3;
			g[y] |= 3 << x;
			g[y + 1] |= 3 << x;
		}
}

/* ---- accents: the font's letter and a mark above or below ---- */

/* U+00C0-00FF: the letter and its mark: ` grave, ' acute, ^ circumflex,
 * ~ tilde, : diaeresis, o ring, , cedilla, / stroke, - bar, e and E half
 * an E (ae, oe), * a symbol of syms.h, space nothing */
static char latin1[] =
	"A`A'A^A~A:AoAEC,E`E'E^E:I`I'I^I:D-N~O`O'O^O~O:**O/U`U'U^U:Y'P "
	"**a`a'a^a~a:aoaec,e`e'e^e:i`i'i^i:d-n~o`o'o^o~o:**o/u`u'u^u:y'p y:";

/* U+0100-017F: some of Latin Extended-A, Polish and Czech among them */
static struct { unsigned short cp; char base, mark; } latx[] = {
	{0x100, 'A', '='}, {0x101, 'a', '='}, {0x102, 'A', 'u'},
	{0x103, 'a', 'u'}, {0x104, 'A', ';'}, {0x105, 'a', ';'},
	{0x106, 'C', '\''}, {0x107, 'c', '\''}, {0x10c, 'C', 'v'},
	{0x10d, 'c', 'v'}, {0x10e, 'D', 'v'}, {0x10f, 'd', 'v'},
	{0x110, 'D', '-'}, {0x111, 'd', '-'}, {0x112, 'E', '='},
	{0x113, 'e', '='}, {0x116, 'E', '.'}, {0x117, 'e', '.'},
	{0x118, 'E', ';'}, {0x119, 'e', ';'}, {0x11a, 'E', 'v'},
	{0x11b, 'e', 'v'}, {0x11e, 'G', 'u'}, {0x11f, 'g', 'u'},
	{0x12a, 'I', '='}, {0x12b, 'i', '='}, {0x130, 'I', '.'},
	{0x131, 'i', ' '}, {0x141, 'L', '/'}, {0x142, 'l', '/'},
	{0x143, 'N', '\''}, {0x144, 'n', '\''}, {0x147, 'N', 'v'},
	{0x148, 'n', 'v'}, {0x14c, 'O', '='}, {0x14d, 'o', '='},
	{0x150, 'O', '"'}, {0x151, 'o', '"'}, {0x152, 'O', 'E'},
	{0x153, 'o', 'e'}, {0x158, 'R', 'v'}, {0x159, 'r', 'v'},
	{0x15a, 'S', '\''}, {0x15b, 's', '\''}, {0x15e, 'S', ','},
	{0x15f, 's', ','}, {0x160, 'S', 'v'}, {0x161, 's', 'v'},
	{0x164, 'T', 'v'}, {0x165, 't', 'v'}, {0x16a, 'U', '='},
	{0x16b, 'u', '='}, {0x16e, 'U', 'o'}, {0x16f, 'u', 'o'},
	{0x170, 'U', '"'}, {0x171, 'u', '"'}, {0x178, 'Y', ':'},
	{0x179, 'Z', '\''}, {0x17a, 'z', '\''}, {0x17b, 'Z', '.'},
	{0x17c, 'z', '.'}, {0x17d, 'Z', 'v'}, {0x17e, 'z', 'v'},
	{0x10a, 'C', '.'}, {0x10b, 'c', '.'}, {0x120, 'G', '.'},
	{0x121, 'g', '.'}, {0x139, 'L', '\''}, {0x13a, 'l', '\''},
	{0x13d, 'L', 'v'}, {0x13e, 'l', 'v'}, {0x154, 'R', '\''},
	{0x155, 'r', '\''}, {0x16c, 'U', 'u'}, {0x16d, 'u', 'u'},
	{0x15c, 'S', '^'}, {0x15d, 's', '^'}, {0x108, 'C', '^'},
	{0x109, 'c', '^'}, {0x11c, 'G', '^'}, {0x11d, 'g', '^'},
	{0x124, 'H', '^'}, {0x125, 'h', '^'}, {0x134, 'J', '^'},
	{0x135, 'j', '^'}, {0x174, 'W', '^'}, {0x175, 'w', '^'},
	{0x176, 'Y', '^'}, {0x177, 'y', '^'}, {0x128, 'I', '~'},
	{0x129, 'i', '~'}, {0x168, 'U', '~'}, {0x169, 'u', '~'},
	/* Greek with tonos, as Latin letters */
	{0x386, 'A', '\''}, {0x388, 'E', '\''}, {0x389, 'H', '\''},
	{0x38a, 'I', '\''}, {0x38c, 'O', '\''}, {0x38e, 'Y', '\''},
	{0x38f, 'W', '\''}, {0x3aa, 'I', ':'}, {0x3ab, 'Y', ':'},
	{0x3ac, 'a', '\''}, {0x3ad, 'e', '\''}, {0x3ae, 'h', '\''},
	{0x3af, 'i', '\''}, {0x3ca, 'i', ':'}, {0x3cb, 'y', ':'},
	{0x3cc, 'o', '\''}, {0x3cd, 'y', '\''}, {0x3ce, 'w', '\''},
	{0x390, 'i', ':'}, {0x3b0, 'y', ':'},
	/* Cyrillic yo */
	{0x401, 'E', ':'}, {0x451, 'e', ':'},
	{0, 0, 0}
};

/* a capital one row shorter: a row like the next one goes (else row 5),
 * so that a mark of two rows fits over it */
static void squeeze(unsigned short *g)
{
	int r, y;

	for (r = 2; r < 9 && g[r] != g[r + 1]; r++)
		;
	if (r == 9)
		r = 5;
	for (y = r; y > 1; y--)
		g[y] = g[y - 1];
	g[0] = g[1] = 0;
}

static void letter(unsigned short *g, int base, int m)
{
	int y, i, cap = base >= 'A' && base <= 'Z';

	for (y = 0; y < CH; y++)
		g[y] = font[base - 32][y];
	if (base == 'i' && m != '.' && m != ' ')	/* the dot goes */
		g[1] = g[2] = 0;
	switch (m) {
	case ' ':
		break;
	case ',':				/* cedilla */
		g[10] |= 0x030;
		g[11] |= 0x018;
		break;
	case ';':				/* ogonek */
		g[10] |= 0x060;
		g[11] |= 0x0c0;
		break;
	case '/':
		if (base == 'o' || base == 'O')
			for (y = cap ? 1 : 3; y <= 10; y++)
				g[y] |= 1 << (cap ? 8 - (y - 1) * 8 / 9 : 8 - (y - 3));
		else if (base == 'l') {
			g[4] |= 0x0c0;
			g[6] |= 0x00c;
		} else {
			g[4] |= 0x030;
			g[6] |= 0x003;
		}
		break;
	case '-':
		if (base == 'D')
			g[5] |= 0x01f;
		else
			g[2] |= 0x1f0;
		break;
	case 'E':
	case 'e':				/* ae oe: half and half */
		for (y = 0; y < CH; y++)
			g[y] = (g[y] & 0x01f) | (font[m - 32][y] & 0x1f0);
		break;
	default:
		for (i = 0; marks[i].m; i++)
			if (marks[i].m == m) {
				if (cap) {
					squeeze(g);
					g[0] |= marks[i].rows[0];
					g[1] |= marks[i].rows[1];
				} else
					for (y = 0; y < 3; y++)
						g[y + 1] |= marks[i].rows[y];
			}
		break;
	}
	for (y = 0; y < CH; y++)
		g[y] &= 0x1ff;
}

/* a letter and a combining mark (U+0300-036F) as one letter, or 0 */
unsigned r_compose(unsigned base, unsigned mark)
{
	static struct { unsigned short cp; char m; } t[] = {
		{0x300, '`'}, {0x301, '\''}, {0x302, '^'}, {0x303, '~'},
		{0x304, '='}, {0x306, 'u'}, {0x307, '.'}, {0x308, ':'},
		{0x30a, 'o'}, {0x30b, '"'}, {0x30c, 'v'}, {0x327, ','},
		{0x328, ';'}, {0, 0}
	};
	int i, m = 0;

	if (!((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z')))
		return 0;
	for (i = 0; t[i].cp; i++)
		if (t[i].cp == mark)
			m = t[i].m;
	if (!m)
		return 0;
	for (i = 0; i < 64; i++)
		if (latin1[i * 2] == (char)base && latin1[i * 2 + 1] == m)
			return 0xc0 + i;
	for (i = 0; latx[i].cp; i++)
		if (latx[i].base == (char)base && latx[i].mark == m &&
			latx[i].cp < 0x180)
			return latx[i].cp;
	return 0;
}

/* ---- symbols: syms.txt ---- */
static int sym(unsigned short *g, unsigned cp)
{
	int i, y;

	for (i = 0; syms[i].cp; i++)
		if (syms[i].cp == cp) {
			for (y = 0; y < CH; y++)
				g[y] = syms[i].rows[y];
			return 1;
		}
	return 0;
}

/* letters that look like an ASCII one */
static int ascii(unsigned cp)
{
	static struct { unsigned short a, b; char c; } t[] = {
		{0x2000, 0x200a, ' '}, {0x202f, 0x202f, ' '},
		{0x205f, 0x205f, ' '}, {0x3000, 0x3000, ' '},
		{0x2010, 0x2015, '-'}, {0x2212, 0x2212, '-'},
		{0x2018, 0x2019, '\''}, {0x201a, 0x201a, ','},
		{0x201b, 0x201b, '\''}, {0x201c, 0x201f, '"'},
		{0x2032, 0x2032, '\''}, {0x2033, 0x2033, '"'},
		{0x2039, 0x2039, '<'}, {0x203a, 0x203a, '>'},
		{0x2020, 0x2021, '+'}, {0x2030, 0x2030, '%'},
		{0x203c, 0x203c, '!'}, {0x2044, 0x2044, '/'},
		{0x02c6, 0x02c6, '^'}, {0x02dc, 0x02dc, '~'},
		{0x2215, 0x2215, '/'}, {0x2216, 0x2216, '\\'},
		{0x2217, 0x2217, '*'}, {0x2223, 0x2223, '|'},
		{0x2236, 0x2236, ':'}, {0x223c, 0x223c, '~'},
		{0x2227, 0x2227, '^'}, {0x2228, 0x2228, 'v'},
		{0x00a6, 0x00a6, '|'}, {0x00b4, 0x00b4, '\''},
		{0x00a8, 0x00a8, '"'}, {0x00af, 0x00af, '-'},
		{0x00b8, 0x00b8, ','}, {0x00aa, 0x00aa, 'a'},
		{0x00ba, 0x00ba, 'o'}, {0x00a4, 0x00a4, '*'},
		{0x00be, 0x00be, '?'}, {0x00ad, 0x00ad, '-'},
		{0x2122, 0x2122, 'T'}, {0x2713, 0x2713, 'v'},
		{0x25e6, 0x25e6, 'o'}, {0x2027, 0x2027, '.'},
		{0x23ba, 0x23bd, 0}, {0x2409, 0x2424, '?'},
		{0, 0, 0}
	};
	/* Greek and Cyrillic that look like Latin */
	static char *grk = "ABGDEZHQIKLMNXOPRSSTYFXCW";	/* 0x391- */
	static char *cyr = "ABVGDEZZIIKLMNOPRSTUFHCCSSYYIEUA"; /* 0x410- */
	int i;

	if (cp >= 0xff01 && cp <= 0xff5e)
		return cp - 0xfee0;
	for (i = 0; t[i].a; i++)
		if (cp >= t[i].a && cp <= t[i].b)
			return t[i].c;
	if (cp >= 0x391 && cp <= 0x3a9)
		return grk[cp - 0x391];
	if (cp >= 0x3b1 && cp <= 0x3c9)
		return grk[cp - 0x3b1] | 0x20;
	if (cp >= 0x410 && cp <= 0x42f)
		return cyr[cp - 0x410];
	if (cp >= 0x430 && cp <= 0x44f)
		return cyr[cp - 0x430] | 0x20;
	return -1;
}

/* the rows of a letter; odd: the cell's column is odd (for shades) */
static void glyph(unsigned cp, unsigned short *g, int odd)
{
	int i, c;

	for (i = 0; i < CH; i++)
		g[i] = 0;
	if (cp >= 0x20 && cp < 0x7f) {
		for (i = 0; i < CH; i++)
			g[i] = font[cp - 32][i];
		return;
	}
	if (cp < 0x20 || cp == 0x7f || cp == 0xa0)
		return;
	if (cp >= 0x2500 && cp <= 0x257f) {
		char *d = boxes + (cp - 0x2500) * 4;

		if (d[0] == 'd')
			dash(g, d);
		else if (d[0] == 'x')
			diag(g, d[1] - '0');
		else
			box(g, d);
		return;
	}
	if (cp >= 0x2580 && cp <= 0x259f) {
		block(g, cp - 0x2500, odd);
		return;
	}
	if (cp >= 0x2800 && cp <= 0x28ff) {
		braille(g, cp - 0x2800);
		return;
	}
	if (cp >= 0x23ba && cp <= 0x23bd) {	/* DEC scan lines 1 3 7 9 */
		static char row[4] = {0, 3, 8, 11};

		g[(int)row[cp - 0x23ba]] = 0x1ff;
		return;
	}
	if (cp >= 0xc0 && cp <= 0xff) {
		i = (cp - 0xc0) * 2;
		if (latin1[i] == '*')
			sym(g, cp);
		else
			letter(g, latin1[i], latin1[i + 1]);
		return;
	}
	if (cp == 0xfffd) {		/* a bad byte: an inverse ? */
		for (i = 0; i < CH; i++)
			g[i] = font['?' - 32][i] ^ (i < 11 ? 0x1ff : 0);
		return;
	}
	if (cp >= 0x100 && cp <= 0x451) {
		for (i = 0; latx[i].cp; i++)
			if (latx[i].cp == cp) {
				letter(g, latx[i].base, latx[i].mark);
				return;
			}
	}
	if (sym(g, cp))
		return;
	c = ascii(cp);
	if (c == 0)
		return;
	if (c < 0)
		c = '?';
	for (i = 0; i < CH; i++)
		g[i] = font[c - 32][i];
}

void r_glyph(unsigned cp, unsigned short *rows)
{
	glyph(cp, rows, 0);
}

/* does bold thicken this letter? not lines and blocks */
static int thick(unsigned cp)
{
	return !(cp >= 0x2500 && cp <= 0x25ff) && !(cp >= 0x2800 &&
		cp <= 0x28ff) && !(cp >= 0x23ba && cp <= 0x23bd);
}

/* one cell into the bitmap; cur: the cursor is on it */
static void drawcell(int cx, int cy, Cell *c, int cur)
{
	unsigned short g[CH];
	unsigned long v, m;
	int y, x0, w, s, inv;

	glyph(c->ch ? c->ch : ' ', g, cx & 1);
	/* the background is lit if its colour is light; reverse video
	 * always turns it over, so it shows whatever the colours */
	inv = c->bg != C_DEF && lum[c->bg] >= 8;
	if (c->at & AT_REV)
		inv = !inv;
	if (c->at & AT_INVIS)
		for (y = 0; y < CH; y++)
			g[y] = 0;
	if ((c->at & AT_BOLD) && thick(c->ch))
		for (y = 0; y < CH; y++)
			g[y] |= (g[y] << 1) & 0x1ff;
	if ((c->at & AT_ITAL) && thick(c->ch))	/* a slant */
		for (y = 0; y < CH; y++)
			g[y] = y < 4 ? (g[y] << 1) & 0x1ff : y > 7 ? g[y] >> 1 :
				g[y];
	if (c->at & AT_UNDER)
		g[10] = 0x1ff;
	if (c->at & AT_STRIKE)
		g[5] = 0x1ff;
	if (vt.rvscreen)
		inv = !inv;
	if (inv)
		for (y = 0; y < CH; y++)
			g[y] ^= 0x1ff;
	if (cur) {
		for (y = vt.curshape ? CH - 2 : 0; y < CH; y++)
			g[y] ^= 0x1ff;
	}
	x0 = cx * CW;
	w = x0 >> 4;
	s = x0 & 15;
	m = (unsigned long)0x1ff << s;
	for (y = 0; y < CH; y++) {
		unsigned short *p = &bm[cy * CH + y][w];

		v = (unsigned long)g[y] << s;
		p[0] = (p[0] & ~(unsigned short)(m & 0xffff)) |
			(unsigned short)(v & 0xffff);
		if (s > 7)
			p[1] = (p[1] & ~(unsigned short)(m >> 16)) |
				(unsigned short)(v >> 16);
	}
}

static int same(Cell *a, Cell *b)
{
	return a->ch == b->ch && a->at == b->at && a->fg == b->fg &&
		a->bg == b->bg;
}

/* the changed cells into the bitmap, the changed rows to the screen */
void r_draw(int all)
{
	static int lastrv = -1;
	int x, y, y0 = -1, cx, cy, on, changed;

	if (vt.rvscreen != lastrv) {
		all = 1;
		lastrv = vt.rvscreen;
	}
	on = vt.curon;
	cx = on ? vt.x : -1;
	cy = on ? vt.y : -1;
	if (cx != curx || cy != cury) {
		if (cury >= 0 && cury < ROWS)
			vt.dirty[cury] = 1;
		if (cy >= 0)
			vt.dirty[cy] = 1;
	}
	for (y = 0; y <= ROWS; y++) {
		changed = 0;
		if (y < ROWS && (all || vt.dirty[y])) {
			for (x = 0; x < COLS; x++) {
				Cell *c = &SCR[y][x];
				int iscur = x == cx && y == cy;
				int wascur = x == curx && y == cury;

				if (all || !same(c, &shadow[y][x]) ||
					iscur != wascur) {
					drawcell(x, y, c, iscur);
					shadow[y][x] = *c;
					changed = 1;
				}
			}
			vt.dirty[y] = 0;
		}
		if (changed) {
			if (y0 < 0)
				y0 = y;
		} else if (y0 >= 0) {
			d_blit(y0 * CH, (y - y0) * CH);
			y0 = -1;
		}
	}
	curx = cx;
	cury = cy;
}

/* ---- ASCII for plain terminals (-a) ---- */

/* an ASCII letter that looks like cp, 0 for nothing */
int r_ascii(unsigned cp)
{
	static struct { unsigned short cp; char c; } t[] = {
		{0x2190, '<'}, {0x2192, '>'}, {0x2191, '^'}, {0x2193, 'v'},
		{0x2194, '-'}, {0x2195, '|'}, {0x25b2, '^'}, {0x25bc, 'v'},
		{0x25ba, '>'}, {0x25b6, '>'}, {0x25c4, '<'}, {0x25c0, '<'},
		{0x25a0, '#'}, {0x25a1, '#'}, {0x25aa, '#'}, {0x25ab, '#'},
		{0x25ac, '#'}, {0x25cf, 'o'}, {0x25cb, 'o'}, {0x25d8, '#'},
		{0x25d9, '#'}, {0x2022, '*'}, {0x2219, '.'}, {0x00b7, '.'},
		{0x25c6, '*'}, {0x2666, '*'}, {0x2665, '*'}, {0x2660, '*'},
		{0x2663, '*'}, {0x263a, ':'}, {0x263b, ':'}, {0x263c, '*'},
		{0x266a, 'd'}, {0x266b, 'd'}, {0x2605, '*'}, {0x2606, '*'},
		{0x2713, 'v'}, {0x2714, 'v'}, {0x2717, 'x'}, {0x2718, 'x'},
		{0x2302, '^'}, {0x2026, '.'}, {0x00b0, 'o'}, {0x00b1, '+'},
		{0x2264, '<'}, {0x2265, '>'}, {0x2260, '#'}, {0x2261, '='},
		{0x2248, '~'}, {0x03c0, 'p'}, {0x00a3, 'L'}, {0x20ac, 'E'},
		{0x00a2, 'c'}, {0x00a5, 'Y'}, {0x00a7, 'S'}, {0x00b6, 'P'},
		{0x00b5, 'u'}, {0x00df, 's'}, {0x00a9, 'C'}, {0x00ae, 'R'},
		{0x00bf, '?'}, {0x00a1, '!'}, {0x00ab, '<'}, {0x00bb, '>'},
		{0x00d7, 'x'}, {0x00f7, '/'}, {0x00ac, '-'}, {0x00bd, '/'},
		{0x00bc, '/'}, {0x00be, '/'}, {0x00b2, '2'}, {0x00b3, '3'},
		{0x00b9, '1'}, {0x221e, '8'}, {0x221a, 'V'}, {0x2229, 'n'},
		{0x207f, 'n'}, {0x2310, '-'}, {0x2320, '|'}, {0x2321, '|'},
		{0x00a0, ' '}, {0xfffd, '?'}, {0, 0}
	};
	char *d;
	int i, h, v, c;

	if (cp >= 0x20 && cp < 0x7f)
		return cp;
	if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0) ||
		(cp >= 0x300 && cp <= 0x36f) || (cp >= 0x200b && cp <= 0x200f) ||
		(cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0xfeff)
		return 0;		/* marks and other letters of no width */
	for (i = 0; t[i].cp; i++)
		if (t[i].cp == cp)
			return t[i].c;
	if (cp >= 0xc0 && cp <= 0xff) {
		c = latin1[(cp - 0xc0) * 2];
		return c == '*' ? '?' : c;
	}
	for (i = 0; latx[i].cp; i++)
		if (latx[i].cp == cp)
			return latx[i].base;
	if (cp >= 0x2500 && cp <= 0x257f) {
		d = boxes + (cp - 0x2500) * 4;
		if (d[0] == 'd')
			return d[2] == 'h' ? '-' : '|';
		if (d[0] == 'x')
			return d[1] == '1' ? '/' : d[1] == '2' ? '\\' : 'X';
		h = d[0] != '0' || d[1] != '0';
		v = d[2] != '0' || d[3] != '0';
		if (h && v)
			return '+';
		if (h)
			return d[0] == '3' || d[1] == '3' ? '=' : '-';
		return '|';
	}
	if (cp >= 0x2591 && cp <= 0x2593)
		return ".:#"[cp - 0x2591];
	if (cp >= 0x2580 && cp <= 0x259f)
		return '#';
	if (cp >= 0x2800 && cp <= 0x28ff) {
		for (h = 0, i = cp & 0xff; i; i >>= 1)
			h += i & 1;
		return h == 0 ? ' ' : h < 3 ? '.' : h < 6 ? ':' : '#';
	}
	if (cp >= 0x23ba && cp <= 0x23bd)
		return cp == 0x23bd ? '_' : '-';
	c = ascii(cp);
	return c < 0 ? '?' : c;
}
