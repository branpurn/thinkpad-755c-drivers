#!/bin/sh
# Every docs/s6d-*-run.sh insmod line must pass wd24_load_params_refuse.
# Doom's mode=$gfx / $extra is expanded to the two loads that script uses.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
BIN=/tmp/wd24-test-load-params
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

check() {
	label=$1
	shift
	if "$BIN" "$@" >"$ROOT/modules/wd24adv/tests/.params.out" 2>"$ROOT/modules/wd24adv/tests/.params.err"; then
		ok
	else
		bad "$label"
		cat "$ROOT/modules/wd24adv/tests/.params.err"
	fi
}

# Join continued insmod lines, drop the command and the .ko argument.
extract() {
	awk '
		/^[[:space:]]*#/ { next }
		/insmod / {
			line = $0
			while (line ~ /\\[[:space:]]*$/) {
				sub(/\\[[:space:]]*$/, "", line)
				if (getline nxt <= 0)
					break
				line = line " " nxt
			}
			print line
		}
	' "$1"
}

args_of() {
	# shell words after the ko path
	shift
	printf '%s\n' "$@"
}

lines=$(mktemp)
n=0
for script in "$ROOT"/docs/s6d-*-run.sh; do
	extract "$script" > "$lines"
	while IFS= read -r line; do
		[ -n "$line" ] || continue
		doom=0
		if printf '%s\n' "$line" | grep -q 'mode=\$gfx\|\$extra'; then
			doom=1
		fi
		if [ "$doom" = 1 ]; then
			grep -q 'extra=present_mode=13h' "$script" || bad "$script gfx"
			set -- $(printf '%s\n' "$line" | sed 's/mode=\$gfx/mode=640x480-8/; s/\$extra//; s/trial=\$trial/trial=600/')
			shift 2
			check "$script letterbox" "$@"
			set -- $(printf '%s\n' "$line" | sed 's/mode=\$gfx/mode=320x200-8/; s/\$extra/present_mode=13h/; s/trial=\$trial/trial=180/')
			shift 2
			check "$script 13h" "$@"
		else
			set -- $(printf '%s\n' "$line" | sed 's/trial=\$trial/trial=180/')
			shift 2
			check "$script" "$@"
		fi
	done < "$lines"
	n=$((n + 1))
done
rm -f "$lines"
[ "$n" -ge 8 ] && ok || bad "script count $n"

# The combination that failed on the hardware must still be refused.
if "$BIN" dryrun=0 live=1 mode=640x480-8 trial=180 s6d=1 s6d_cmap=1 \
	s6d_fbcon=1 s6d_bind=1 >/dev/null 2>"$ROOT/modules/wd24adv/tests/.params.err"; then
	bad "bind+cmap allowed"
else
	grep -q 's6d_cmap=1 refused' "$ROOT/modules/wd24adv/tests/.params.err" && ok || bad "refuse text"
fi

echo "test_run_params: $pass passed, $fail failed"
[ "$fail" = 0 ]
