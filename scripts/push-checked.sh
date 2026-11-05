#!/bin/sh
# push-checked.sh [BRANCH]: the only push path. Fails closed: any failed
# check (scan, wd90c24fb diff, branch, clean tree) exits non-zero BEFORE
# git push runs. Default branch wd24adv; never pushes main.
#   PUSH_DRYRUN=1  run every check, print the push command, do not push
set -eu
cd "$(dirname "$0")/.."
br=${1:-wd24adv}
die() { echo "push-checked: BLOCKED: $*" >&2; exit 1; }
[ "$br" != main ] || die "refusing to push main"
[ "$(git rev-parse --abbrev-ref HEAD)" = "$br" ] || die "HEAD is not $br"
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "uncommitted changes"
if ! sh scripts/scan-public.sh; then die "scan-public.sh failed"; fi
if ! git diff --quiet main -- modules/wd90c24fb; then die "wd90c24fb differs from main"; fi
if [ -n "${PUSH_DRYRUN:-}" ]; then echo "push-checked: checks passed; would run: git push origin $br"; exit 0; fi
git push origin "$br"
echo "push-checked: pushed $br $(git rev-parse --short HEAD)"
