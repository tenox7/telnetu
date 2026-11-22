#!/usr/bin/env python3
"""mksyms.py SYMS.TXT > syms.h: telnet's symbol pictures as C tables."""
import sys
lines = [l.rstrip('\n') for l in open(sys.argv[1]) if not l.startswith('# ')]
syms, marks, i = [], [], 0
while i < len(lines):
    head = lines[i].split()
    if not head:
        i += 1
        continue
    if head[0] == 'mark':
        rows, i = lines[i + 1:i + 5], i + 5
        name = head[1]
        dest = marks
        want = 4
    else:
        rows, i = lines[i + 1:i + 13], i + 13
        name = int(head[0], 16)
        dest = syms
        want = 12
    if len(rows) != want or any(len(r) != 9 or set(r) - set('.#') for r in rows):
        sys.exit('bad picture: %s' % ' '.join(head))
    bits = [sum(1 << x for x, c in enumerate(r) if c == '#') for r in rows]
    dest.append((name, ' '.join(head[1:] if dest is syms else head[2:]), bits))
out = ['/* syms.h - made by tools/mksyms.py from syms.txt: do not edit */',
       'static struct { unsigned short cp, rows[12]; } syms[] = {']
for cp, name, b in syms:
    out.append('\t{0x%04x, {%s}},\t/* %s */' % (cp, ','.join('0x%03x' % v for v in b), name))
out += ['\t{0, {0}}', '};', '',
        '/* marks: 3 rows over a small letter, 1 over a capital */',
        'static struct { char m; unsigned short rows[4]; } marks[] = {']
for m, name, b in marks:
    out.append("\t{'%s', {%s}},\t/* %s */" % ('\\' + m if m in "'\\" else m, ','.join('0x%03x' % v for v in b), name))
out += ['\t{0, {0}}', '};']
print('\n'.join(out))
