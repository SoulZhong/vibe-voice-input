#!/usr/bin/env bash
# Publish a Vibe Voice firmware GitHub Release: the merged AI Passport image.
# The computer side ships in Voice Notes; the notes link its download page.
#
# Usage: tools/release_firmware.sh [--dry-run] <version>   (e.g. v0.2.0)
#
# Needs: a clean tree, an activated ESP-IDF 5.5.3 environment and `gh` logged in.
# The tag is created locally before the build, so the firmware reports exactly
# <version>. --dry-run builds and writes the notes but skips the tag push and
# the release upload (and deletes a tag it created).
set -euo pipefail

dry_run=0
version=""
for arg in "$@"; do
    case "${arg}" in
    --dry-run) dry_run=1 ;;
    -h | --help)
        sed -n '2,10p' "$0"
        exit 0
        ;;
    *) version="${arg}" ;;
    esac
done
if [[ ! "${version}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.]+)?$ ]]; then
    echo "release_firmware.sh: give a version like v0.2.0" >&2
    exit 2
fi

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
gh_repo="SoulZhong/vibe-voice-input"
voice_notes="https://github.com/SoulZhong/voice-notes/releases/latest"
out="${repo_root}/build/release/${version}"
fw_name="vibe-voice-input-${version}-full.bin"

step() { printf '\n==> %s\n' "$*"; }
run() {
    printf '+ %s\n' "$*"
    "$@"
}
skip() { printf '[dry-run] skipped: %s\n' "$*"; }
die() {
    echo "release_firmware.sh: $*" >&2
    exit 1
}

cd "${repo_root}"
if [[ -n "$(git status --porcelain)" ]]; then
    if [[ "${dry_run}" == 1 ]]; then
        echo "warning: the working tree is not clean (allowed for --dry-run)"
    else
        die "the working tree is not clean; commit or stash first"
    fi
fi
command -v idf.py >/dev/null || die "activate ESP-IDF 5.5.3 first (. ~/esp/esp-idf-v5.5.3/export.sh)"
[[ "${dry_run}" == 1 ]] || gh auth status >/dev/null 2>&1 || die "log in with gh auth login first"

step "Tag ${version}"
made_tag=0
if git rev-parse -q --verify "refs/tags/${version}" >/dev/null; then
    [[ "$(git rev-parse "${version}^{commit}")" == "$(git rev-parse HEAD)" ]] ||
        die "tag ${version} exists on another commit"
else
    run git tag -a "${version}" -m "Vibe Voice ${version}"
    made_tag=1
fi
if [[ "${dry_run}" == 1 && "${made_tag}" == 1 ]]; then
    trap 'git tag -d "${version}" >/dev/null' EXIT
fi

rm -rf "${out}"
mkdir -p "${out}"

step "Firmware: full gate build"
run ./tools/validate.sh --firmware
run cp build/vibe-voice-input-full.bin "${out}/${fw_name}"

step "Checksums and notes"
(cd "${out}" && run shasum -a 256 "${fw_name}" | tee SHA256SUMS.txt)
notes="${out}/notes.md"
cat >"${notes}" <<EOF
## Vibe Voice ${version}

### Downloads

- \`${fw_name}\`: AI Passport (ESP32-C3) firmware, the merged image. Flash it at
  offset \`0x0\`, for example \`esptool.py --chip esp32c3 write_flash 0x0 ${fw_name}\`.
- \`SHA256SUMS.txt\`: SHA-256 of the image.

### Computer side

Install [Voice Notes](${voice_notes}) (macOS on Apple silicon, Windows x64).
It connects to the Device, recognizes speech and inserts the text. On macOS
type the 6-digit passkey from the Device screen into the system pairing prompt;
on Windows type it into Voice Notes.

### 电脑端

请安装 [Voice Notes](${voice_notes})（macOS Apple 芯片、Windows x64），由它连接设备、识别语音并插入文字。
配对时，macOS 在系统弹框里输入设备屏幕上的 6 位配对码，Windows 在 Voice Notes 里输入。
EOF
echo "notes: ${notes}"

step "Push the tag and create the GitHub Release"
if [[ "${dry_run}" == 1 ]]; then
    skip "git push origin ${version}"
    skip "gh release create ${version} --repo ${gh_repo} --title 'Vibe Voice ${version}' --notes-file ${notes} ${out}/${fw_name} ${out}/SHA256SUMS.txt"
else
    run git push origin "${version}"
    run gh release create "${version}" --repo "${gh_repo}" --title "Vibe Voice ${version}" \
        --notes-file "${notes}" "${out}/${fw_name}" "${out}/SHA256SUMS.txt"
fi

step "Done"
ls -l "${out}"
