/* wnone.c - no window: telnet on a machine with terminals only, such as
 * the 3B2. d_open() fails, so telnet works as on a plain terminal
 * (ttyb.c): in a DMD 5620 layers window, on a serial line, in rlogin.
 */
#include "tn.h"

int mx, my, mb;
int d_cols, d_rows;

int d_open(char *ask, int len)
{
	return -1;
}

void d_fit(int bord, int *cols, int *rows)
{
}

int d_setup(int bord, int cols, int rows)
{
	return -1;
}

void d_close(void)
{
}

void d_blit(int y, int h)
{
}

int d_key(int wait)
{
	return -1;
}

void d_title(char *s)
{
}

void d_prompt(char *s)
{
}

void d_bell(void)
{
}

int d_shell(void)
{
	return -1;
}
