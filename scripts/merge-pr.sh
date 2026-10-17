#!/bin/sh
# Merge a pull request without leaking a personal email into history.
#   scripts/merge-pr.sh <PR number>
# The scan must pass on the PR branch first. The merge commit is authored by
# the GitHub no-reply address (GitHub itself is the committer); the
# resulting history is scanned again.
set -eu
cd "$(dirname "$0")/.."
n=${1:?usage: scripts/merge-pr.sh <PR number>}
NOREPLY=${GIT_NOREPLY:-$(git config user.email)}
case $NOREPLY in *@users.noreply.github.com) ;; *) echo "git user.email is not a GitHub no-reply address" >&2; exit 1;; esac
scripts/scan-public.sh
if ! gh pr merge "$n" --merge --delete-branch --author-email "$NOREPLY"; then
    # The API can refuse the no-reply address; merge locally instead. GitHub
    # marks the PR merged once main contains its head.
    br=$(gh pr view "$n" --json headRefName -q .headRefName)
    title=$(gh pr view "$n" --json title -q .title)
    git fetch -q origin main "$br"
    git checkout -q main && git merge -q --ff-only origin/main
    git -c user.email="$NOREPLY" merge -q --no-ff "origin/$br" \
        -m "Merge pull request #$n from $(gh repo view --json owner -q .owner.login)/$br" -m "$title"
    git push -q origin main
    git push -q origin --delete "$br" || true
    git branch -q -d "$br" 2>/dev/null || true
fi
git checkout -q main && git pull -q --ff-only
scripts/scan-public.sh
git log -1 --format='merged: %H'
