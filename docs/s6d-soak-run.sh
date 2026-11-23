#!/bin/sh
# One hour of mixed console, X, and Doom, later, on the 755C.
# This file does not start that hour unless WD24X_SOAK_RUN=1.
# First failing step stops the run. Log is /tmp/wd24x-soak.log
# unless WD24X_SOAK_LOG is set. dash/ash only.
set -eu
LOG=${WD24X_SOAK_LOG:-/tmp/wd24x-soak.log}
BUDGET=${WD24X_SOAK_BUDGET:-3600}

plan() {
	echo "WD24X-SOAK scanpat docs/s6d-scanpat-run.sh"
	echo "WD24X-SOAK desktop docs/s6d-desktop-run.sh"
	echo "WD24X-SOAK doom docs/s6d-doom-run.sh timedemo13hmap"
	echo "WD24X-SOAK fbcon docs/s6d-fbcon-run.sh"
	echo "WD24X-SOAK budget $BUDGET"
}

if [ "${WD24X_SOAK_RUN:-}" != 1 ]; then
	plan
	echo "WD24X-SOAK plan only"
	exit 0
fi

: > "$LOG"
echo "WD24X-SOAK start" >> "$LOG"
step() {
	echo "WD24X-SOAK step $1" >> "$LOG"
	# shellcheck disable=SC2086
	sh -c "$2" >> "$LOG" 2>&1 || {
		echo "WD24X-SOAK stop $1"
		echo "WD24X-SOAK stop $1" >> "$LOG"
		exit 1
	}
	echo "WD24X-SOAK ok $1" >> "$LOG"
}

if [ -n "${WD24X_SOAK_CMDS:-}" ]; then
	while read -r name cmd; do
		[ -n "$name" ] || continue
		step "$name" "$cmd"
	done < "$WD24X_SOAK_CMDS"
else
	step scanpat "sh docs/s6d-scanpat-run.sh"
	step desktop "sh docs/s6d-desktop-run.sh"
	step doom "sh docs/s6d-doom-run.sh timedemo13hmap"
	step fbcon "sh docs/s6d-fbcon-run.sh"
fi
echo "WD24X-SOAK done"
echo "WD24X-SOAK done" >> "$LOG"
