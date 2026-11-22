/* w3b1.c - telnet's window on the AT&T UNIX PC (3B1).
 *
 * A window of its own. Without a border it is in the middle of the
 * screen, and at its biggest it fills all of it between the top line and
 * the prompt line: 720 x 288 pixels, 80 x 24 cells of the system font.
 * With a border the user can move it (the box at the top left) and
 * resize it (the box at the bottom right); then the terminal takes the
 * new size, in whole cells, and the host gets it. The bitmap goes in with
 * wrastop(). Keys come from tam's wgetc() in keypad mode, so the 3B1's
 * own keys come with names: Exit gives the telnet> prompt, Help its help,
 * Rfrsh draws it all again. A click comes as the Mouse key; it goes to
 * the host only if the host asked for mouse reports.
 */
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <termio.h>
#include <sys/types.h>
#include <sys/window.h>
#include <sys/rastop.h>
#include <errno.h>
#include <tam.h>
#include "tn.h"

extern int errno;

extern char *getenv();

int mx, my, mb;
int d_cols, d_rows;

/* the area of the windows: 720 x 288 under the top line; a border takes
 * 34 x 40 pixels of it (the driver's numbers) */
#define AREAW 720
#define AREAH 288
#define AREAY 12
#define BORDW 34
#define BORDH 40
/* the boxes on the border: resize, ? (it gives the Help key) and X (it
 * gives the Cancl key, which closes telnetu). The Esc key sends ESC. */
#define BFLAGS (BORDRESIZE | BORDHELP | BORDCANCEL)

static int wn = -1;
static int bord;		/* the window has a border */
static int pw, ph;		/* its size in pixels, as we set it */
static int placed;		/* it has its place */
static struct termio tsave;
static int tsaved, twait = -1;

static void setwait(int wait)
{
	struct termio t;

	if (wait == twait || !tsaved)
		return;
	t = tsave;
	t.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON | IXOFF | ISTRIP);
	t.c_lflag &= ~(ICANON | ECHO | ISIG);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = wait ? 1 : 0;	/* wait at most 0.1 s, or not */
	ioctl(wn, TCSETA, &t);
	twait = wait;
}

/* ---- the host box ----
 * With no host, telnetu asks for one in a box of its own, before its
 * window opens. The Office items used the TWG form (shform) for this, but
 * there only the Enter key ends the form: Return goes to the next field,
 * and the Return key of a PC is the 3B1's Return. Here both connect.
 * Cmd lists the known hosts, as the TWG form did: its list
 * (/usr/ethernet/ua/HostTable), else the names in /etc/hosts. The list
 * is ours too: in tam's menu() Return also goes to the next item. */

#define HB_ROWS 4		/* the box, in cells of the system font */
#define HB_COLS 66
#define HB_FCOL 37		/* the field: its column and width */
#define HB_FLEN 27
#define HB_HOSTS 100
#define HL_ROWS 16		/* the list: hosts it shows at a time */

static int hb = -1;
static char *hbname[HB_HOSTS];
static char hbpool[3000];

/* a window for the box or the list: keys one at a time, with names */
static int hbwin(int row, int col, int rows, int cols, char *label)
{
	struct termio t;
	struct umdata um;
	int w = wcreate(row, col, rows, cols, BORDHELP | BORDCANCEL);

	if (w < 0)
		return -1;
	wlabel(w, label);
	wuser(w, label);
	keypad(w, 1);
	if (ioctl(w, TCGETA, &t) == 0) {
		t.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON | IXOFF | ISTRIP);
		t.c_lflag &= ~(ICANON | ECHO | ISIG);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		ioctl(w, TCSETA, &t);
	}
	um.um_flags = MSDOWN;
	um.um_x = um.um_y = um.um_w = um.um_h = 0;
	um.um_icon = 0;
	wsetmouse(w, &um);
	return w;
}

