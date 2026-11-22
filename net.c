/* net.c - TCP for telnet, and a small DNS client.
 *
 * On the 3B1 the sockets come from WIN/TCP (-lnet). Its gethostbyname()
 * reads /etc/hosts only, so a name that is not there goes to the name
 * server: "nameserver" in /etc/resolv.conf, else 10.0.2.3 (the one QEMU
 * and FreeBee give a guest on slirp). Its select() takes a long as the
 * mask, and works on sockets only.
 */
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#ifdef __APPLE__
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#else
#include <sys/times.h>
extern int errno;
extern unsigned long inet_addr();
extern char *inet_ntoa();
extern long times();
#endif
#include "tn.h"

#define NS_DEFAULT "10.0.2.3"

static void zero(char *p, int n)
{
	while (n-- > 0)
		*p++ = 0;
}

static void copy(char *d, char *s, int n)
{
	while (n-- > 0)
		*d++ = *s++;
}

static int prefix(char *s, char *p)
{
	while (*p)
		if (*s++ != *p++)
			return 0;
	return 1;
}

static char *errstr(int e)
{
	static char buf[32];

	switch (e) {
	case ECONNREFUSED: return "Connection refused";
	case ETIMEDOUT: return "Connection timed out";
	case EINTR: return "Connection timed out";
	case ENETUNREACH: return "Network is unreachable";
	case EHOSTUNREACH: return "No route to host";
	case EHOSTDOWN: return "Host is down";
	case ECONNRESET: return "Connection reset by peer";
	case EADDRNOTAVAIL: return "Can't assign requested address";
	case ENETDOWN: return "Network is down";
	case ENOBUFS: return "No buffer space available";
	}
	sprintf(buf, "error %d", e);
	return buf;
}

/* ---- the clock, for drawing now and then ---- */

long n_ms(void)
{
#ifdef __APPLE__
	struct timeval tv;

	gettimeofday(&tv, 0);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
#else
	struct tms tb;
	long t = times(&tb);		/* 60 a second */

	return t / 60 * 1000 + t % 60 * 1000 / 60;
#endif
}

/* ---- waiting for data ---- */

int n_wait(int s, int ms)
{
	struct timeval tv;
#ifdef __APPLE__
	fd_set rd;

	FD_ZERO(&rd);
	FD_SET(s, &rd);
#else
	long rd = 1L << s;
#endif
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (long)(ms % 1000) * 1000L;
	if (s < 0)
		return 0;
#ifdef __APPLE__
	return select(s + 1, &rd, (fd_set *)0, (fd_set *)0, &tv);
#else
	return select(s + 1, &rd, (long *)0, (long *)0, &tv);
#endif
}

/* ---- DNS ---- */

static char *nameserver(void)
{
	static char ns[40];
	char line[128], *p;
	FILE *f;
	int i;

	f = fopen("/etc/resolv.conf", "r");
	if (f) {
		while (fgets(line, sizeof line, f)) {
			p = line;
			while (*p == ' ' || *p == '\t')
				p++;
			if (!prefix(p, "nameserver"))
				continue;
			p += 10;
			while (*p == ' ' || *p == '\t')
				p++;
			for (i = 0; (p[i] >= '0' && p[i] <= '9') || p[i] == '.'; i++)
				if (i < (int)sizeof ns - 1)
					ns[i] = p[i];
			ns[i < (int)sizeof ns ? i : (int)sizeof ns - 1] = 0;
			if (ns[0]) {
				fclose(f);
				return ns;
			}
		}
		fclose(f);
	}
	return NS_DEFAULT;
}

static int skipname(unsigned char *r, int len, int p)
{
	while (p < len) {
		if (r[p] == 0)
			return p + 1;
		if ((r[p] & 0xc0) == 0xc0)
			return p + 2;
		p += r[p] + 1;
	}
	return len + 1;
}

