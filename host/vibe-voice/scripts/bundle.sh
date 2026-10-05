#!/usr/bin/env bash
# Build the release binary and assemble a signed VibeVoice.app.
# Usage: scripts/bundle.sh [output-dir]   (default: target/bundle)
#
# Environment (all optional; the default is a host-arch, ad-hoc signed app):
#   VV_SIGN_IDENTITY  keychain signing identity (default "-": ad hoc)
#   VV_UNIVERSAL=1    build arm64 + x86_64 and lipo them into one binary
#   VV_HARDENED=1     sign for distribution: hardened runtime + secure timestamp
#   VV_VERSION        set CFBundleShortVersionString / CFBundleVersion (e.g. 0.1.0)
set -euo pipefail

crate_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out_dir="${1:-${crate_dir}/target/bundle}"
app="${out_dir}/VibeVoice.app"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "bundle.sh: macOS only" >&2
    exit 1
fi

manifest="${crate_dir}/Cargo.toml"
if [[ "${VV_UNIVERSAL:-0}" == "1" ]]; then
    for target in aarch64-apple-darwin x86_64-apple-darwin; do
        cargo build --release --manifest-path "${manifest}" --target "${target}"
    done
    binary="${crate_dir}/target/universal/vibe-voice"
    mkdir -p "$(dirname -- "${binary}")"
    lipo -create -output "${binary}" \
        "${crate_dir}/target/aarch64-apple-darwin/release/vibe-voice" \
        "${crate_dir}/target/x86_64-apple-darwin/release/vibe-voice"
else
    cargo build --release --manifest-path "${manifest}"
    binary="${crate_dir}/target/release/vibe-voice"
fi

rm -rf "${app}"
mkdir -p "${app}/Contents/MacOS" "${app}/Contents/Resources"
cp "${crate_dir}/macos/Info.plist" "${app}/Contents/Info.plist"
cp "${binary}" "${app}/Contents/MacOS/vibe-voice"
if [[ -n "${VV_VERSION:-}" ]]; then
    plutil -replace CFBundleShortVersionString -string "${VV_VERSION}" "${app}/Contents/Info.plist"
    plutil -replace CFBundleVersion -string "${VV_VERSION}" "${app}/Contents/Info.plist"
fi
plutil -lint "${app}/Contents/Info.plist" >/dev/null

# Ad-hoc signatures change on every build, so macOS forgets the Accessibility
# grant. Set VV_SIGN_IDENTITY to a keychain signing identity to keep it.
# Distribution builds (VV_HARDENED=1) need the hardened runtime and a secure
# timestamp for notarization. No entitlements are needed: Bluetooth and Speech
# are granted through Info.plist usage strings and TCC, Accessibility through
# TCC, and the app uses no hardened-runtime exception (no JIT, no Apple Events).
sign_args=(--force --sign "${VV_SIGN_IDENTITY:--}" --identifier cn.folotoy.vibevoice)
if [[ "${VV_HARDENED:-0}" == "1" ]]; then
    sign_args+=(--options runtime --timestamp)
fi
codesign "${sign_args[@]}" "${app}"
codesign --verify --strict "${app}"

echo "${app}"
