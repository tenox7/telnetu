#!/usr/bin/env python3
"""tortured.py - a telnet server that tortures telnet clients.

    python3 tortured.py [-p PORT] [-b ADDR] [-P PAGE] [-r FILE]

It listens on 127.0.0.1:PORT (default 47423). A 3B1 guest on slirp
reaches it as 10.0.2.2:PORT. It asks for odd options, logs what the
client answers, then shows a menu of test pages (a key each). -P PAGE
shows that page at once (for scripted tests), -r FILE replays a capture.
"""
import argparse, os, random, socket, socketserver, sys, time

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
NAMES = {0: 'BINARY', 1: 'ECHO', 3: 'SGA', 5: 'STATUS', 6: 'TM',
         24: 'TTYPE', 31: 'NAWS', 32: 'TSPEED', 33: 'LFLOW',
         34: 'LINEMODE', 36: 'ENVIRON', 39: 'NEW-ENVIRON', 42: 'CHARSET'}
CMDS = {DO: 'DO', DONT: 'DONT', WILL: 'WILL', WONT: 'WONT'}
E = '\033'


def oname(o):
    return NAMES.get(o, str(o))


# ---- pages ----

def page_boxes():
    o = [E + '[2J' + E + '[H' + 'Box drawing U+2500-257F:\r\n']
    for r in range(8):
        o.append('  ' + ' '.join(chr(0x2500 + r * 16 + c) for c in range(16)) + '\r\n')
    o.append('Blocks U+2580-259F:  ' + ''.join(chr(c) for c in range(0x2580, 0x25a0)) + '\r\n')
    o.append('Shades: ' + '\u2591' * 8 + '\u2592' * 8 + '\u2593' * 8 + '\u2588' * 8 + '\r\n')
    o.append('\u250c\u2500\u2500\u2500\u252c\u2500\u2500\u2500\u2510 \u2554\u2550\u2550\u2550\u2566\u2550\u2550\u2550\u2557 '
             '\u250f\u2501\u2501\u2501\u2533\u2501\u2501\u2501\u2513 \u2552\u2550\u2550\u2564\u2550\u2550\u2555 \u2553\u2500\u2500\u2565\u2500\u2500\u2556 \u256d\u2500\u2500\u256e\r\n')
    o.append('\u2502 a \u2502 b \u2502 \u2551 a \u2551 b \u2551 \u2503 a \u2503 b \u2503 \u2502ab\u2502cd\u2502 \u2551ab\u2551cd\u2551 \u2502  \u2502\r\n')
    o.append('\u251c\u2500\u2500\u2500\u253c\u2500\u2500\u2500\u2524 \u2560\u2550\u2550\u2550\u256c\u2550\u2550\u2550\u2563 '
             '\u2523\u2501\u2501\u2501\u254b\u2501\u2501\u2501\u252b \u255e\u2550\u2550\u256a\u2550\u2550\u2561 \u255f\u2500\u2500\u256b\u2500\u2500\u2562 \u2570\u2500\u2500\u256f\r\n')
    o.append('\u2514\u2500\u2500\u2500\u2534\u2500\u2500\u2500\u2518 \u255a\u2550\u2550\u2550\u2569\u2550\u2550\u2550\u255d '
             '\u2517\u2501\u2501\u2501\u253b\u2501\u2501\u2501\u251b \u2558\u2550\u2550\u2567\u2550\u2550\u255b \u2559\u2500\u2500\u2568\u2500\u2500\u255c \u2571\u2572\u2573\r\n')
    # braille sine wave, 2 x 4 dots a cell
    import math
    W, H = 60, 4
    cells = [[0] * W for _ in range(H)]
    bit = {(0, 0): 1, (0, 1): 2, (0, 2): 4, (1, 0): 8, (1, 1): 16, (1, 2): 32, (0, 3): 64, (1, 3): 128}
    for x in range(W * 2):
        y = int((math.sin(x / 12.0) + 1) / 2 * (H * 4 - 1))
        cells[y // 4][x // 2] |= bit[(x % 2, y % 4)]
    for row in cells:
        o.append('  ' + ''.join(chr(0x2800 + b) for b in row) + '\r\n')
    o.append('DEC graphics: ' + E + '(0' + 'lqqwqqk x x x tqqnqqu mqqvqqj `afgjklmnopqrstuvwxyz{|}~' + E + '(B\r\n')
    return ''.join(o)


def page_colors():
    o = [E + '[2J' + E + '[H' + 'SGR 30-37 on 40-47 (and 90-97 / 100-107):\r\n']
    for bg in range(8):
        o.append(''.join(E + '[%d;%dm %d%d ' % (30 + fg, 40 + bg, fg, bg) for fg in range(8)) + E + '[0m ')
        o.append(''.join(E + '[%d;%dm%d%d' % (90 + fg, 100 + bg, fg, bg) for fg in range(8)) + E + '[0m\r\n')
    o.append('Attributes: ' + E + '[1mbold' + E + '[0m ' + E + '[2mdim' + E + '[0m ' + E + '[3mitalic' + E + '[0m '
             + E + '[4munderline' + E + '[0m ' + E + '[5mblink' + E + '[0m ' + E + '[7mreverse' + E + '[0m '
             + E + '[8minvisible' + E + '[0m ' + E + '[9mstrike' + E + '[0m ' + E + '[1;4;7mall3' + E + '[0m\r\n')
    o.append('256 colours:\r\n')
    for r in range(4):
        o.append(''.join(E + '[48;5;%dm ' % (16 + r * 54 + c) for c in range(54)) + E + '[0m\r\n')
    o.append(''.join(E + '[48;5;%dm  ' % (232 + c) for c in range(24)) + E + '[0m grey ramp\r\n')
    o.append('24-bit:\r\n')
    o.append(''.join(E + '[48;2;%d;%d;%dm ' % (x * 3, 255 - x * 3, 128) for x in range(80)) + E + '[0m\r\n')
    o.append(''.join(E + '[38:2::%d:%d:%dm\u2588' % (x * 3, x * 3, x * 3) for x in range(80)) + E + '[0m\r\n')
    o.append(E + '[44m' + E + '[K blue line to the end (bce)' + E + '[0m\r\n')
    o.append(E + '[33;1mbright yellow' + E + '[22m normal ' + E + '[39mdefault' + E + '[0m\r\n')
    return ''.join(o)


def page_utf8():
    lines = [
        'UTF-8 torture (each line should end at the |):',
        'Latin-1:  àáâãäå æ ç èéêë ìíîï ð ñ òóôõö ø ùúûü ý þ ÿ ß |',
        'Capitals: ÀÁÂÃÄÅ Æ Ç ÈÉÊË ÌÍÎÏ Ð Ñ ÒÓÔÕÖ Ø ÙÚÛÜ Ý Þ Ÿ   |',
        'Polish:   Zażółć gęślą jaźń. ZAŻÓŁĆ GĘŚLĄ JAŹŃ.       |',
        'Czech:    Příliš žluťoučký kůň úpěl ďábelské ódy.     |',
        'Greek:    Ξεσκεπάζω την ψυχοφθόρα βδελυγμία            |',
        'Cyrillic: Съешь же ещё этих мягких французских булок |',
        'Symbols:  € £ ¥ ¢ § ¶ © ® ™ ° ± × ÷ ½ ¼ ¾ ¹ ² ³ µ     |',
        'Quotes:   \u201cdouble\u201d \u2018single\u2019 \u00abguill\u00bb \u2026 \u2013 \u2014 \u2022       |',
        'Arrows:   \u2190 \u2191 \u2192 \u2193 \u2194 \u2195 \u25b2 \u25bc \u25c0 \u25b6 \u25cf \u25cb \u25a0 \u25a1 \u2605 \u2713 \u2717      |',
        'Cards:    \u2660 \u2663 \u2665 \u2666 \u263a \u263b \u266a \u266b \u263c \u2640 \u2642             |',
        'Math:     \u2264 \u2265 \u2260 \u2248 \u2261 \u221e \u221a \u03c0 \u03a3 \u03a9 \u2229 \u00ac         |',
        'Wide CJK: \u65e5\u672c\u8a9e \ud55c\uad6d\uc5b4 \u4e2d\u6587 (2 cells each)     |',
        'Combining: e\u0301 a\u0300 o\u0308 n\u0303 (4 letters)         |',
        'Emoji:    \U0001F600 \U0001F680 (beyond the BMP)           |',
        'Fullwidth: \uff21\uff22\uff23 \uff11\uff12\uff13                |',
    ]
    o = E + '[2J' + E + '[H' + '\r\n'.join(lines) + '\r\n'
    o = o.encode('utf-8')
    bad = [b'Bad: lone continuation \x80\xbf, cut \xc3 x, overlong \xc0\xaf \xe0\x80\xaf, ',
           b'surrogate \xed\xa0\x80, too big \xf8\x88\x80\x80\x80, FF FE \xff\xfe end|\r\n',
           b'CP437 bytes (no UTF-8): \xc9\xcd\xcd\xbb \xb0\xb1\xb2\xdb \x81\x82\x84 \xe1 |\r\n']
    return o + b''.join(bad)


def page_cursor():
    o = [E + '#8', E + '[3;10H' + E + '[2K', E + '[4;1H', E + '[1J']
    o.append(E + '[1;1HDECALN (E all over), lines 1-4 erased above row 4, row 3 cleared')
    o.append(E + '[6;5r' + E + '[6;20H' + 'bad region (ignored)')
    o.append(E + '[8;12r' + E + '[12;1H')
    for i in range(8):
        o.append('\r\nscroll region 8-12, line %d' % i)
    o.append(E + '[r')
    o.append(E + '[14;1H' + E + '[0J')
    o.append(E + '[14;1H' + 'tabs:\tA\tB\tC\tD\tE\tF\tG\tH\tI')
    o.append(E + '[15;1H' + 'insert: abcdef' + E + '[15;10H' + E + '[4h' + 'XY' + E + '[4l' + ' (abXYcdef?)')
    o.append(E + '[16;1H' + 'delete: abcdef' + E + '[16;10H' + E + '[2P' + '  (should be ab..ef: "abef")')
    o.append(E + '[17;1H' + 'ICH: abcdef' + E + '[17;9H' + E + '[3@' + '  (ab   cdef)')
    o.append(E + '[18;1H' + 'ECH: abcdef' + E + '[18;9H' + E + '[2X' + '  (ab  ef)')
    o.append(E + '[19;1H' + 'wrap test: ' + 'x' * 69 + 'END' + 'next line')
    o.append(E + '[21;1H' + E + '7' + 'save' + E + '[22;40H' + 'moved' + E + '8' + ' restored after save')
    o.append(E + '[23;1H' + 'REP: a' + E + '[9b' + ' (ten a)  CHA:' + E + '[60G' + '<col 60')
    o.append(E + '[24;1H' + E + '[?7l' + 'no wrap: ' + 'y' * 80 + E + '[?7h')
    return ''.join(o)


def page_cp437():
    # a small BBS style ANSI picture in raw CP437 bytes
    o = bytearray(b'\x1b[2J\x1b[H\x1b[0;1;36m')
    o += b'\xc9' + b'\xcd' * 40 + b'\xbb\r\n'
    o += b'\xba\x1b[33m  CP437 BBS page \x1b[31m\x03\x04\x05\x06\x1b[36m' + b' ' * 19 + b'\xba\r\n'
    o += b'\xcc' + b'\xcd' * 40 + b'\xb9\r\n'
    o += b'\xba \x1b[32m\xb0\xb0\xb1\xb1\xb2\xb2\xdb\xdb\xdc\xdf\xdd\xde\x1b[36m \x1b[35m\x10\x11\x1e\x1f\x18\x19\x1a\x1b[36m' + b' ' * 19 + b'\xba\r\n'
    o += b'\xba \x1b[37m\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f \xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7' + b' ' * 14 + b'\x1b[36m\xba\r\n'
    o += b'\xc8' + b'\xcd' * 40 + b'\xbc\x1b[0m\r\n'
    return bytes(o)


def page_alt():
    return (E + '[?1049h' + E + '[2J' + E + '[H' + 'ALTERNATE SCREEN for 2 s ...').encode(), \
           (E + '[?1049l' + 'back on the main screen, with what was there').encode()


def page_burst(n=100000):
    rnd = random.Random(1)
    out = []
    size = 0
    while size < n:
        s = E + '[%d;%dH' % (rnd.randint(1, 24), rnd.randint(1, 70)) + E + '[%dm' % rnd.choice([0, 1, 4, 7, 31, 32]) + \
            rnd.choice(['hello', '\u2588\u2593\u2592\u2591', '\u2554\u2550\u2557', 'zażółć', '\u28ff\u2847']) + E + '[0m'
        out.append(s)
        size += len(s)
    return ''.join(out)


PAGES = [('1', 'box lines, blocks, braille', page_boxes),
         ('2', 'colours and attributes', page_colors),
         ('3', 'UTF-8 torture', page_utf8),
         ('4', 'cursor, scroll region, insert, delete', page_cursor),
         ('5', 'CP437 BBS bytes', page_cp437),
         ('6', 'alternate screen', None),
         ('7', 'burst of 100 KB', page_burst),
         ('8', 'ask for the terminal type again', None),
         ('9', 'xterm title and bell', None),
         ('m', 'mouse reports on/off', None)]


def menu():
    s = '\r\n' + E + '[0m' + 'tortured menu: ' + '  '.join('%s %s' % (k, n) for k, n, f in PAGES[:5]) + '\r\n' + \
        '  ' + '  '.join('%s %s' % (k, n) for k, n, f in PAGES[5:]) + '  q quit\r\nkey? '
    return s


class Handler(socketserver.BaseRequestHandler):
    def setup(self):
        self.buf = b''
        self.ttypes = []
        self.mouse = False
        self.log = lambda m: print('[%s] %s' % (self.client_address[0], m), flush=True)

    def send(self, data):
        if isinstance(data, str):
            data = data.encode('utf-8')
        self.request.sendall(data.replace(b'\xff', b'\xff\xff'))

    def cmd(self, *b):
        self.request.sendall(bytes(b))

    def reader(self, timeout):
        """bytes typed, after telnet commands are taken out"""
        self.request.settimeout(timeout)
        try:
            d = self.request.recv(4096)
        except socket.timeout:
            return b''
        if not d:
            raise EOFError
        out = bytearray()
        i = 0
        d = self.buf + d
        self.buf = b''
        while i < len(d):
            c = d[i]
            if c != IAC:
                out.append(c)
                i += 1
                continue
            if i + 1 >= len(d):
                self.buf = d[i:]
                break
            c2 = d[i + 1]
            if c2 in CMDS:
                if i + 2 >= len(d):
                    self.buf = d[i:]
                    break
                self.log('got %s %s' % (CMDS[c2], oname(d[i + 2])))
                i += 3
            elif c2 == SB:
                j = d.find(bytes([IAC, SE]), i)
                if j < 0:
                    self.buf = d[i:]
                    break
                sb = d[i + 2:j]
                if sb and sb[0] == 24 and len(sb) > 1 and sb[1] == 0:
                    self.ttypes.append(sb[2:].decode('latin-1'))
                    self.log('TTYPE IS %s' % sb[2:].decode('latin-1'))
                elif sb and sb[0] == 31 and len(sb) >= 5:
                    self.log('NAWS %dx%d' % (sb[1] * 256 + sb[2], sb[3] * 256 + sb[4]))
                elif sb and sb[0] == 42:
                    self.log('CHARSET reply %r' % sb[1:])
                else:
                    self.log('SB %r' % sb)
                i = j + 2
            elif c2 == IAC:
                out.append(IAC)
                i += 2
            else:
                self.log('got IAC %d' % c2)
                i += 2
        return bytes(out)

    def drain(self, secs):
        end = time.time() + secs
        got = b''
        while time.time() < end:
            got += self.reader(max(0.05, end - time.time()))
        return got

    def key(self):
        while True:
            k = self.reader(60)
            if k:
                return k

    def handle(self):
        self.log('connected')
        # the odd ones: options nobody has, STATUS, LINEMODE, TM, CHARSET
        self.cmd(IAC, DO, 24, IAC, DO, 31, IAC, WILL, 1, IAC, WILL, 3,
                 IAC, DO, 200, IAC, WILL, 201, IAC, DO, 34, IAC, WILL, 5,
                 IAC, DO, 39, IAC, DO, 42, IAC, DO, 6)
        self.drain(1.0)
        self.cmd(IAC, SB, 24, 1, IAC, SE)
        self.drain(0.5)
        self.cmd(IAC, SB, 42, 1) ; self.request.sendall(b';ISO-8859-1;UTF-8;CP437'); self.cmd(IAC, SE)
        self.drain(0.5)
        # the same requests again: an RFC 1143 client must not answer
        self.cmd(IAC, WILL, 1, IAC, DO, 24, IAC, WILL, 3)
        self.drain(0.5)
        if self.server.page:
            self.show(self.server.page)
            if self.server.page != 'r':
                self.drain(self.server.hold)
        try:
            self.send(menu())
            while True:
                k = self.key()
                c = k[:1].decode('latin-1')
                self.log('key %r' % k)
                if c in 'qQ':
                    self.send('\r\nbye\r\n')
                    return
                if c in 'r123456789m':
                    self.show(c)
                elif c == E and self.mouse:
                    self.send('\r\nmouse report: %r\r\n' % k)
                else:
                    self.send(menu())
        except EOFError:
            self.log('client closed')

    def show(self, c):
        if c == 'r' and self.server.replay:
            data = open(self.server.replay, 'rb').read()
            for i in range(0, len(data), 1024):
                self.send(data[i:i + 1024])
                time.sleep(0.01)
            return
        for k, n, f in PAGES:
            if k == c:
                break
        else:
            return
        if c == '6':
            a, b = page_alt()
            self.send(a)
            time.sleep(2)
            self.send(b)
        elif c == '8':
            self.cmd(IAC, SB, 24, 1, IAC, SE)
            self.drain(0.5)
            self.send('\r\nterminal types so far: %s\r\n' % ' '.join(self.ttypes))
        elif c == '9':
            self.send(E + ']0;tortured title\007' + E + ']2;and window title\033\\' + '\007bell sent\r\n')
        elif c == 'm':
            self.mouse = not self.mouse
            if self.mouse:
                self.send(E + '[?1000h' + E + '[?1006h\r\nmouse reports on (xterm, SGR): click in the window\r\n')
            else:
                self.send(E + '[?1006l' + E + '[?1000l\r\nmouse reports off\r\n')
        else:
            t0 = time.time()
            self.send(f())
            if c == '7':
                self.send(E + '[24;1H' + E + '[0m\r\nburst sent in %.1f s\r\n' % (time.time() - t0))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', type=int, default=47423)
    ap.add_argument('-b', default='127.0.0.1')
    ap.add_argument('-P', default='', help='page to show at once')
    ap.add_argument('-H', type=float, default=3600, help='seconds to hold the page')
    ap.add_argument('-r', default='', help='capture file to replay as page r')
    a = ap.parse_args()
    srv = Server((a.b, a.p), Handler)
    srv.page, srv.replay, srv.hold = a.P, a.r, a.H
    print('tortured on %s:%d' % (a.b, a.p), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
