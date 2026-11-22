#!/bin/sh
# Put the TelnetU items of ua/Office.add at the end of /usr/lib/ua/Office.
# Old TelnetU items there (the last items, from telnetu 1.0 or 1.1) go first.
# Usage: sh office-fix.sh /path/to/Office.add
# (Old sh: no \$ inside back quotes, it loses the backslash there.)
O=/usr/lib/ua/Office
A=$1
if [ ! -f "$A" ]; then echo "usage: sh office-fix.sh Office.add"; exit 1; fi
n=`wc -l < $O | sed 's/ //g'`
first=`grep -n '^Name=TelnetU' $O | sed -n 1p | sed 's/:.*//'`
if [ -z "$first" ]; then
	first=`expr $n + 1`
fi
names=`sed -n "$first"',$p' $O | grep -c '^Name='`
tnames=`sed -n "$first"',$p' $O | grep -c '^Name=TelnetU'`
echo "Office: $n lines, TelnetU items from line $first: $tnames of $names"
if [ "$names" != "$tnames" ]; then
	echo "other items come after the TelnetU items: nothing changed"
	exit 1
fi
k=`expr $first - 1`
sed "${k}q" $O > /tmp/Office.new
cat $A >> /tmp/Office.new
cp /tmp/Office.new $O && rm -f /tmp/Office.new
echo "now `wc -l < $O | sed 's/ //g'` lines, `grep -c '^Name=TelnetU' $O` TelnetU items"
