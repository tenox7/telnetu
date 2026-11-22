/* telnet.c - telnet for the AT&T UNIX PC (3B1).
 *
 *	telnet [-t] [-a] [-e c] [-T type] [-C charset] [host [port]]
 *
 * In a window of the 3B1's console it opens a window of its own, the whole
 * screen (80 x 24), and is a terminal there: VT100, VT220 and xterm codes,
 * UTF-8, CP437 and DEC line drawing, drawn with its own letters and lines.
 * Anywhere else (the serial port, rlogin, telnet) it passes the host's
 * output on as it is, as the old telnet did (-t does that in a window too).
 *
 * It speaks the telnet protocol (RFC 854) and asks for options as RFC 1143
 * says, so it never loops. It does BINARY, ECHO, SGA, TTYPE, NAWS, CHARSET
 * and TM, and says no to the rest.
 *
 * The escape character (^] or ~ at the start of a line, or the Exit key)
 * gives the telnet> prompt. The old telnet's commands still work there.
 */
#include <stdio.h>
#include <signal.h>
#ifdef __APPLE__
#include <stdlib.h>
#include <unistd.h>
#else
extern char *getenv();
extern void exit();
extern int system();
#endif
#include "tn.h"

#define IAC	255
#define DONT	254
#define DO	253
#define WONT	252
#define WILL	251
#define SB	250
#define GA	249
#define EL	248
#define EC	247
#define AYT	246
#define AO	245
#define IP	244
#define BRK	243
#define DM	242
#define NOP	241
#define SE	240
#define TEOF	236

#define O_BINARY	0
#define O_ECHO		1
#define O_SGA		3
#define O_STATUS	5
#define O_TM		6
#define O_TTYPE		24
#define O_NAWS		31
#define O_CHARSET	42

/* option states, RFC 1143 */
#define NO	0
#define YES	1
#define WANTNO	2
#define WANTYES	3

static unsigned char us[256], him[256];

static int net = -1;		/* the connection */
static char host[80], addr[40];
static int port = 23;
static int win;			/* the window and its terminal; else tty */
static int escc = 035;		/* ^] */
static int tildes = 1;		/* ~ at the start of a line escapes too */
static int bol = 1;		/* at the start of a line */
static int crlf = 1;		/* Return sends CR LF (CR NUL if 0) */
static int bsdel = 1;		/* Backspace sends DEL (BS if 0) */
static int showopts;
static int force;		/* line or char mode chosen by the user */
static char *ttypes[4] = {"XTERM", "VT100", 0, 0};
static int ntt = 2, tti;
static char title[80];
static int quitting;
static int interactive;		/* no host on the command line */
static int lost;		/* the connection broke while sending */

/* the line of line mode (the host does not echo) */
static char line[256];
static int ln;

static unsigned char obuf[2048];
static int on;

static void out(unsigned char *s, int n);
static void cmdmode(int fromkey);
static void note(char *s);

/* ---- sending ---- */

static void nflush(void)
{
	if (on > 0 && net >= 0)
		if (n_write(net, (char *)obuf, on) < 0) {
			n_close(net);
			net = -1;
			lost = 1;
		}
	on = 0;
}

static void nputc(int c)
{
	if (on >= (int)sizeof obuf - 2)
		nflush();
	obuf[on++] = c;
}

/* data: an IAC in it goes twice */
static void nput(char *s, int n)
{
	while (n-- > 0) {
		if ((*s & 0377) == IAC)
			nputc(IAC);
		nputc(*s++ & 0377);
	}
}

static void nputs(char *s)
{
	int n = 0;

	while (s[n])
		n++;
	nput(s, n);
}

/* what the terminal answers (DA, cursor reports) */
void tn_reply(char *s, int n)
{
	nput(s, n);
}

void tn_bell(void)
{
	if (win)
		d_bell();
}

void tn_title(char *s)
{
	int i;

	for (i = 0; s[i] && i < (int)sizeof title - 1; i++)
		title[i] = (s[i] & 0377) < ' ' || (s[i] & 0377) > '~' ? ' ' :
			s[i];
	title[i] = 0;
	if (win)
		d_title(title[0] ? title : host);
}