/* the address of name, from the name server: 0, or -1 */
static int dns(char *name, unsigned long *ip)
{
	unsigned char q[300], r[600];
	struct sockaddr_in sa, from;
	int s, n, len, i, try, id, qd, an, p, type, class, rdlen;
	int fromlen;
	char *ns = nameserver();

	id = (getpid() * 31 + (int)n_ms()) & 0xffff;
	zero((char *)q, 12);
	q[0] = id >> 8;
	q[1] = id;
	q[2] = 1;			/* recursion desired */
	q[5] = 1;			/* one question */
	n = 12;
	while (*name) {
		for (i = 0; name[i] && name[i] != '.'; i++)
			;
		if (i == 0 || i > 63 || n + i + 6 > (int)sizeof q)
			return -1;
		q[n++] = i;
		copy((char *)q + n, name, i);
		n += i;
		name += i;
		if (*name == '.')
			name++;
	}
	q[n++] = 0;
	q[n++] = 0;
	q[n++] = 1;			/* A */
	q[n++] = 0;
	q[n++] = 1;			/* IN */

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;
	zero((char *)&sa, sizeof sa);
	sa.sin_family = AF_INET;
	bind(s, (struct sockaddr *)&sa, sizeof sa);
	sa.sin_port = htons(53);
	sa.sin_addr.s_addr = inet_addr(ns);
	for (try = 0; try < 3; try++) {
		if (sendto(s, (char *)q, n, 0, (struct sockaddr *)&sa,
			sizeof sa) != n)
			break;
		while (n_wait(s, 2000 + try * 1000) > 0) {
			fromlen = sizeof from;
			len = recvfrom(s, (char *)r, sizeof r, 0,
				(struct sockaddr *)&from, &fromlen);
			if (len < 12 || r[0] != q[0] || r[1] != q[1] ||
				!(r[2] & 0x80))
				continue;
			if ((r[3] & 15) != 0) {		/* no such name */
				close(s);
				return -1;
			}
			qd = r[4] << 8 | r[5];
			an = r[6] << 8 | r[7];
			p = 12;
			while (qd-- > 0)
				p = skipname(r, len, p) + 4;
			while (an-- > 0 && p < len) {
				p = skipname(r, len, p);
				if (p + 10 > len)
					break;
				type = r[p] << 8 | r[p + 1];
				class = r[p + 2] << 8 | r[p + 3];
				rdlen = r[p + 8] << 8 | r[p + 9];
				p += 10;
				if (type == 1 && class == 1 && rdlen == 4 &&
					p + 4 <= len) {
					copy((char *)ip, (char *)r + p, 4);
					close(s);
					return 0;
				}
				p += rdlen;
			}
			close(s);
			return -1;
		}
	}
	close(s);
	return -1;
}

/* ---- the connection ---- */

static void onalarm(int sig)
{
	signal(SIGALRM, onalarm);
}

/* connect to host, port: the socket, or -1 and err says why. addr: the
 * address, as text */
int n_open(char *host, int port, char *err, char *addr)
{
	struct sockaddr_in sa;
	struct hostent *hp;
	unsigned long ip;
	int s, r, e;
#ifdef __APPLE__
	struct sigaction act;
#endif

	ip = inet_addr(host);
	if (ip == (unsigned long)-1 || ip == 0xffffffffUL) {
		hp = gethostbyname(host);
		if (hp && hp->h_length == 4)
			copy((char *)&ip, hp->h_addr, 4);
		else if (dns(host, &ip) < 0) {
			sprintf(err, "%s: unknown host", host);
			return -1;
		}
	}
	zero((char *)&sa, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	copy((char *)&sa.sin_addr, (char *)&ip, 4);
	sprintf(addr, "%s", inet_ntoa(sa.sin_addr));

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		sprintf(err, "socket: %s", errstr(errno));
		return -1;
	}
#ifdef __APPLE__
	act.sa_handler = onalarm;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;		/* no restart: connect() stops */
	sigaction(SIGALRM, &act, 0);
#else
	signal(SIGALRM, onalarm);
#endif
	alarm(30);
	r = connect(s, (struct sockaddr *)&sa, sizeof sa);
	e = errno;
	alarm(0);
	if (r < 0) {
		close(s);
		sprintf(err, "%s: %s", addr, errstr(e));
		return -1;
	}
	return s;
}

int n_read(int s, char *buf, int n)
{
	int r;

	do
		r = recv(s, buf, n, 0);
	while (r < 0 && errno == EINTR);
	return r;
}

int n_write(int s, char *buf, int n)
{
	int r, done = 0;

	while (done < n) {
		r = send(s, buf + done, n - done, 0);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		done += r;
	}
	return done;
}

void n_close(int s)
{
	close(s);
}
