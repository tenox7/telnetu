/* w3b1.c - telnet's window on the AT&T UNIX PC (3B1).
 *
 * A window of its own without a border, 720 x 288 pixels: all of the
 * screen between the top line and the prompt line, 80 x 24 cells of the
 * system font's size. The bitmap goes in with wrastop(). Keys come from
 * tam's wgetc() in keypad mode, so the 3B1's own keys come with names:
 * Exit gives the telnet> prompt, Help its help, Rfrsh draws it all again.
 * A click comes as the Mouse key; it goes to the host only if the host
 * asked for mouse reports.
 */
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <termio.h>
#include <sys/types.h>
#include <sys/window.h>
#include <sys/rastop.h>
#include <tam.h>
#include "tn.h"

extern char *getenv();

int mx, my, mb;

static int wn = -1;
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

int d_open(void)
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
	wn = wcreate(1, 0, 24, 80, NBORDER);
	if (wn < 0) {
		wexit(1);
		return -1;
	}
	ioctl(wn, WIOCGETD, &uw);
	uw.uw_x = 0;
	uw.uw_y = 12;			/* under the top line */
	uw.uw_width = COLS * CW;
	uw.uw_height = PXH;
	uw.uw_uflags = NBORDER;
	ioctl(wn, WIOCSETD, &uw);
	wprintf(wn, "\033[=1C");	/* no text cursor: we draw our own */
	keypad(wn, 1);
	if (ioctl(wn, TCGETA, &tsave) == 0)
		tsaved = 1;
	setwait(0);
	um.um_flags = MSDOWN;
	um.um_x = um.um_y = um.um_w = um.um_h = 0;
	um.um_icon = 0;
	wsetmouse(wn, &um);
	/* keys of our window do not make signals; the window we came from
	 * must not stop us either */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	return 0;
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

	setwait(wait);
	k = wgetc(wn);
	if (k < 0)
		return -1;
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
	case Cancl: case s_Cancl: return 033;
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
			uw.uw_y = 12;
			uw.uw_width = COLS * CW;
			uw.uw_height = PXH;
			uw.uw_uflags = NBORDER;
			ioctl(0, WIOCSETD, &uw);
		}
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		write(1, "Type exit to go back to telnet.\r\n", 33);
		execl(sh, "sh", (char *)0);
		_exit(1);
	}
	while ((w = wait(&st)) != pid && w != -1)
		;
	ioctl(wn, WIOCSELECT, 0);
	return 0;
}
