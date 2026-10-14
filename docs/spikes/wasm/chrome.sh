#!/bin/bash
# Opens serve.mjs's page in headless Chrome (muted, throwaway profile) for SECONDS, then quits it.
SECS=${1:-90}
PROFILE=$(mktemp -d /tmp/wasmspike-chrome.XXXXXX)
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --user-data-dir="$PROFILE" \
  --no-first-run --mute-audio http://127.0.0.1:8123/page.html > "$PROFILE.log" 2>&1 &
PID=$!
sleep "$SECS"
kill $PID
wait $PID 2>/dev/null
rm -rf "$PROFILE"
