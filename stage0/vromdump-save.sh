#!/bin/sh
# Run ON THE 755C (tc32 shell, as root) after: tce-load -i .../vromdump.tcz
#   sh vromdump-save.sh DIR      copy the snapshot files into DIR (e.g. a
#                                FAT floppy or CF card mounted read-write)
#   sh vromdump-save.sh -serial  print them as hex between BEGIN/END markers
#                                (capture with picocom --logfile, decode on
#                                the host: stage0/harness/serial2bin.py)
# Only reads /proc/vromdump/*; the module touches no hardware register.
set -eu
P=/proc/vromdump
[ -r $P/info ] || { echo "no $P: tce-load -i vromdump.tcz (or insmod vromdump.ko) first" >&2; exit 1; }
FILES="vrom sysbios lowmem"
if [ "${1:-}" = "-serial" ]; then
    echo "### vromdump BEGIN info"; cat $P/info; echo "### vromdump END info"
    for f in $FILES; do
        echo "### vromdump BEGIN $f $(wc -c < $P/$f) $(sha256sum $P/$f | cut -d' ' -f1)"
        hexdump -v -e '32/1 "%02x" "\n"' $P/$f
        echo "### vromdump END $f"
    done
    exit 0
fi
D=${1:?usage: $0 DIR | -serial}
mkdir -p "$D"
cp $P/info "$D/vromdump-info.txt"
for f in $FILES; do cat $P/$f > "$D/$f.bin"; done
(cd "$D" && sha256sum vrom.bin sysbios.bin lowmem.bin > vromdump.sha256)
sync
cat "$D/vromdump-info.txt"; cat "$D/vromdump.sha256"
echo "saved to $D"
