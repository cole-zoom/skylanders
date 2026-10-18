#!/usr/bin/env bash
# Run the spike in real Chrome (not Node) and print the results.
# Chrome logs display errors to stderr on headless macOS; ignore them.
set -euo pipefail
cd "$(dirname "$0")"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
[ -x "$CHROME" ] || { echo "Chrome not found at $CHROME" >&2; exit 1; }
ITERS="${1:-30000000}"; TRIALS="${2:-7}"
node make-bench-html.mjs "$ITERS" "$TRIALS" >/dev/null
PROFILE=$(mktemp -d); trap 'rm -rf "$PROFILE"' EXIT
"$CHROME" --headless --disable-gpu --no-sandbox --dump-dom \
  --user-data-dir="$PROFILE" "file://$PWD/bench.html" 2>/dev/null > "$PROFILE/dom.html"
# Extract across newlines -- sed cannot, which silently produced empty output.
python3 -c "
import re,html,sys
d=open('$PROFILE/dom.html').read()
m=re.search(r'<pre[^>]*>(.*?)</pre>', d, re.S)
sys.exit('no results in DOM') if not m else print(html.unescape(m.group(1)))
"