static char *optname(int o)
{
	static char buf[8];
	static char *names[] = {"BINARY", "ECHO", "RCP", "SGA", "NAMS",
		"STATUS", "TM"};

	if (o < 7)
		return names[o];
	switch (o) {
	case 24: return "TTYPE";
	case 31: return "NAWS";
	case 32: return "TSPEED";
	case 33: return "LFLOW";
	case 34: return "LINEMODE";
	case 35: return "XDISPLOC";
	case 36: return "ENVIRON";
	case 39: return "NEW-ENVIRON";
	case 42: return "CHARSET";
	}
	sprintf(buf, "%d", o);
	return buf;
}

static void showopt(char *dir, int cmd, int o)
{
	char buf[80];

	if (!showopts)
		return;
	sprintf(buf, "%s %s %s\r\n", dir, cmd == DO ? "DO" : cmd == DONT ?
		"DONT" : cmd == WILL ? "WILL" : cmd == WONT ? "WONT" : "SB",
		optname(o));
	note(buf);
}

static void sendopt(int cmd, int o)
{
	nputc(IAC);
	nputc(cmd);
	nputc(o);
	showopt("SENT", cmd, o);
}

static void naws(void)
{
	nputc(IAC);
	nputc(SB);
	nputc(O_NAWS);
	nputc(0);
	nputc(COLS);
	nputc(0);
	nputc(ROWS);
	nputc(IAC);
	nputc(SE);
}

/* ---- options ---- */

/* we do these */
static int willok(int o)
{
	return o == O_BINARY || o == O_SGA || o == O_TTYPE || o == O_NAWS ||
		o == O_CHARSET;
}

/* the host may do these */
static int dook(int o)
{
	return o == O_BINARY || o == O_ECHO || o == O_SGA;
}

static void usnow(int o)		/* we now do o */
{
	if (o == O_NAWS && us[o] == YES)
		naws();
}

static void ask(int will, int o)
{
	if (will && us[o] == NO) {
		us[o] = WANTYES;
		sendopt(WILL, o);
	} else if (!will && him[o] == NO) {
		him[o] = WANTYES;
		sendopt(DO, o);
	}
}

static void gotopt(int cmd, int o)
{
	showopt("RCVD", cmd, o);
	switch (cmd) {
	case DO:
		if (o == O_TM) {	/* RFC 860: answer, change nothing */
			sendopt(WILL, O_TM);
			break;
		}
		if (us[o] == NO) {
			if (willok(o)) {
				us[o] = YES;
				sendopt(WILL, o);
				usnow(o);
			} else
				sendopt(WONT, o);
		} else if (us[o] == WANTYES) {
			us[o] = YES;
			usnow(o);
		} else if (us[o] == WANTNO)
			us[o] = NO;
		break;
	case DONT:
		if (us[o] == YES) {
			us[o] = NO;
			sendopt(WONT, o);
		} else if (us[o] != NO)
			us[o] = NO;
		break;
	case WILL:
		if (him[o] == NO) {
			if (dook(o)) {
				him[o] = YES;
				sendopt(DO, o);
			} else
				sendopt(DONT, o);
		} else if (him[o] == WANTYES)
			him[o] = YES;
		else if (him[o] == WANTNO)
			him[o] = NO;
		break;
	case WONT:
		if (him[o] == YES) {
			him[o] = NO;
			sendopt(DONT, o);
		} else if (him[o] != NO)
			him[o] = NO;
		break;
	}
}

static int match(char *a, char *b)
{
	int x, y;

	for (; *a && *b; a++, b++) {
		x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
		y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
		if (x != y)
			return 0;
	}
	return *a == *b;
}

static unsigned char sbuf[256];
static int sbn;

