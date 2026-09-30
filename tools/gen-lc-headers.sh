#!/bin/bash
# Rebuild the dsp/linux/lc_headers case-insensitivity bridge with RELATIVE
# symlinks. The original set (and upstream's) pointed at absolute paths inside
# the author's home directory, so it broke for everyone else and would end up in
# the CodeRook snapshot. Here the link NAMES are taken from the original set (so
# every inconsistent include casing still resolves) and the targets are computed
# relative to the checkout.
set -eu
cd "$(dirname "$0")"
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

# 1. the exact link names the build expects, from the commit that deleted them
git show 5dd618b --diff-filter=D --name-only --format='' \
  | grep '^dsp/linux/lc_headers/' \
  | sed 's|.*/||' | sort -u > /tmp/lc_names.txt
echo "names needed: $(wc -l < /tmp/lc_names.txt)"

# 2. every real header outside lc_headers itself, keyed by lowercase basename.
#    Covers the whole checkout (the Windows sources live under both dsp/ and
#    audiopassthru/); skips .git, build output and third_party.
find . -iname '*.h' -not -path './.git/*' -not -path '*/lc_headers/*' \
       -not -path './third_party/*' -not -path './build*/*' -print0 > /tmp/lc_real.txt
echo "candidate headers: $(tr -dc '\0' < /tmp/lc_real.txt | wc -c)"

relpath() { # $1 = target file, from dsp/linux/lc_headers/
  python3 -c "
import os,sys
print(os.path.relpath(sys.argv[1], 'dsp/linux/lc_headers'))" "$1"
}

made=0; missing=0
while read -r name; do
  lc=$(echo "$name" | tr 'A-Z' 'a-z')
  target=$(python3 - "$lc" <<'PY'
import os, sys
want = sys.argv[1]
best = None
for root, dirs, files in os.walk('.'):
    if any(p in root for p in ('/.git', 'lc_headers', 'third_party', '/build')):
        continue
    for f in files:
        if f.lower() == want:
            # prefer the shortest path (topmost match) for a stable choice
            if best is None or len(os.path.join(root, f)) < len(best):
                best = os.path.join(root, f)
print(best or '')
PY
)
  if [ -n "$target" ]; then
    ln -sf "$(relpath "$target")" "dsp/linux/lc_headers/$name"
    made=$((made+1))
  else
    echo "  no source for: $name"
    missing=$((missing+1))
  fi
done < /tmp/lc_names.txt

echo "linked: $made   unresolved: $missing"
echo "broken symlinks: $(find dsp/linux/lc_headers -type l ! -exec test -e {} \; -print | wc -l)"
echo "absolute symlinks: $(find dsp/linux/lc_headers -type l -lname '/*' | wc -l)"
