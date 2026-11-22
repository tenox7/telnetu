/* tn.h - telnet for the AT&T UNIX PC (3B1): what the parts share.
 *
 * telnet.c  the program: options, the telnet protocol, keys, commands
 * vt.c      the terminal: VT100/VT220/xterm codes, UTF-8, CP437, colours
 * glyph.c   the letters and the lines, drawn into a bitmap of the window
 * w3b1.c    the 3B1 window (or wsim.c, a stand-in for tests on a Mac)
 * ttyb.c    a plain terminal (serial port, rlogin, telnet): no emulation
 * net.c     TCP and DNS
 */
#define VERSION "1.0"

#define COLS 80
#define ROWS 24
#define CW 9			/* a cell is 9 x 12 pixels, as the system font */
#define CH 12
#define WPR 45			/* 16-bit words in a row of 720 pixels */
#define PXH (ROWS * CH)		/* 288 */

/* cell attributes */
#define AT_BOLD		0x01
#define AT_DIM		0x02
#define AT_ITAL		0x04
#define AT_UNDER	0x08
#define AT_BLINK	0x10
#define AT_REV		0x20
#define AT_INVIS	0x40
#define AT_STRIKE	0x80

#define C_DEF 16		/* the default colour; 0-15 are the ANSI colours */

typedef struct {
	unsigned short ch;	/* Unicode (BMP); 0 = right half of a wide one */
	unsigned char at, fg, bg;
} Cell;

typedef struct {
	int x, y, at, fg, bg, wrap, origin, gl, g[4];
} Saved;

/* character sets */
#define CS_AUTO   0		/* UTF-8, and CP437 for bytes that are not */
#define CS_UTF8   1
#define CS_LATIN1 2
#define CS_CP437  3
#define CS_ASCII  4		/* tty mode only: UTF-8 shown as ASCII */

typedef struct {
	Cell buf[2][ROWS][COLS];	/* the main and the alternate screen */
	int alt;
	int x, y;			/* the cursor */
	int wrap;			/* the last column is written: wrap first */
	int at, fg, bg;			/* attributes for new letters */
	int top, bot;			/* the scrolling region */
	int origin, awrap, insert, lnm, curon, appcur, appkey, rvscreen;
	int curshape;			/* 0 block, 1 underline */
	int g[4], gl, ss;		/* G0-G3; GL is G0 or G1; single shift */
	Saved save[2];			/* DECSC, one for each screen */
	char tabs[COLS];
	int charset;
	int mouse, msgr;		/* mouse reports the host asked for */
	unsigned long last;		/* the last letter (REP) */
	char dirty[ROWS];
	/* the parser */
	int st, par[16], np, sub, priv, inter, strk, ns;
	char str[160];
	unsigned long ucp;
	int uneed, un;
	unsigned char ub[4];
	int good, bad, is437;		/* auto: UTF-8 letters, bad bytes */
} Vt;

extern Vt vt;
#define SCR vt.buf[vt.alt]

/* keys from the window, besides the bytes 0-255 */
#define K_UP	0x100
#define K_DOWN	0x101
#define K_RIGHT	0x102
#define K_LEFT	0x103
#define K_HOME	0x104
#define K_END	0x105
#define K_PGUP	0x106
#define K_PGDN	0x107
#define K_INS	0x108
#define K_DEL	0x109
#define K_BTAB	0x10a
#define K_EXIT	0x10b		/* Exit: the command prompt */
#define K_REDRAW 0x10c
#define K_HELP	0x10d
#define K_MOUSE	0x10e		/* a click: mx, my, mb */
#define K_BRK	0x10f		/* Break: a telnet BRK */
#define K_F1	0x120		/* to K_F1 + 15 */

/* vt.c */
void vt_init(void);
void vt_reset(void);
void vt_write(unsigned char *s, int n);
void vt_puts(char *s);
void vt_dirtyall(void);
int vt_wide(unsigned long c);	/* a letter two cells wide */
unsigned vt_cp437(int b);	/* a CP437 byte as Unicode */
unsigned vt_c0(int b);		/* CP437's picture for a control code, or 0 */

/* glyph.c */
extern unsigned short bm[PXH][WPR];
int r_init(char *fontfile);
void r_draw(int all);
void r_glyph(unsigned cp, unsigned short *rows);
int r_ascii(unsigned cp);	/* an ASCII letter like it, or 0 */
unsigned r_compose(unsigned base, unsigned mark);

/* the display: w3b1.c or wsim.c */
int d_open(void);
void d_close(void);
void d_blit(int y, int h);	/* bitmap rows to the screen */
int d_key(int wait);		/* a key, or -1; wait: at most 0.1 s */
void d_title(char *s);
void d_prompt(char *s);
void d_bell(void);
int d_shell(void);		/* a local shell in a window of its own */
extern int mx, my, mb;		/* the last click: cell, button 0 1 2 */

/* the plain terminal: ttyb.c */
int t_open(void);
void t_raw(int raw);
void t_close(void);
int t_key(int wait);
void t_write(char *s, int n);

/* net.c */
int n_open(char *host, int port, char *err, char *addr);
int n_wait(int s, int ms);
int n_read(int s, char *buf, int n);
int n_write(int s, char *buf, int n);
void n_close(int s);
long n_ms(void);		/* a clock, in ms */

/* telnet.c: what the terminal answers to the host */
void tn_reply(char *s, int n);
void tn_bell(void);
void tn_title(char *s);