static void subneg(void)
{
	char *t;
	int i, j, sep;

	if (sbn < 2)
		return;
	showopt("RCVD", SB, sbuf[0]);
	if (sbuf[0] == O_TTYPE && sbuf[1] == 1 && us[O_TTYPE] == YES) {
		/* RFC 1091: each SEND gets the next type; the last one
		 * comes twice, then it starts again */
		t = ttypes[tti < ntt ? tti : ntt - 1];
		if (tti > ntt)
			tti = 0, t = ttypes[0];
		tti++;
		nputc(IAC);
		nputc(SB);
		nputc(O_TTYPE);
		nputc(0);
		while (*t)
			nputc(*t++);
		nputc(IAC);
		nputc(SE);
	} else if (sbuf[0] == O_CHARSET && sbuf[1] == 1 &&
		us[O_CHARSET] == YES) {
		/* RFC 2066 REQUEST: yes to UTF-8, or to the charset the
		 * user chose. The terminal stays as it is: in auto mode
		 * bytes that are not UTF-8 still show as CP437 */
		i = 2;
		if (sbn > 10 && sbuf[2] == '[')
			while (i < sbn && sbuf[i] != ']')
				i++, i += i < sbn;
		sep = i < sbn ? sbuf[i++] : ';';
		while (i < sbn) {
			char name[40];
			int ok;

			for (j = 0; i < sbn && sbuf[i] != sep; i++)
				if (j < (int)sizeof name - 1)
					name[j++] = sbuf[i];
			name[j] = 0;
			i++;
			if (vt.charset == CS_LATIN1)
				ok = match(name, "ISO-8859-1") ||
					match(name, "LATIN1");
			else if (vt.charset == CS_CP437)
				ok = match(name, "CP437") || match(name, "IBM437");
			else
				ok = match(name, "UTF-8") || match(name, "UTF8");
			if (ok) {
				nputc(IAC);
				nputc(SB);
				nputc(O_CHARSET);
				nputc(2);
				for (t = name; *t; )
					nputc(*t++);
				nputc(IAC);
				nputc(SE);
				return;
			}
		}
		nputc(IAC);
		nputc(SB);
		nputc(O_CHARSET);
		nputc(3);
		nputc(IAC);
		nputc(SE);
	}
}

/* ---- what comes from the host ---- */

static int ts;			/* the state of tnin() */
static int lastcr;

static void tnin(unsigned char *b, int n)
{
	static unsigned char data[2048];
	int k = 0, c;

	while (n-- > 0) {
		c = *b++;
		switch (ts) {
		case 0:
			if (c == IAC) {
				ts = 1;
				break;
			}
			if (lastcr && c == 0 && him[O_BINARY] != YES) {
				lastcr = 0;
				break;
			}
			lastcr = c == '\r';
			data[k++] = c;
			break;
		case 1:
			ts = 0;
			if (c == IAC) {
				data[k++] = c;
				lastcr = 0;
			} else if (c >= WILL && c <= DONT)
				ts = c;
			else if (c == SB) {
				sbn = 0;
				ts = SB;
			}
			break;
		case WILL:
		case WONT:
		case DO:
		case DONT:
			out(data, k);
			k = 0;
			gotopt(ts, c);
			ts = 0;
			break;
		case SB:
			if (c == IAC)
				ts = SE;
			else if (sbn < (int)sizeof sbuf)
				sbuf[sbn++] = c;
			break;
		case SE:
			if (c == IAC) {
				if (sbn < (int)sizeof sbuf)
					sbuf[sbn++] = c;
				ts = SB;
			} else {
				out(data, k);
				k = 0;
				subneg();
				ts = c == SE ? 0 : 1;
				if (ts == 1) {		/* IAC x in SB: take it */
					b--;
					n++;
				}
			}
			break;
		}
		if (k == (int)sizeof data) {
			out(data, k);
			k = 0;
		}
	}
	out(data, k);
}

/* ---- the screen or the tty ---- */

static void out(unsigned char *s, int n)
{
	if (n <= 0)
		return;
	if (win)
		vt_write(s, n);
	else
		t_write((char *)s, n);
}

static void puts2(char *s)
{
	int n = 0;

	while (s[n])
		n++;
	out((unsigned char *)s, n);
}

/* a message of ours: in the window it goes on with the host's output */
static void note(char *s)
{
	puts2(s);
	if (win)
		r_draw(0);
}

/* a key, or -1; wait: wait for one at most 0.1 s */
static int key(int wait)
{
	return win ? d_key(wait) : t_key(wait);
}

/* ---- keys to the host ---- */

static int linemode(void)
{
	if (force)
		return force == 1;
	return him[O_ECHO] != YES;
}

static void sendcr(void)
{
	if (vt.lnm && win)
		nputs("\r\n");
	else if (us[O_BINARY] == YES)
		nputc('\r');
	else if (crlf)
		nputs("\r\n");
	else {
		nputc('\r');
		nputc(0);
	}
}

static char *fkeys[] = {
	"\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
	"\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~",
	"\033[24~", "\033[25~", "\033[26~", "\033[28~", "\033[29~"
};