/* a key from a box window; -1: closed */
static int hbkey(int w)
{
	int k;

	for (;;) {
		k = wgetc(w);
		if (k >= 0 || errno != EINTR)
			return k;
	}
}

/* the known hosts into hbname; returns how many */
static int hbload(void)
{
	static int n = -1;
	static char *files[] = { "/usr/ethernet/ua/HostTable", "/etc/hosts" };
	char line[200], *p, *q;
	FILE *f;
	int i, used = 0;

	if (n >= 0)
		return n;
	n = 0;
	for (i = 0; i < 2 && n == 0; i++) {
		if (!(f = fopen(files[i], "r")))
			continue;
		while (n < HB_HOSTS && fgets(line, sizeof line, f)) {
			p = line;
			if (i == 0) {	/* Name=host */
				if (p[0] != 'N' || p[1] != 'a' || p[2] != 'm' ||
				    p[3] != 'e' || p[4] != '=')
					continue;
				p += 5;
			} else {	/* address name ... */
				while (*p == ' ' || *p == '\t')
					p++;
				if (*p == '#')
					continue;
				while (*p && *p != ' ' && *p != '\t' && *p != '\n')
					p++;
			}
			while (*p == ' ' || *p == '\t')
				p++;
			for (q = p; *q && *q != ' ' && *q != '\t' && *q != '\n' &&
			    *q != '#'; q++)
				;
			if (q == p || used + (q - p) + 1 > (int)sizeof hbpool)
				continue;
			hbname[n++] = hbpool + used;
			while (p < q)
				hbpool[used++] = *p++;
			hbpool[used++] = 0;
		}
		fclose(f);
	}
	return n;
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* the list of known hosts: the one chosen goes into buf */
static int hblist(char *buf, int len)
{
	char line[60];
	int n = hbload(), w, i, j, k, cur = 0, top = 0, rows, width = 20;
	int x, y, b, r;

	if (n == 0) {
		wprompt(hb, "No known hosts in /usr/ethernet/ua/HostTable "
			"or /etc/hosts");
		return 0;
	}
	for (i = 0; i < n; i++) {
		for (j = 0; hbname[i][j]; j++)
			;
		if (j + 4 > width)
			width = j + 4;
	}
	if (width > (int)sizeof line - 1)
		width = sizeof line - 1;
	rows = n < HL_ROWS ? n : HL_ROWS;
	w = hbwin(3, (80 - width) / 2 + 8, rows + 2, width, "Known Hosts");
	if (w < 0)
		return 0;
	wprintf(w, "\033[=1C");		/* no text cursor: a bar shows it */
	wprompt(w, "Choose a host: the arrows, then Return. Exit: back");
	for (;;) {
		if (cur < top)
			top = cur;
		if (cur >= top + rows)
			top = cur - rows + 1;
		for (i = 0; i < rows; i++) {
			char *h = hbname[top + i];

			line[0] = ' ';
			for (j = 1; j < width; j++)
				line[j] = *h ? *h++ : ' ';
			line[width] = 0;
			wgoto(w, i + 1, 0);
			if (top + i == cur)
				wputs(w, "\033[7m");
			wputs(w, line);
			if (top + i == cur)
				wputs(w, "\033[0m");
		}
		k = hbkey(w);
		if (k == '\r' || k == '\n' || k == Enter)
			break;
		if (k < 0 || k == Exit || k == s_Exit || k == Cancl ||
		    k == s_Cancl) {
			cur = -1;
			break;
		}
		if (k == Up && cur > 0)
			cur--;
		else if (k == Down && cur < n - 1)
			cur++;
		else if (k == Home || k == Beg)
			cur = 0;
		else if (k == End)
			cur = n - 1;
		else if (k == Page || k == Next)
			cur = cur + rows < n ? cur + rows : n - 1;
		else if (k == s_Page || k == Prev)
			cur = cur > rows ? cur - rows : 0;
		else if (k == Mouse) {	/* a click on a host chooses it */
			wreadmouse(w, &x, &y, &b, &r);
			if ((r & MSDOWN) && b) {
				i = top + y / 12 - 1;
				if (y >= 12 && i < top + rows && i < n) {
					cur = i;
					break;
				}
			}
		} else if (k > ' ' && k < 0177) {	/* the next with that letter */
			for (i = 1; i <= n; i++)
				if (lower(hbname[(cur + i) % n][0]) == lower(k)) {
					cur = (cur + i) % n;
					break;
				}
		}
	}
	wdelete(w);
	if (cur < 0)
		return 0;
	for (i = 0; hbname[cur][i] && i < len - 1; i++)
		buf[i] = hbname[cur][i];
	buf[i] = 0;
	return 1;
}

static void hbfield(char *buf, int n)
{
	char vis[HB_FLEN + 1];
	int i, s = n >= HB_FLEN ? n - HB_FLEN + 1 : 0;

	for (i = 0; i < HB_FLEN; i++)
		vis[i] = s + i < n ? buf[s + i] : ' ';
	vis[HB_FLEN] = 0;
	wgoto(hb, 1, HB_FCOL);
	wputs(hb, "\033[7m");
	wputs(hb, vis);
	wputs(hb, "\033[0m");
	wgoto(hb, 1, HB_FCOL + n - s);
}

/* returns 1 with a host (a port may follow) in buf, 0 with no host,
 * -1 if the user closed the box */
static int askhost(char *buf, int len)
{
	int k, n = 0, r;

	buf[0] = 0;
	hb = hbwin(4, (80 - HB_COLS) / 2, HB_ROWS, HB_COLS, "TelnetU");
	if (hb < 0)
		return 0;		/* no box: the telnet> prompt */
	wgoto(hb, 1, 2);
	wputs(hb, "Which host do you wish to access?");
	wgoto(hb, 3, 2);
	wputs(hb, "Return: connect    Cmd: known hosts    Exit: close");
	wprompt(hb, "Type the name of a host (a port may follow), then Return");
	for (;;) {
		hbfield(buf, n);
		k = hbkey(hb);
		if (k == '\r' || k == '\n' || k == Enter) {
			r = 1;
			break;
		}
		if (k < 0 || k == Exit || k == s_Exit || k == Cancl ||
		    k == s_Cancl) {
			r = -1;
			break;
		}
		if (k == Cmd || k == s_Cmd) {
			if (hblist(buf, len)) {
				r = 1;
				break;
			}
			wprompt(hb, "Type the name of a host (a port may "
				"follow), then Return");
		} else if (k == Help || k == s_Help)
			wprompt(hb, "A name, such as sdf.org, or an address and "
				"a port: 10.0.2.2 2323");
		else if ((k == 010 || k == 0177) && n > 0)
			buf[--n] = 0;
		else if (k == 025) {
			n = 0;
			buf[0] = 0;
		} else if (k >= ' ' && k < 0177 && n < len - 1) {
			buf[n++] = k;
			buf[n] = 0;
		}
	}
	wdelete(hb);
	hb = -1;
	for (n = 0; buf[n] == ' '; n++)
		;
	if (r > 0 && !buf[n])
		r = 0;
	return r;
}

int d_open(char *ask, int len)
{
	struct uwdata uw;
	struct umdata um;
	int fd;

	if (ioctl(0, WIOCGETD, &uw) < 0)
		return -1;		/* not a window: a plain terminal */
	/* a new window as 0, 1 and 2, as the games do: tam's first
	 * wcreate() takes the window on 0, and that must not be the one we
	 * came from. At exit it closes, and the 3B1 puts back what was
	 * under it. */
	fd = open("/dev/window", O_RDWR);
	if (fd < 0)
		return -1;
	close(0);
	dup(fd);
	close(1);
	dup(fd);
	close(2);
	dup(fd);
	close(fd);
	winit();
	/* keys of our windows do not make signals; the window we came from
	 * must not stop us either. SIGWIND (a window changed) we do not
	 * need: d_key() looks at the size itself. */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGWIND, SIG_IGN);
	if (ask && askhost(ask, len) < 0) {
		wexit(0);		/* the user closed the box */
		exit(0);
	}
	wn = wcreate(1, 0, 24, 80, NBORDER);
	if (wn < 0) {
		wexit(1);
		return -1;
	}
	/* d_setup() gives it its size; a new size from the user comes as
	 * K_RESIZE, from d_key(), which looks at the size each time */
	wprintf(wn, "\033[=1C");	/* no text cursor: we draw our own */
	keypad(wn, 1);
	if (ioctl(wn, TCGETA, &tsave) == 0)
		tsaved = 1;
	setwait(0);
	um.um_flags = MSDOWN;
	um.um_x = um.um_y = um.um_w = um.um_h = 0;
	um.um_icon = 0;
	wsetmouse(wn, &um);
	return 0;
}

