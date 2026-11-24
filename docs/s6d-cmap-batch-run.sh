#!/bin/sh
# Not authorized. Runs only when WD24X_BATCH_ARM=phase2-and-phase3.
# That variable is set by a person after authorizing both phases.
# Copying this file does not set it and does not insmod.
# Phase 3 starts only after phase 2 exits 0. A failure does not retry.
P2=${WD24X_P2_SCRIPT:-/tmp/wd24x-cmap-p2-run.sh}
P3=${WD24X_P3_SCRIPT:-/tmp/wd24x-cmap-p3-run.sh}
if [ "$WD24X_BATCH_ARM" != "phase2-and-phase3" ]; then
	echo "WD24X-CMAP-BATCH stop not armed"
	exit 1
fi
echo "WD24X-CMAP-BATCH phase 2"
sh "$P2"
rc2=$?
echo "P2RC=$rc2"
if [ "$rc2" != 0 ]; then
	echo "WD24X-CMAP-BATCH stop phase 2"
	exit 1
fi
echo "WD24X-CMAP-BATCH phase 3"
sh "$P3"
rc3=$?
echo "P3RC=$rc3"
if [ "$rc3" != 0 ]; then
	echo "WD24X-CMAP-BATCH stop phase 3"
	exit 1
fi
echo "WD24X-CMAP-BATCH done"
