#!/usr/bin/env bash
# Build VibeVoice.app and install it to /Applications (or ~/Applications when
# /Applications is not writable), replacing an older copy, then start it.
# An installed copy registers itself to launch at login (menu: 开机自启动).
#
# Usage: scripts/install.sh
# Environment: VV_SIGN_IDENTITY (passed to bundle.sh; a stable identity keeps
# the Accessibility grant across reinstalls).
set -euo pipefail

crate_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "install.sh: macOS only" >&2
    exit 1
fi

built="$("${crate_dir}/scripts/bundle.sh" | tail -n 1)"

dest_dir="/Applications"
if [[ ! -w "${dest_dir}" ]]; then
    dest_dir="${HOME}/Applications"
    mkdir -p "${dest_dir}"
fi
dest="${dest_dir}/VibeVoice.app"

# Quit a running copy (installed or not) so the new one can start.
if pkill -f "VibeVoice.app/Contents/MacOS/vibe-voice" 2>/dev/null; then
    echo "stopped the running Vibe Voice"
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        pgrep -f "VibeVoice.app/Contents/MacOS/vibe-voice" >/dev/null || break
        sleep 0.3
    done
fi

rm -rf "${dest}"
ditto "${built}" "${dest}"
codesign --verify --strict "${dest}"

open "${dest}"
echo "installed ${dest} (launches at login; turn it off in the menu bar: VV > 开机自启动)"
