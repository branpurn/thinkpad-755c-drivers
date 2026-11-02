#!/bin/sh
# check-i486.sh FILE...: fail if an ELF (or .a/.o) holds instructions a
# 486 DX4 cannot run: cmov/fcmov, fcomi, rdtsc, cmpxchg8b, sysenter,
# long NOPs (nopl/nopw), MMX/SSE/AVX registers or instructions, fences,
# prefetch, popcnt/lzcnt/tzcnt, movbe, fisttp. (cpuid, bswap, cmpxchg,
# xadd and invlpg are 486 instructions and are allowed.)
set -eu
OBJDUMP=${OBJDUMP:-}
if [ -z "$OBJDUMP" ]; then
    for c in i486-linux-musl-objdump objdump; do
        command -v "$c" >/dev/null && { OBJDUMP=$c; break; }
    done
fi
[ -n "$OBJDUMP" ] || { echo "check-i486: no objdump" >&2; exit 2; }
BAD='^(f?cmov[a-z]*|f(u)?comip?|rdtscp?|rdpmc|cmpxchg8b|cmpxchg16b|sysenter|sysexit|syscall|sysret|nop[lwq]|emms|femms|[lms]fence|prefetch[a-z0-9]*|popcnt|lzcnt|tzcnt|movbe|fisttp[a-z]*|xgetbv|rdrand|rdseed|crc32[a-z]*|movnti|clflush[a-z]*)$'
rc=0
for f in "$@"; do
    tmp=$(mktemp)
    "$OBJDUMP" -d --no-show-raw-insn -M att "$f" > "$tmp"
    # mnemonic is the 2nd tab-separated field of an instruction line
    n_insn=$(awk -F'\t' 'NF>=2 && $1 ~ /^ *[0-9a-f]+:$/ {n++} END {print n+0}' "$tmp")
    hits=$(awk -F'\t' -v bad="$BAD" '
        NF>=2 && $1 ~ /^ *[0-9a-f]+:$/ {
            split($2, w, " "); m = w[1]
            if (m ~ /^(lock|rep|repz|repnz|repe|repne|data16|addr16|cs|ds|es|ss|fs|gs)$/) m = w[2]
            if (m ~ bad || $2 ~ /%(mm|xmm|ymm|zmm)[0-9]/) { print; c++ }
        } END { exit 0 }' "$tmp")
    if [ -n "$hits" ]; then
        echo "check-i486: $f: NOT 486-safe ($(printf '%s\n' "$hits" | wc -l) of $n_insn instructions):" >&2
        printf '%s\n' "$hits" | sed 's/^/    /' | head -40 >&2
        rc=1
    else
        echo "check-i486: $f: OK ($n_insn instructions, none beyond i486)" >&2
    fi
    rm -f "$tmp"
done
exit $rc
