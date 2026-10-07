#!/usr/bin/env bash
# Publish a GitHub Release: a notarized Apple silicon VibeVoice DMG + Device firmware.
#
# Usage: host/vibe-voice/scripts/release.sh [--dry-run] <version>   (e.g. v0.1.0)
#
# Needs: a clean tree, an activated ESP-IDF 5.5.3 environment (firmware),
# VV_SIGN_IDENTITY = a "Developer ID Application" identity, a notarytool
# keychain profile (NOTARY_PROFILE, default voice-notes-notary) and `gh` logged in.
# --dry-run builds, signs, packs the DMG and checks everything but skips notarization,
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
app_dmg="VibeVoice-${version}-macos-arm64.dmg"
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

step "Companion: Apple silicon, hardened, Developer ID signed VibeVoice.app"
[[ "$(uname -m)" == "arm64" ]] || die "build the Apple silicon release on an Apple silicon Mac"
run env VV_HARDENED=1 VV_VERSION="${plain}" VV_SIGN_IDENTITY="${identity}" \
    "${crate_dir}/scripts/bundle.sh" "${out}"
app="${out}/VibeVoice.app"
run lipo -info "${app}/Contents/MacOS/vibe-voice"
archs="$(lipo -archs "${app}/Contents/MacOS/vibe-voice")"
[[ "${archs}" == "arm64" ]] || die "the binary is not arm64 only (${archs})"
run codesign -dv --verbose=2 "${app}"
sig="$(codesign -dv "${app}" 2>&1)"
[[ "${sig}" == *"(runtime)"* ]] || die "hardened runtime flag missing"
run codesign --verify --strict --deep "${app}"

step "DMG: VibeVoice.app beside an Applications link"
stage="${out}/dmg"
rm -rf "${stage}"
mkdir -p "${stage}"
run ditto "${app}" "${stage}/VibeVoice.app"
run ln -s /Applications "${stage}/Applications"
run hdiutil create -volname "Vibe Voice" -srcfolder "${stage}" -ov -format UDZO "${out}/${app_dmg}"
rm -rf "${stage}"
run codesign --force --sign "${identity}" --timestamp "${out}/${app_dmg}"

step "Notarize and staple"
if [[ "${dry_run}" == 1 ]]; then
    skip "xcrun notarytool submit ${out}/${app_dmg} --keychain-profile ${notary_profile} --wait"
    skip "xcrun stapler staple ${out}/${app_dmg}"
    printf '+ spctl -a -vv -t open --context context:primary-signature %s   (expected to fail until notarized)\n' "${out}/${app_dmg}"
    spctl -a -vv -t open --context context:primary-signature "${out}/${app_dmg}" || true
else
    # Notarizing the DMG covers the app inside it; the ticket is stapled to
    # the DMG so the first open works offline.
    run xcrun notarytool submit "${out}/${app_dmg}" --keychain-profile "${notary_profile}" --wait
    run xcrun stapler staple "${out}/${app_dmg}"
    run xcrun stapler validate "${out}/${app_dmg}"
    run spctl -a -vv -t open --context context:primary-signature "${out}/${app_dmg}"
fi

step "Checksums and notes"
(cd "${out}" && run shasum -a 256 "${app_dmg}" "${fw_name}" | tee SHA256SUMS.txt)
notes="${out}/notes.md"
cat >"${notes}" <<EOF
## Vibe Voice ${version}

### Downloads

- \`${fw_name}\`: AI Passport (ESP32-C3) firmware, the merged image. Flash it at
  offset \`0x0\`, for example \`esptool.py --chip esp32c3 write_flash 0x0 ${fw_name}\`.
- \`${app_dmg}\`: the macOS Companion for Apple silicon Macs (macOS 13 or
  later), signed with Developer ID and notarized. Open the DMG, drag
  \`VibeVoice\` onto Applications and open it; it registers itself to launch
  at login.
- \`SHA256SUMS.txt\`: SHA-256 of both files.

See the README for pairing and the macOS permissions (Bluetooth, Speech
Recognition, Accessibility).
EOF
echo "notes: ${notes}"

step "Tag and GitHub Release"
if [[ "${dry_run}" == 1 ]]; then
    skip "git tag -a ${version} -m 'Vibe Voice ${version}' (if missing)"
    skip "git push origin ${version}"
    skip "gh release create ${version} --repo ${gh_repo} --title 'Vibe Voice ${version}' --notes-file ${notes} ${out}/${app_dmg} ${out}/${fw_name} ${out}/SHA256SUMS.txt"
else
    if ! git rev-parse -q --verify "refs/tags/${version}" >/dev/null; then
        run git tag -a "${version}" -m "Vibe Voice ${version}"
    fi
    run git push origin "${version}"
    run gh release create "${version}" --repo "${gh_repo}" --title "Vibe Voice ${version}" \
        --notes-file "${notes}" "${out}/${app_dmg}" "${out}/${fw_name}" "${out}/SHA256SUMS.txt"
fi

step "Done"
ls -l "${out}"