/* the most cells that fit on the screen, with a border or without */
void d_fit(int b, int *c, int *r)
{
	*c = (AREAW - (b ? BORDW : 0)) / CW;
	*r = (AREAH - (b ? BORDH : 0)) / CH;
	if (*c > MAXCOLS)
		*c = MAXCOLS;
	if (*r > MAXROWS)
		*r = MAXROWS;
}

/* the window's border and size in cells; a window with a border that
 * stays one stays where the user put it, if it still fits there */
int d_setup(int b, int c, int r)
{
	struct uwdata uw;
	int x, y, fw, fh;

	if (wn < 0 || ioctl(wn, WIOCGETD, &uw) < 0)
		return -1;
	pw = c * CW;
	ph = r * CH;
	fw = pw + (b ? BORDW : 0);
	fh = ph + (b ? BORDH : 0);
	if (b && bord && placed) {
		x = uw.uw_x;
		y = uw.uw_y;
		if (x + fw > AREAW)
			x = AREAW - fw;
		if (y + fh > AREAY + AREAH)
			y = AREAY + AREAH - fh;
	} else {
		x = (AREAW - fw) / 2;
		y = AREAY + (AREAH - fh) / 2;
	}
	uw.uw_x = x < 0 ? 0 : x;
	uw.uw_y = y < AREAY ? AREAY : y;
	uw.uw_width = pw;
	uw.uw_height = ph;
	uw.uw_uflags = b ? BFLAGS : NBORDER;
	if (ioctl(wn, WIOCSETD, &uw) < 0)
		return -1;
	/* what the driver made of it: in whole cells */
	if (ioctl(wn, WIOCGETD, &uw) == 0 && (uw.uw_width != pw ||
		uw.uw_height != ph)) {
		c = uw.uw_width / CW;
		r = uw.uw_height / CH;
		pw = c * CW;
		ph = r * CH;
		uw.uw_width = pw;
		uw.uw_height = ph;
		uw.uw_uflags = b ? BFLAGS : NBORDER;
		ioctl(wn, WIOCSETD, &uw);
	}
	bord = b;
	placed = 1;
	d_cols = c;
	d_rows = r;
	return 0;
}

