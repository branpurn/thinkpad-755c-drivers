#!/bin/sh
# Later acceptance for a physical key. Not armed.
# WD24X_TTY_ACCEPT=keystroke is set by a person at the machine.
# The key helper saves termios, waits at most WD24X_TTY_KEY_SEC seconds
# (default 15), and puts the saved termios back on success, timeout,
# and caught signals. It does not call KDSETMODE or VT_ACTIVATE.
# A byte from the tty is not proof of which VT is foreground.
# Serial output and VGA+ bind are not this test.
# before and after snapshots are kept when the key times out.
P=${WD24X_MOCK:-}
READ=${WD24X_TTY_BIN:-$P/tmp/wd24x-tty-read}
KEYBIN=${WD24X_TTY_KEYBIN:-$P/tmp/wd24x-tty-key}
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
key_rc=1
if [ -n "${WD24X_TTY_KEY:-}" ]; then
	key_rc=0
else
	[ -x "$KEYBIN" ] || {
		echo "WD24X-TTY-ACCEPT stop key helper"
		exit 1
	}
	"$KEYBIN" > "$P/tmp/wd24x-tty-key.out" 2>"$P/tmp/wd24x-tty-key.err" || key_rc=$?
fi
"$READ" > "$P/tmp/wd24x-tty-after.out" 2>"$P/tmp/wd24x-tty-after.err" || {
	echo "WD24X-TTY-ACCEPT stop after"
	exit 1
}
if [ "$key_rc" != 0 ]; then
	echo "WD24X-TTY-ACCEPT stop no key"
	exit 1
fi
echo "WD24X-TTY-ACCEPT before"
cat "$P/tmp/wd24x-tty-before.out"
echo "WD24X-TTY-ACCEPT after"
cat "$P/tmp/wd24x-tty-after.out"
echo "WD24X-TTY-ACCEPT key 1"
echo "WD24X-TTY-ACCEPT done"