static void sendkey(int k)
{
	static char arrows[] = "ABCDHF";
	char buf[32];

	if (k >= K_UP && k <= K_END) {
		buf[0] = 033;
		buf[1] = vt.appcur ? 'O' : '[';
		buf[2] = arrows[k == K_UP ? 0 : k == K_DOWN ? 1 :
			k == K_RIGHT ? 2 : k == K_LEFT ? 3 : k == K_HOME ? 4 : 5];
		nput(buf, 3);
		return;
	}
	switch (k) {
	case K_PGUP: nputs("\033[5~"); return;
	case K_PGDN: nputs("\033[6~"); return;
	case K_INS: nputs("\033[2~"); return;
	case K_DEL: nputs("\033[3~"); return;
	case K_BTAB: nputs("\033[Z"); return;
	case K_BRK:
		nputc(IAC);
		nputc(BRK);
		return;
	case K_MOUSE:
		if (!vt.mouse)
			return;
		if (vt.msgr) {
			sprintf(buf, "\033[<%d;%d;%dM", mb, mx + 1, my + 1);
			nputs(buf);
			sprintf(buf, "\033[<%d;%d;%dm", mb, mx + 1, my + 1);
			nputs(buf);
		} else {
			sprintf(buf, "\033[M%c%c%c", 32 + mb, 33 + mx, 33 + my);
			nputs(buf);
			sprintf(buf, "\033[M%c%c%c", 32 + 3, 33 + mx, 33 + my);
			nputs(buf);
		}
		return;
	}
	if (k >= K_F1 && k < K_F1 + 16) {
		nputs(fkeys[k - K_F1]);
		return;
	}
	if (k == '\r' || k == '\n') {
		sendcr();
		return;
	}
	if (k == 010 && win)
		k = bsdel ? 0177 : 010;
	buf[0] = k;
	nput(buf, 1);
}

/* line mode: the line is edited here and goes on Return */
static void linekey(int k)
{
	char c;

	if (k == '\r' || k == '\n') {
		nput(line, ln);
		sendcr();
		ln = 0;
		note("\r\n");
	} else if (k == 010 || k == 0177) {
		if (ln > 0) {
			ln--;
			note("\b \b");
		}
	} else if (k == 025) {			/* ^U */
		while (ln > 0) {
			ln--;
			note("\b \b");
		}
	} else if (k == 027) {			/* ^W */
		while (ln > 0 && line[ln - 1] == ' ') {
			ln--;
			note("\b \b");
		}
		while (ln > 0 && line[ln - 1] != ' ') {
			ln--;
			note("\b \b");
		}
	} else if (k == 003) {			/* ^C: IP */
		ln = 0;
		nputc(IAC);
		nputc(IP);
		note("^C\r\n");
	} else if (k >= K_UP || k == 033) {
		sendkey(k);
	} else if (k < ' ' && k != '\t') {
		nput(line, ln);
		ln = 0;
		c = k;
		nput(&c, 1);
	} else if (ln < (int)sizeof line - 1) {
		line[ln++] = k;
		c = k;
		if (win) {
			unsigned char u = k;

			vt_write(&u, 1);
			r_draw(0);
		} else
			t_write(&c, 1);
	}
}

/* a key from the user */
static void dokey(int k)
{
	if (k == K_EXIT || k == escc) {
		cmdmode(1);
		return;
	}
	if (k == '~' && tildes && bol && (!linemode() || ln == 0)) {
		cmdmode(3);		/* as the old telnet: ~quit, ~? */
		return;
	}
	if (k == K_REDRAW) {
		if (win)
			r_draw(1);
		return;
	}
	if (k == K_HELP) {
		cmdmode(2);
		return;
	}
	bol = k == '\r' || k == '\n' || k == 003 || k == 025;
	if (linemode())
		linekey(k);
	else
		sendkey(k);
}

/* ---- the connection ---- */

static void reset(void)
{
	int i;

	for (i = 0; i < 256; i++)
		us[i] = him[i] = NO;
	ts = 0;
	lastcr = 0;
	tti = 0;
	vt.good = vt.bad = vt.is437 = 0;
	ln = 0;
	bol = 1;
	on = 0;
	title[0] = 0;
}