/* has the user resized the window? Then its new size, in whole cells */
static int resized(void)
{
	struct uwdata uw;
	int c, r, mc, mr;

	if (!bord || ioctl(wn, WIOCGETD, &uw) < 0 ||
		(uw.uw_width == pw && uw.uw_height == ph))
		return 0;
	d_fit(1, &mc, &mr);
	c = uw.uw_width / CW;
	r = uw.uw_height / CH;
	c = c < MINCOLS ? MINCOLS : c > mc ? mc : c;
	r = r < MINROWS ? MINROWS : r > mr ? mr : r;
	pw = c * CW;
	ph = r * CH;
	if (uw.uw_width != pw || uw.uw_height != ph) {
		uw.uw_width = pw;
		uw.uw_height = ph;
		uw.uw_uflags = BFLAGS;
		ioctl(wn, WIOCSETD, &uw);
	}
	d_cols = c;
	d_rows = r;
	return 1;
}

void d_close(void)
{
	if (wn < 0)
		return;
	if (tsaved)
		ioctl(wn, TCSETA, &tsave);
	wdelete(wn);
	wn = -1;
	wexit(0);
}

void d_blit(int y, int h)
{
	if (wn >= 0 && h > 0)
		wrastop(wn, &bm[0][0], WPR * 2, (unsigned short *)0, 0, 0, y,
			0, y, COLS * CW, h, SRCSRC, DSTSRC, (unsigned short *)0);
}

