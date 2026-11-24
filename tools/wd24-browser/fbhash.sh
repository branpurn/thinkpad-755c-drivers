#!/bin/sh
# Hash the visible framebuffer and print the wd24adv counters.
# WD24X_MOCK is a path prefix for a host stand-in. On the machine leave
# it empty. Default length is 640x480x8, 307200 bytes.
# No register write.
P=${WD24X_MOCK:-}
N=${WD24X_FB_BYTES:-307200}
fb=$P/dev/fb0
if [ ! -r "$fb" ]; then
	echo "WD24X-BROWSER stop no fb"
	exit 1
fi
sz=$(wc -c < "$fb" | tr -d ' ')
if [ "$sz" -lt "$N" ]; then
	echo "WD24X-BROWSER stop short fb"
	exit 1
fi
dd if="$fb" bs="$N" count=1 status=none | sha256sum | awk '{print "WD24X-BROWSER fb", $1, "'"$N"'"}'
s6d=$P/proc/wd24adv/s6d
if [ -r "$s6d" ]; then
	echo "WD24X-BROWSER s6d"
	cat "$s6d"
fi
live=$P/proc/wd24adv/live
if [ -r "$live" ]; then
	echo "WD24X-BROWSER live"
	cat "$live"
fi
echo "WD24X-BROWSER done"
