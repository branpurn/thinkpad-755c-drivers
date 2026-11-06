#!/bin/sh
# Later acceptance for a physical key. Not armed.
# WD24X_TTY_ACCEPT=keystroke is set by a person at the machine.
# This file does not call KDSETMODE or VT_ACTIVATE.
# A byte from the tty is not proof of which VT is foreground.
# Serial output and VGA+ bind are not this test.
P=${WD24X_MOCK:-}
READ=${WD24X_TTY_BIN:-$P/tmp/wd24x-tty-read}
if [ "$WD24X_TTY_ACCEPT" != "keystroke" ]; then
	echo "WD24X-TTY-ACCEPT stop not armed"
	exit 1
fi
[ -x "$READ" ] || {
	echo "WD24X-TTY-ACCEPT stop reader missing"
	exit 1
}
"$READ" > "$P/tmp/wd24x-tty-before.out" 2>"$P/tmp/wd24x-tty-before.err" || {
	echo "WD24X-TTY-ACCEPT stop before"
	exit 1
}
echo "WD24X-TTY-ACCEPT press one key on the physical keyboard"
if [ -n "${WD24X_TTY_KEY:-}" ]; then
	key=1
else
	key=$(dd if=/dev/tty0 bs=1 count=1 2>/dev/null | wc -c)
fi
if [ "$key" != "1" ]; then
	echo "WD24X-TTY-ACCEPT stop no key"
	exit 1
fi
"$READ" > "$P/tmp/wd24x-tty-after.out" 2>"$P/tmp/wd24x-tty-after.err" || {
	echo "WD24X-TTY-ACCEPT stop after"
	exit 1
}
echo "WD24X-TTY-ACCEPT before"
cat "$P/tmp/wd24x-tty-before.out"
echo "WD24X-TTY-ACCEPT after"
cat "$P/tmp/wd24x-tty-after.out"
echo "WD24X-TTY-ACCEPT key 1"
echo "WD24X-TTY-ACCEPT done"
