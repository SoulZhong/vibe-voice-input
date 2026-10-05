#!/usr/bin/env bash
# Publish a GitHub Release: notarized universal VibeVoice.app + Device firmware.
#
# Usage: host/vibe-voice/scripts/release.sh [--dry-run] <version>   (e.g. v0.1.0)
#
# Needs: a clean tree, an activated ESP-IDF 5.5.3 environment (firmware),
# VV_SIGN_IDENTITY = a "Developer ID Application" identity, a notarytool
# keychain profile (NOTARY_PROFILE, default voice-notes-notary) and `gh` logged in.
# --dry-run builds, signs, zips and checks everything but skips notarization,
# the tag, the push and the release upload.
set -euo pipefail

dry_run=0
version=""
for arg in "$@"; do
    case "${arg}" in
    --dry-run) dry_run=1 ;;
    -h | --help)
        sed -n '2,11p' "$0"
        exit 0
        ;;
    *) version="${arg}" ;;
    esac
done
if [[ ! "${version}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.]+)?$ ]]; then
    echo "release.sh: give a version like v0.1.0" >&2
    exit 2
fi
plain="${version#v}"

crate_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd -- "${crate_dir}/../.." && pwd)"
gh_repo="SoulZhong/vibe-voice-input"
notary_profile="${NOTARY_PROFILE:-voice-notes-notary}"
out="${repo_root}/build/release/${version}"
app_zip="VibeVoice-${version}-macos-universal.zip"
fw_name="vibe-voice-input-${version}-full.bin"

step() { printf '\n==> %s\n' "$*"; }
run() {
    printf '+ %s\n' "$*"
    "$@"
}
skip() { printf '[dry-run] skipped: %s\n' "$*"; }
die() {
    echo "release.sh: $*" >&2
    exit 1
}

[[ "$(uname -s)" == "Darwin" ]] || die "macOS only"
identity="${VV_SIGN_IDENTITY:-}"
[[ -n "${identity}" && "${identity}" != "-" ]] || die "set VV_SIGN_IDENTITY to a Developer ID Application identity (ad hoc cannot be notarized)"
security find-identity -v -p codesigning | grep -F "${identity}" | grep -q "Developer ID Application" ||
    die "VV_SIGN_IDENTITY ${identity} is not a valid Developer ID Application identity"

cd "${repo_root}"
if [[ -n "$(git status --porcelain)" ]]; then
    if [[ "${dry_run}" == 1 ]]; then
        echo "warning: the working tree is not clean (allowed for --dry-run)"
    else
        die "the working tree is not clean; commit or stash first"
    fi
fi

rm -rf "${out}"
mkdir -p "${out}"

step "Firmware: full gate build"
command -v idf.py >/dev/null || die "activate ESP-IDF 5.5.3 first (. ~/esp/esp-idf-v5.5.3/export.sh)"
run ./tools/validate.sh --firmware
run cp build/vibe-voice-input-full.bin "${out}/${fw_name}"

step "Companion: universal, hardened, Developer ID signed VibeVoice.app"
run env VV_UNIVERSAL=1 VV_HARDENED=1 VV_VERSION="${plain}" VV_SIGN_IDENTITY="${identity}" \
    "${crate_dir}/scripts/bundle.sh" "${out}"
app="${out}/VibeVoice.app"
run lipo -info "${app}/Contents/MacOS/vibe-voice"
archs="$(lipo -archs "${app}/Contents/MacOS/vibe-voice")"
[[ "${archs}" == *x86_64* && "${archs}" == *arm64* ]] || die "the binary is not universal (${archs})"
run codesign -dv --verbose=2 "${app}"
sig="$(codesign -dv "${app}" 2>&1)"
[[ "${sig}" == *"(runtime)"* ]] || die "hardened runtime flag missing"
run codesign --verify --strict --deep "${app}"

step "Zip"
(cd "${out}" && run ditto -c -k --keepParent VibeVoice.app "${app_zip}")

step "Notarize and staple"
if [[ "${dry_run}" == 1 ]]; then
    skip "xcrun notarytool submit ${out}/${app_zip} --keychain-profile ${notary_profile} --wait"
    skip "xcrun stapler staple ${app}"
    skip "re-zip the stapled app"
    printf '+ spctl -a -vv -t exec %s   (expected to fail until notarized)\n' "${app}"
    spctl -a -vv -t exec "${app}" || true
else
    run xcrun notarytool submit "${out}/${app_zip}" --keychain-profile "${notary_profile}" --wait
    run xcrun stapler staple "${app}"
    run xcrun stapler validate "${app}"
    rm -f "${out}/${app_zip}"
    (cd "${out}" && run ditto -c -k --keepParent VibeVoice.app "${app_zip}")
    run spctl -a -vv -t exec "${app}"
fi

step "Checksums and notes"
(cd "${out}" && run shasum -a 256 "${app_zip}" "${fw_name}" | tee SHA256SUMS.txt)
notes="${out}/notes.md"
cat >"${notes}" <<EOF
## Vibe Voice ${version}

### Downloads

- \`${fw_name}\`: AI Passport (ESP32-C3) firmware, the merged image. Flash it at
  offset \`0x0\`, for example \`esptool.py --chip esp32c3 write_flash 0x0 ${fw_name}\`.
- \`${app_zip}\`: the macOS Companion (macOS 13 or later, Apple Silicon and Intel),
  signed with Developer ID and notarized. Unzip, move \`VibeVoice.app\` to
  Applications and open it; it registers itself to launch at login.
- \`SHA256SUMS.txt\`: SHA-256 of both files.

See the README for pairing and the macOS permissions (Bluetooth, Speech
Recognition, Accessibility).
EOF
echo "notes: ${notes}"

step "Tag and GitHub Release"
if [[ "${dry_run}" == 1 ]]; then
    skip "git tag -a ${version} -m 'Vibe Voice ${version}' (if missing)"
    skip "git push origin ${version}"
    skip "gh release create ${version} --repo ${gh_repo} --title 'Vibe Voice ${version}' --notes-file ${notes} ${out}/${app_zip} ${out}/${fw_name} ${out}/SHA256SUMS.txt"
else
    if ! git rev-parse -q --verify "refs/tags/${version}" >/dev/null; then
        run git tag -a "${version}" -m "Vibe Voice ${version}"
    fi
    run git push origin "${version}"
    run gh release create "${version}" --repo "${gh_repo}" --title "Vibe Voice ${version}" \
        --notes-file "${notes}" "${out}/${app_zip}" "${out}/${fw_name}" "${out}/SHA256SUMS.txt"
fi

step "Done"
ls -l "${out}"
