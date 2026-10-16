#!/bin/sh
# Public-repo hygiene scan over tracked + staged files. Run before every push.
# (adapted from tinycore-32-ram)
# Flags: private/LAN IPs, MAC addresses, emails other than noreply/example,
# home-directory paths, hostnames of the dev box, likely tokens/keys.
set -u
cd "$(dirname "$0")/.."
files=$(git ls-files -co --exclude-standard | grep -vE '^(dl|build|out)/')
[ -n "$files" ] || { echo "nothing to scan"; exit 0; }
fail=0
chk() { # desc regex
    hits=$(printf '%s\n' "$files" | xargs grep -nIE "$2" 2>/dev/null | grep -vE '^scripts/scan-public\.sh:' \
           | grep -vE 'users\.noreply\.github\.com|@example\.(com|org)|@vger\.kernel\.org|@users\.sourceforge' || true)
    if [ -n "$hits" ]; then echo "!! $1"; printf '%s\n' "$hits" | head -20; fail=1; fi
}
chk "private IPv4 address" '\b(10\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}|192\.168\.[0-9]{1,3}\.[0-9]{1,3}|172\.(1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.[0-9]{1,3})\b'
chk "MAC address" '\b([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b'
chk "email address (non-noreply)" '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[a-z]{2,}' 
chk "home directory path" '/home/([a-su-z]|t[abd-z]|tc[a-z0-9_])|/Users/[A-Za-z]'
chk "token / key material" '(gh[pousr]_[A-Za-z0-9]{20,}|github_pat_|AKIA[0-9A-Z]{16}|BEGIN [A-Z ]*PRIVATE KEY|xox[abp]-|sk-[A-Za-z0-9]{20,})'
chk "dev-box identifiers" "${TC755_SCAN_EXTRA:-pop-os}|$(hostname 2>/dev/null || echo pop-os)"
# Commit metadata must use GitHub no-reply addresses only.
bad_authors=$(git log --all --format='%ae%n%ce' 2>/dev/null | sort -u | grep -vE '@users\.noreply\.github\.com$|^noreply@github\.com$' || true)
if [ -n "$bad_authors" ]; then echo "!! non-noreply commit author/committer email(s) in history"; fail=1; fi
if [ $fail -ne 0 ]; then echo "scan: FAILED (review above; allowlisted: noreply/example addresses)"; exit 1; fi
echo "scan: clean ($(printf '%s\n' "$files" | wc -l) files)"
