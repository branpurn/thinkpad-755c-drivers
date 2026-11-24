#!/bin/sh
# Boot a 486 guest on the WD90C24 model. This script does not download
# QEMU and does not invent a kernel image. It prints the command and
# exits non-zero until both qemu-system-i386 and the tc755x vmlinuz
# and initrd are supplied.
#
# CPU: -cpu 486 has no TSC. -icount shift=4 is 16 ns of virtual time
# per retired instruction, 62.5 million instructions/s. The 755C CPU
# is the 75 MHz 486 named in docs/s6d-kernel-speed.txt (Intel 486DX4
# at 75 MHz, ThinkPad 755C type 9545). 62.5/75 is about 0.83
# instructions per clock. That ratio is an assumption, not a measurement.
# RAM is 36 MB. The guest clock is the PIT; HZ=250 and HZ_PERIODIC are
# the audited tc755x config in that same note, not a QEMU flag.
set -u
QEMU=${WD24X_QEMU:-qemu-system-i386}
KERNEL=${WD24X_VMLINUZ:-}
INITRD=${WD24X_INITRD:-}
if ! command -v "$QEMU" >/dev/null 2>&1; then
	echo "qemu-system-i386 is not installed" >&2
	echo "pin: QEMU 9.2.x, device tools/wd24-validate/wd24_chip.c" >&2
	exit 1
fi
if [ -z "$KERNEL" ] || [ ! -f "$KERNEL" ]; then
	echo "tc755x vmlinuz is not in this tree" >&2
	exit 1
fi
if [ -z "$INITRD" ] || [ ! -f "$INITRD" ]; then
	echo "tc755x initrd is not in this tree" >&2
	exit 1
fi
exec "$QEMU" -machine pc -cpu 486 -m 36 \
	-icount shift=4,sleep=off,align=off \
	-kernel "$KERNEL" -initrd "$INITRD" \
	-append "console=ttyS0,9600" \
	-serial stdio -display none \
	-device wd90c24
