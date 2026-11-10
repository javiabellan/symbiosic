#!/bin/sh
set -eu
# Pinned upstream release, checksum verified. Only writes to the chosen prefix.
version=v0.9.1
dest=${1:-.deps}
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) platform=macos-aarch64; digest=95b740f55bb63fe49b20dd0305aa9f1046645cc2b5d3c32a82b42aabf8b3fb77 ;;
    Linux-x86_64) platform=linux-x86_64; digest=0b2c5c462602a7e95703e93a6d84222b20172e2d36ff7ece555fd2ff776f9a26 ;;
    *) echo 'Build kojix2/tiktoken-c v0.9.1 with Cargo; set TIKTOKEN_DIR to its header and static library directory.' >&2; exit 1 ;;
esac
archive=$(mktemp)
trap 'rm -f "$archive"' EXIT HUP INT TERM
curl --fail --location --proto '=https' --tlsv1.2 "https://github.com/kojix2/tiktoken-c/releases/download/$version/tiktoken-c-$version-$platform.tar.gz" -o "$archive"
if command -v sha256sum >/dev/null 2>&1; then actual=$(sha256sum "$archive" | cut -d ' ' -f1); else actual=$(shasum -a 256 "$archive" | cut -d ' ' -f1); fi
[ "$actual" = "$digest" ] || { echo 'Tokenizer checksum mismatch' >&2; exit 1; }
mkdir -p "$dest"
tar -xzf "$archive" -C "$dest"
