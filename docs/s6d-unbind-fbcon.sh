#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Drop a bound framebuffer console before rmmod. Linux vt.c
# store_bind(0) calls vt_unbind, which calls do_unbind_con_driver.
# rmmod returns EBUSY ("Resource temporarily unavailable") while that
# console still holds the fb. WD24X_MOCK prefixes the sysfs path.
P=${WD24X_MOCK:-}
tag=${1:-WD24X-UNBIND}
for c in "$P"/sys/class/vtconsole/vtcon*; do
	[ -e "$c" ] || continue
	[ -r "$c/name" ] || continue
	name=$(cat "$c/name" 2>/dev/null || echo "")
	case "$name" in
	*frame\ buffer*)
		if echo 0 > "$c/bind" 2>/dev/null; then
			echo "$tag unbind $(basename "$c")"
		else
			echo "$tag unbind failed $(basename "$c")"
		fi
		;;
	esac
done