static int topen(char *h, char *p)
{
	char err[160], buf[200];
	int i, pn = 23, tport = 1;

	if (p && *p) {
		/* "-23" asks for options on another port too, as BSD does */
		if (*p == '-') {
			p++;
			tport = 2;
		}
		for (pn = 0, i = 0; p[i] >= '0' && p[i] <= '9'; i++)
			pn = pn * 10 + p[i] - '0';
		if (p[i] || pn <= 0 || pn > 65535) {
			sprintf(buf, "%s: bad port number\r\n", p);
			note(buf);
			return -1;
		}
		if (pn != 23 && tport == 1)
			tport = 0;
	}
	for (i = 0; h[i] && i < (int)sizeof host - 1; i++)
		host[i] = h[i];
	host[i] = 0;
	port = pn;
	sprintf(buf, "Trying %s...\r\n", host);
	note(buf);
	if (win) {
		sprintf(buf, "Trying %s ...", host);
		d_prompt(buf);
	}
	reset();
	net = n_open(host, port, err, addr);
	if (net < 0) {
		sprintf(buf, "%s\r\n", err);
		note(buf);
		if (win)
			d_prompt("telnet: no connection");
		return -1;
	}
	sprintf(buf, "Connected to %s (%s).\r\nEscape character is '^%c'%s."
		"\r\n", host, addr, escc + '@', !tildes ? "" : win ?
		" (or ~ at the start of a line, or the Exit key)" :
		" (or ~ at the start of a line)");
	note(buf);
	if (win) {
		sprintf(buf, "%s  -  Exit or ^%c: telnet> prompt", host,
			escc + '@');
		d_prompt(buf);
		d_title(host);
	}
	/* a telnet server: say what we can do (BSD telnet does as much) */
	if (tport) {
		ask(0, O_SGA);
		ask(1, O_TTYPE);
		ask(1, O_NAWS);
	}
	nflush();
	return 0;
}

static void tclose(void)
{
	if (net >= 0) {
		nflush();
		n_close(net);
		net = -1;
		note("Connection closed.\r\n");
	}
}

/* ---- the telnet> prompt ---- */

static char *help[] = {
	"open host [port]  connect (o)",
	"close             close the connection (c)",
	"quit              close and leave telnet (q, Exit twice)",
	"z                 a local shell (exit it to come back)",
	"send x            ayt brk ip ao ec el eof nop escape",
	"escape [c]        the escape character, as ^] or c",
	"term [types]      terminal types to give the host",
	"charset c         auto utf8 latin1 cp437",
	"crlf              Return sends CR LF or CR NUL",
	"bs                Backspace sends DEL or BS",
	"mode line|char    local or remote echo",
	"options           show option negotiation",
	"status            what is going on",
	"Return            back to the host",
	0
};

static void status(void)
{
	char buf[200];
	static char *cs[] = {"auto (UTF-8, else CP437)", "UTF-8", "Latin-1",
		"CP437", "ASCII"};
	char *c = vt.charset == CS_AUTO && vt.is437 ?
		"auto (the host sends CP437)" : cs[vt.charset];

	if (net >= 0) {
		sprintf(buf, "Connected to %s (%s) port %d.\r\n", host, addr,
			port);
		note(buf);
	} else
		note("No connection.\r\n");
	sprintf(buf, "Escape character is '^%c'%s.\r\n", escc + '@',
		tildes ? " and ~ at the start of a line" : "");
	note(buf);
	if (!win)
		c = vt.charset == CS_ASCII ? "UTF-8 shown as ASCII" :
			"passed on as it comes";
	sprintf(buf, "Mode: %s%s, terminal %s, charset %s.\r\n",
		linemode() ? "line (local echo)" : "character",
		force ? " (set by hand)" : "", ttypes[0], c);
	note(buf);
	sprintf(buf, "Return sends %s%s%s; host binary %s, we binary %s.\r\n",
		crlf ? "CR LF" : "CR NUL", win ? ", Backspace sends " : "",
		win ? (bsdel ? "DEL" : "BS") : "", him[O_BINARY] == YES ? "on" :
		"off", us[O_BINARY] == YES ? "on" : "off");
	note(buf);
}

static char *word(char **s)
{
	char *w;

	while (**s == ' ' || **s == '\t')
		(*s)++;
	w = *s;
	while (**s && **s != ' ' && **s != '\t')
		(*s)++;
	if (**s)
		*(*s)++ = 0;
	return w;
}

static int is(char *w, char *full, int min)
{
	int n = 0;

	while (w[n] && full[n] == w[n])
		n++;
	return !w[n] && n >= min;
}

