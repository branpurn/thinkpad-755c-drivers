#!/bin/sh
# Paste one line at a time. Each line is under 480 characters.
# Reads only, plus the CRTC index select. No data-register writes.
# WD24X_PROBE_DRY=1 prints the plan and does not open /dev/port.
set -u
BIN=${WD24X_PROBE_BIN:-/tmp/wd24-probe}
if [ "${WD24X_PROBE_DRY:-0}" = 1 ]; then
	exec "$BIN" --dry
fi
exec "$BIN"
