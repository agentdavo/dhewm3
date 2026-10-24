#!/usr/bin/env bash
# Vendor miniaudio (single header) — gitignored, fetched on demand like gl4es.
set -euo pipefail
cd "$(dirname "$0")"
curl -fsSL https://raw.githubusercontent.com/mackron/miniaudio/refs/heads/master/miniaudio.h -o miniaudio.h
echo "fetched miniaudio.h ($(wc -l < miniaudio.h) lines)"