/* 1: back to the host, 0: stay at the prompt */
static int command(char *s)
{
	static char types[80];
	char *w = word(&s), *a, buf[120];
	int i;

	if (!*w)
		return net >= 0;
	if (is(w, "quit", 1) || is(w, "exit", 2) || is(w, "bye", 3)) {
		tclose();
		quitting = 1;
		return 1;
	}
	if (is(w, "close", 1)) {
		tclose();
		return 0;
	}
	if (is(w, "open", 1) || is(w, "connect", 4)) {
		a = word(&s);
		if (!*a) {
			note("usage: open host [port]\r\n");
			return 0;
		}
		if (net >= 0) {
			sprintf(buf, "?Already connected to %s\r\n", host);
			note(buf);
			return 0;
		}
		if (win) {
			vt_reset();
			r_draw(1);
		}
		return topen(a, word(&s)) == 0 ? 2 : 0;
	}
	if (is(w, "status", 2)) {
		status();
		return 0;
	}
	if (is(w, "?", 1) || is(w, "help", 1)) {
		for (i = 0; help[i]; i++) {
			note(help[i]);
			note("\r\n");
		}
		return 0;
	}
	if (is(w, "escape", 1)) {
		a = word(&s);
		if (*a == '^' && a[1])
			escc = a[1] & 037;
		else if (*a)
			escc = *a;
		sprintf(buf, "Escape character is '^%c'.\r\n", escc + '@');
		note(buf);
		return 0;
	}
	if (is(w, "z", 1)) {
		if (win)
			d_shell();
		else {
			t_close();
			note("Type exit to come back.\r\n");
			system("${SHELL:-/bin/sh}");
			t_open();
		}
		return 1;
	}
	if (is(w, "send", 2)) {
		a = word(&s);
		i = is(a, "ayt", 1) ? AYT : is(a, "brk", 1) || is(a, "break", 1) ?
			BRK : is(a, "ip", 1) ? IP : is(a, "ao", 2) ? AO :
			is(a, "ec", 2) ? EC : is(a, "el", 2) ? EL : is(a, "eof",
			2) ? TEOF : is(a, "nop", 1) ? NOP : is(a, "synch", 2) ?
			DM : is(a, "escape", 2) ? -1 : 0;
		if (!i) {
			note("send: ayt brk ip ao ec el eof nop synch escape\r\n");
			return 0;
		}
		if (i < 0) {
			buf[0] = escc;
			nput(buf, 1);
		} else {
			nputc(IAC);
			nputc(i);
		}
		nflush();
		return 1;
	}
	if (is(w, "term", 1)) {
		a = word(&s);
		if (*a) {
			for (i = 0; *a && i < (int)sizeof types - 1; )
				types[i++] = *a++;
			types[i] = 0;
			ntt = 0;
			for (a = types; *a && ntt < 4; ) {
				ttypes[ntt++] = a;
				while (*a && *a != ',')
					a++;
				if (*a)
					*a++ = 0;
			}
			tti = 0;
		}
		note("Terminal types:");
		for (i = 0; i < ntt; i++) {
			note(" ");
			note(ttypes[i]);
		}
		note(" (the host asks again to get the next one)\r\n");
		return 0;
	}
	if (is(w, "charset", 2)) {
		a = word(&s);
		if (is(a, "auto", 1)) {
			vt.charset = CS_AUTO;
			vt.good = vt.bad = vt.is437 = 0;
		}
		else if (is(a, "utf8", 1) || is(a, "utf-8", 1))
			vt.charset = CS_UTF8;
		else if (is(a, "latin1", 1) || is(a, "iso8859-1", 1))
			vt.charset = CS_LATIN1;
		else if (is(a, "cp437", 1) || is(a, "ibm", 1))
			vt.charset = CS_CP437;
		else if (is(a, "ascii", 2))
			vt.charset = CS_ASCII;
		else
			note("charset: auto utf8 latin1 cp437\r\n");
		status();
		return 0;
	}
	if (is(w, "crlf", 2) || is(w, "crmod", 2)) {
		crlf = !crlf;
		sprintf(buf, "Return sends %s.\r\n", crlf ? "CR LF" : "CR NUL");
		note(buf);
		return 0;
	}
	if (is(w, "bs", 1) || is(w, "backspace", 1)) {
		bsdel = !bsdel;
		sprintf(buf, "Backspace sends %s.\r\n", bsdel ? "DEL" : "BS");
		note(buf);
		return 0;
	}
	if (is(w, "mode", 1)) {
		a = word(&s);
		force = is(a, "line", 1) ? 1 : is(a, "character", 1) ? 2 : 0;
		status();
		return 0;
	}
	if (is(w, "options", 1) || is(w, "debug", 1) || is(w, "negotiate", 1)) {
		showopts = !showopts;
		note(showopts ? "Will show option negotiation.\r\n" :
			"Will not show option negotiation.\r\n");
		return 0;
	}
	if (is(w, "toggle", 1)) {
		a = word(&s);
		if (is(a, "options", 1)) {
			showopts = !showopts;
			return 0;
		}
		if (is(a, "crlf", 1) || is(a, "crmod", 1)) {
			crlf = !crlf;
			return 0;
		}
	}
	note("?Invalid command (? lists them)\r\n");
	return 0;
}

