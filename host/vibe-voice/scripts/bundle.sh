#!/usr/bin/env bash
# Build the release binary and assemble an ad-hoc signed VibeVoice.app.
# Usage: scripts/bundle.sh [output-dir]   (default: target/bundle)
set -euo pipefail

crate_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out_dir="${1:-${crate_dir}/target/bundle}"
app="${out_dir}/VibeVoice.app"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "bundle.sh: macOS only" >&2
    exit 1
fi

cargo build --release --manifest-path "${crate_dir}/Cargo.toml"

rm -rf "${app}"
mkdir -p "${app}/Contents/MacOS" "${app}/Contents/Resources"
cp "${crate_dir}/macos/Info.plist" "${app}/Contents/Info.plist"
cp "${crate_dir}/target/release/vibe-voice" "${app}/Contents/MacOS/vibe-voice"
plutil -lint "${app}/Contents/Info.plist" >/dev/null

# Ad-hoc signature: enough for local use. macOS ties Accessibility and other
# permissions to the signature, so re-grant them after rebuilding.
codesign --force --sign - --identifier cn.folotoy.vibevoice "${app}"
codesign --verify --strict "${app}"

echo "${app}"