int d_key(int wait)
{
	int k, x, y, b, r;

	if (resized())
		return K_RESIZE;
	setwait(wait);
	k = wgetc(wn);
	if (k < 0)
		return resized() ? K_RESIZE : -1;
	switch (k) {
	case Up: return K_UP;
	case Down: return K_DOWN;
	case Forward: return K_RIGHT;
	case Back: return K_LEFT;
	case Home: case Beg: case s_Home: case s_Beg: return K_HOME;
	case End: case s_End: return K_END;
	case Page: case Next: return K_PGDN;
	case s_Page: case Prev: return K_PGUP;
	case InputMode: case s_InputMode: return K_INS;
	case DleteChar: case s_DleteChar: return K_DEL;
	case BackTab: return K_BTAB;
	case Exit: case s_Exit: case Cmd: case s_Cmd: return K_EXIT;
	case Help: case s_Help: return K_HELP;
	case Rfrsh: return K_REDRAW;
	case Cancl: case s_Cancl: return K_CLOSE;
	case Enter: return '\r';
	case Break: return K_BRK;	/* the host gets a telnet BRK */
	case Mouse:
		wreadmouse(wn, &x, &y, &b, &r);
		if (!(r & MSDOWN) || !b)
			return -1;
		mx = x / CW;
		my = y / CH;
		if (mx >= COLS)
			mx = COLS - 1;
		if (my >= ROWS)
			my = ROWS - 1;
		mb = b & 4 ? 0 : b & 2 ? 1 : 2;	/* left, middle, right */
		return K_MOUSE;
	}
	if (k >= F1 && k <= F8)
		return K_F1 + k - F1;
	if (k >= s_F1 && k <= s_F8)
		return K_F1 + 8 + k - s_F1;
	if (k >= 0200)
		return -1;		/* other 3B1 keys */
	return k;
}

void d_title(char *s)
{
	if (wn >= 0) {
		wlabel(wn, s);
		wuser(wn, s);
	}
}

void d_prompt(char *s)
{
	if (wn >= 0)
		wprompt(wn, s);
}

void d_bell(void)
{
	if (wn >= 0)
		wprintf(wn, "\007");
}

/* a shell in a window of its own; telnet waits until it ends */
int d_shell(void)
{
	struct uwdata uw;
	char *sh = getenv("SHELL");
	int pid, w, st;

	if (!sh || !*sh)
		sh = "/bin/sh";
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		setpgrp();
		close(0);
		close(1);
		close(2);
		if (open("/dev/window", O_RDWR) != 0)
			_exit(1);
		dup(0);
		dup(0);
		if (ioctl(0, WIOCGETD, &uw) == 0) {
			uw.uw_x = 0;
			uw.uw_y = AREAY;
			uw.uw_width = AREAW;
			uw.uw_height = AREAH;
			uw.uw_uflags = NBORDER;
			ioctl(0, WIOCSETD, &uw);
		}
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGWIND, SIG_DFL);
		write(1, "Type exit to go back to telnet.\r\n", 33);
		execl(sh, "sh", (char *)0);
		_exit(1);
	}
	while ((w = wait(&st)) != pid && w != -1)
		;
	ioctl(wn, WIOCSELECT, 0);
	return 0;
}