static Vt saved;

/* back to the host's screen after the telnet> prompt */
static void restore(void)
{
	char buf[120];

	if (!win) {
		t_raw(1);
		return;
	}
	vt = saved;
	vt_dirtyall();
	r_draw(1);
	if (net >= 0) {
		sprintf(buf, "%s  -  Exit or ^%c: telnet> prompt", host,
			escc + '@');
		d_prompt(buf);
	}
}

/* the telnet> prompt. fromkey: 1 escape, 2 Help, 0 no connection */
static void cmdmode(int fromkey)
{
	char cmd[120];
	int n, k, r, i;

	if (win) {
		saved = vt;
		vt.wrap = 0;
		vt.at = 0;
		vt.fg = vt.bg = C_DEF;
		vt.g[0] = vt.g[1] = 'B';
		vt.gl = 0;
		vt.insert = 0;
		vt.curon = 1;
		vt.top = 0;
		vt.bot = ROWS - 1;
		vt.origin = 0;
		vt.awrap = 1;
		if (net >= 0) {		/* under the host's screen */
			vt.y = ROWS - 1;
			vt.x = 1;
		}
		d_prompt(net >= 0 ? "telnet> commands (? lists them). Return: "
			"back to the host. Exit: quit" : "telnet> commands (? lists "
			"them), such as: open host. Exit: quit");
	} else
		t_raw(1);
	if (vt.x > 0 || !win)
		note("\r\n");
	if (fromkey == 2)
		command("?");
	for (;;) {
		note("telnet> ");
		n = 0;
		for (;;) {
			k = key(1);
			if (k < 0)
				continue;
			if (k == '~' && n == 0 && fromkey == 3 && net >= 0) {
				/* ~~ sends a ~ */
				n = -2;
				break;
			}
			if (k == K_EXIT) {
				if (n == 0) {
					note("quit\r\n");
					tclose();
					quitting = 1;
					return;
				}
				k = '\r';
			}
			if (k == '\r' || k == '\n')
				break;
			if ((k == 010 || k == 0177) && n > 0) {
				n--;
				note("\b \b");
			} else if (k == 025) {
				while (n > 0) {
					n--;
					note("\b \b");
				}
			} else if (k == 033 && n == 0 && net >= 0) {
				n = -1;
				break;
			} else if (k >= ' ' && k < 0177 &&
				n < (int)sizeof cmd - 1) {
				cmd[n++] = k;
				cmd[n] = 0;
				note(cmd + n - 1);
			}
		}
		if (n == -2) {
			restore();
			bol = 0;
			if (linemode())
				linekey('~');
			else
				sendkey('~');
			return;
		}
		note("\r\n");
		if (n < 0)
			break;
		cmd[n] = 0;
		for (i = 0; i < n && (cmd[i] == ' ' || cmd[i] == '\t'); i++)
			;
		if (i == n && net < 0)
			continue;
		r = command(cmd);
		if (quitting)
			return;
		if (r == 2)		/* a new connection: its own screen */
			return;
		if (r)
			break;
	}
	restore();
}

/* ---- the program ---- */

static void session(void)
{
	char buf[2048];
	int n, k, busy = 0;
	long last = n_ms();

	lost = 0;
	while (net >= 0 && !quitting) {
		/* while the host sends, draw only now and then */
		n = n_wait(net, busy ? 0 : 50);
		busy = 0;
		if (n > 0) {
			n = n_read(net, buf, sizeof buf);
			if (n <= 0) {
				nflush();
				n_close(net);
				net = -1;
				note("\r\nConnection closed by foreign host.\r\n");
				break;
			}
			tnin((unsigned char *)buf, n);
			busy = 1;
		}
		while (net >= 0 && !quitting && (k = key(0)) >= 0)
			dokey(k);
		nflush();
		if (win && (!busy || n_ms() - last >= 200)) {
			r_draw(0);
			last = n_ms();
		}
	}
	if (lost)
		note("\r\nConnection lost.\r\n");
	if (win)
		r_draw(0);
}

