#!/usr/bin/env bash
# Download a pinned repository tree as a tarball through gh.
#
# curl and git (OpenSSL) get their TLS handshakes reset by the GFW on every
# github.com/codeload request, while gh (Go TLS) passes, so the transfer runs
# through gh api.  Downloads are validated with gzip -t and retried; a truncated
# stream fails the test and the attempt restarts.
set -uo pipefail

REPO=$1
REF=$2
OUT=$3
LOG=/tmp/gh-download.log
PROXY=${GH_PROXY:-http://127.0.0.1:7890}

log() { echo "[$(date -Is)] $*" >> "$LOG"; }

for attempt in $(seq 1 60); do
  rm -f "$OUT"
  if HTTPS_PROXY="$PROXY" timeout 900 gh api "repos/$REPO/tarball/$REF" > "$OUT" 2>>"$LOG"; then
    if gzip -t "$OUT" 2>/dev/null; then
      log "OK $REPO@$REF -> $OUT ($(stat -c%s "$OUT") bytes, attempt $attempt)"
      exit 0
    fi
    log "truncated gzip for $REPO@$REF (attempt $attempt, $(stat -c%s "$OUT" 2>/dev/null) bytes)"
  else
    log "gh api failed for $REPO@$REF (attempt $attempt)"
  fi
  rm -f "$OUT"
  sleep 12
done

log "GAVE UP on $REPO@$REF"
exit 1
