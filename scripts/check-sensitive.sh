#!/usr/bin/env bash
# Pre-push guard: report information that must not be published to a public remote.
#
# Usage:
#   scripts/check-sensitive.sh              scan staged changes (the normal pre-commit gate)
#   scripts/check-sensitive.sh --all        scan every tracked file
#   scripts/check-sensitive.sh --range A..B scan the tip tree of a commit range, e.g. HEAD~3..HEAD
#
# Generic detectors cover email addresses, private/LAN IPv4 ranges, absolute home
# paths and private-key blocks. Project-specific literal terms (real domains,
# hostnames, account names) belong in a gitignored .sensitive-patterns file, one
# term per line; blank lines and #-comments are ignored.
#
# The allow list is line-level: a line is dropped from the report if it also
# matches a known placeholder, upstream attribution or a documented reserved
# range. Review --all output manually before trusting a clean result.
#
# Exit status: 0 when nothing was found, 1 when findings were printed, 2 on usage errors.
set -uo pipefail

usage() {
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
}

mode=staged
range=""
while [ $# -gt 0 ]; do
    case "$1" in
        --all) mode=all ;;
        --range)
            mode=range
            range="${2:-}"
            [ -n "$range" ] || { usage >&2; exit 2; }
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

root=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "error: not a git repository" >&2; exit 2; }
cd "$root" || exit 2

# Values that are intentionally public: placeholders, the RFC 5737 documentation
# ranges, loopback, upstream attribution, documented example addresses, and the
# reserved/private CIDR literals that upstream templates list.
allow_placeholder='example\.(com|org|net|invalid)|@users\.noreply\.github\.com|@github\.com|127\.0\.0\.1|localhost|192\.0\.2\.|198\.51\.100\.|203\.0\.113\.'
allow_upstream='miloyip@gmail\.com|rjeczalik@gmail\.com|tindy\.it@gmail\.com|192\.168\.100\.1|192\.168\.1\.5|192\.168\.122\.11'
allow_cidr='([0-9]{1,3}\.){3}[0-9]{1,3}/(3[0-2]|[12]?[0-9])([^0-9]|$)'
allow="$allow_placeholder|$allow_upstream|$allow_cidr"

patterns_file=$(mktemp)
trap 'rm -f "$patterns_file"' EXIT

cat > "$patterns_file" <<'PATTERNS'
[[:alnum:]._%+-]+@[[:alnum:].-]+\.[[:alpha:]]{2,}
(^|[^0-9])10\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}
(^|[^0-9])192\.168\.[0-9]{1,3}\.[0-9]{1,3}
(^|[^0-9])172\.(1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.[0-9]{1,3}
(/Users|/home)/[[:alnum:]._-]+
BEGIN [A-Z ]*PRIVATE KEY
PATTERNS

if [ -f .sensitive-patterns ]; then
    grep -vE '^[[:space:]]*(#|$)' .sensitive-patterns >> "$patterns_file"
fi

scan_blob() {
    # $1: display path, stdin: content
    grep -nEf "$patterns_file" 2>/dev/null | grep -vE "$allow" | sed "s|^|$1:|"
}

findings=""
case "$mode" in
    staged)
        while IFS= read -r -d '' file; do
            [ -f "$file" ] || continue
            findings+=$(git show ":$file" 2>/dev/null | scan_blob "$file")
            findings+=$'\n'
        done < <(git diff --cached --name-only -z --diff-filter=ACM)
        ;;
    range)
        tip=$(git rev-list -1 "$range" 2>/dev/null) || { echo "error: bad range '$range'" >&2; exit 2; }
        while IFS= read -r -d '' file; do
            findings+=$(git show "$tip:$file" 2>/dev/null | scan_blob "$file")
            findings+=$'\n'
        done < <(git diff --name-only -z --diff-filter=ACM "$range")
        ;;
    all)
        while IFS= read -r -d '' file; do
            case "$file" in
                include/*|base/rules/*|LICENSE) continue ;;
            esac
            [ -f "$file" ] || continue
            findings+=$(scan_blob "$file" < "$file")
            findings+=$'\n'
        done < <(git ls-files -z)
        ;;
esac

findings=$(printf '%s\n' "$findings" | grep -vE '^[[:space:]]*$' || true)

if [ -n "$findings" ]; then
    echo "Sensitive information found; do not push until each item is fixed:"
    echo
    printf '%s\n' "$findings"
    echo
    echo "Replace with placeholders (example.com, 192.0.2.0/24, <PLACEHOLDER>) or keep the value out of the repository."
    exit 1
fi

echo "check-sensitive: no findings ($mode)"
exit 0