/* after the host closed: 1 for the telnet> prompt, 0 to quit */
static int afterclose(void)
{
	int k;

	if (interactive)
		return 1;
	if (!win)
		return 0;		/* as the old telnet */
	d_prompt("Connection closed. Return: telnet> prompt. Other keys: "
		"close window");
	while ((k = key(1)) < 0)
		;
	return k == '\r' || k == '\n';
}

static void bye(int sig)
{
	if (net >= 0)
		n_close(net);
	if (win)
		d_close();
	else
		t_close();
	exit(sig ? 1 : 0);
}

static char *prog = "telnetu";	/* our name, from argv[0] */

static void usage(void)
{
	fprintf(stderr, "usage: %s [-t] [-a] [-e c] [-T type,...] "
		"[-C charset] [host [port]]\n", prog);
	exit(2);
}

int main(int argc, char **argv)
{
	char *fontfile = "/usr/lib/wfont/system.8.ft", *t, *h = 0, *p = 0;
	int tty = 0, i, n, cs = CS_AUTO;
	static char types[80];

	if (argc > 0 && argv[0] && argv[0][0]) {
		prog = argv[0];
		for (t = argv[0]; *t; t++)
			if (*t == '/')
				prog = t + 1;
	}
	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		t = argv[i];
		if (t[1] == 't')
			tty = 1;
		else if (t[1] == 'a') {
			cs = CS_ASCII;	/* for a terminal, not a window */
			tty = 1;
		}
		else if (t[1] == 'e' && i + 1 < argc) {
			t = argv[++i];
			escc = t[0] == '^' && t[1] ? t[1] & 037 : t[0];
		} else if (t[1] == 'T' && i + 1 < argc) {
			t = argv[++i];
			for (n = 0; t[n] && n < (int)sizeof types - 1; n++)
				types[n] = t[n] >= 'a' && t[n] <= 'z' ?
					t[n] - 32 : t[n];
			types[n] = 0;
			ntt = 0;
			for (t = types; *t && ntt < 4; ) {
				ttypes[ntt++] = t;
				while (*t && *t != ',')
					t++;
				if (*t)
					*t++ = 0;
			}
		} else if (t[1] == 'C' && i + 1 < argc) {
			t = argv[++i];
			cs = t[0] == 'u' ? CS_UTF8 : t[0] == 'l' ? CS_LATIN1 :
				t[0] == 'c' ? CS_CP437 : CS_AUTO;
		} else if (t[1] == 'f' && i + 1 < argc)
			fontfile = argv[++i];
		else if (t[1] == 'V') {
			printf("%s %s\n", prog, VERSION);
			return 0;
		} else
			usage();
	}
	if (i < argc)
		h = argv[i++];
	if (i < argc)
		p = argv[i++];
	if (i < argc)
		usage();

	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, bye);
	signal(SIGTERM, bye);
	vt_init();
	vt.charset = cs;
	if (!tty && r_init(fontfile) == 0 && d_open() == 0)
		win = 1;
	else {
		win = 0;
		t_open();
		if (!ttypes[0] || (ntt == 2 && ttypes[0][0] == 'X')) {
			/* no emulation: give the host this terminal's type */
			static char tt[40];
			char *e = getenv("TERM");

			if (e && *e) {
				for (i = 0; e[i] && i < (int)sizeof tt - 1; i++)
					tt[i] = e[i] >= 'a' && e[i] <= 'z' ?
						e[i] - 32 : e[i];
				tt[i] = 0;
				ttypes[0] = tt;
				ntt = 1;
			}
		}
	}
	if (win) {
		d_title("TelnetU");
		r_draw(1);
	}
	interactive = !h;
	if (h && topen(h, p) < 0 && !win)
		bye(1);
	while (!quitting) {
		if (net >= 0) {
			session();
			if (quitting || !afterclose())
				break;
		}
		cmdmode(0);
	}
	bye(0);
	return 0;
}
