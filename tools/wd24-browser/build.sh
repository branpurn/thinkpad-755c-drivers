#!/bin/sh
# Stop point for an i486 static NetSurf framebuffer build.
# Does not download and does not compile. The pins are the GitHub
# master commits read on 2026-11-22.
set -eu
NS_SHA=170d89462442c286f80c0604f30e4c7f2a801265
NSFB_SHA=a287525eeafa4b9e721822670e1e03eeeef6b31c
CC=${WD24X_NSFB_CC:-i486-linux-musl-gcc}
echo "WD24X-BROWSER pin netsurf $NS_SHA"
echo "WD24X-BROWSER pin libnsfb $NSFB_SHA"
if ! command -v "$CC" >/dev/null 2>&1; then
	echo "WD24X-BROWSER stop no $CC"
fi
echo "WD24X-BROWSER stop no linux fbdev surface"
echo "WD24X-BROWSER surface list ram sdl x vnc wld"
exit 2
