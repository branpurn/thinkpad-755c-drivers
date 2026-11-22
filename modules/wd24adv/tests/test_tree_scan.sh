#!/bin/sh
# Scan the current docs, tests, and scripts. Git history is not read.
# Rewriting history needs separate owner approval.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
fail=0
# Built so this file does not contain the forbidden text.
name=$(printf '%s%s' Bran don)
host=$(printf '%s%s' grok-bot- server)
prompt=$(printf '%s%s' 'root@' box)
box=$(printf '%s%s' P op)
id_re='(^|[^A-Za-z0-9_])t[0-9]{3,}[a-z][0-9]*([^A-Za-z0-9_]|$)'
pt_re='(^|[^A-Za-z0-9_])pt[0-9]{3,}[a-z][0-9]*([^A-Za-z0-9_]|$)'

scan() {
	pat=$1
	label=$2
	hit=$(grep -R -I -n -E -e "$pat" \
		"$ROOT/docs" "$ROOT/tests" "$ROOT/tools" \
		"$ROOT/modules/wd24adv/tests" "$ROOT/sdl/tests" \
		--include='*.txt' --include='*.md' --include='*.sh' \
		--include='*.py' --include='*.c' --include='*.h' \
		2>/dev/null || true)
	if [ -n "$hit" ]; then
		echo "FAIL $label"
		echo "$hit"
		fail=1
	fi
}

scan "\\b${name}\\b" "name"
scan "$host" "relay"
scan "$prompt" "prompt"
scan "\\b${box}\\b" "serial-host"
scan "$id_re" "message-id"
scan "$pt_re" "message-id-pt"

if [ "$fail" = 0 ]; then
	echo "test_tree_scan: passed (current tree only; history not rewritten)"
else
	echo "test_tree_scan: failed"
	exit 1
fi
