#!/bin/sh
set -eu
# Pinned archives are verified before extraction; Intel macOS builds the same source.
version=v0.9.1
dest=${1:-.deps}
platform=$(uname -s)-$(uname -m)
archive=$(mktemp)
work=$(mktemp -d)
trap 'rm -f "$archive"; rm -rf "$work"' EXIT HUP INT TERM
case "$platform" in
 Darwin-arm64) asset=macos-aarch64.tar.gz; digest=95b740f55bb63fe49b20dd0305aa9f1046645cc2b5d3c32a82b42aabf8b3fb77 ;;
 Linux-x86_64) asset=linux-x86_64.tar.gz; digest=0b2c5c462602a7e95703e93a6d84222b20172e2d36ff7ece555fd2ff776f9a26 ;;
 MSYS*-x86_64) asset=windows-x86_64.zip; digest=c4ee3ae6fad481b31fb19f5f080164858fa373fe0d656d62291a0228609bc16c ;;
 Darwin-x86_64) asset=source; digest=db891d87e6453492ce01cb03c74014552487bc82d881682f83295d5badd0e4e7 ;;
 *) echo 'Unsupported platform. See src/README.md.' >&2; exit 1 ;;
esac
url="https://github.com/kojix2/tiktoken-c/releases/download/$version/tiktoken-c-$version-$asset"
if [ "$asset" = source ]; then url="https://github.com/kojix2/tiktoken-c/archive/refs/tags/$version.tar.gz"; fi
curl --fail --location --proto '=https' --tlsv1.2 "$url" -o "$archive"
if command -v sha256sum >/dev/null 2>&1; then actual=$(sha256sum "$archive" | cut -d ' ' -f1); else actual=$(shasum -a 256 "$archive" | cut -d ' ' -f1); fi
[ "$actual" = "$digest" ] || { echo 'Tokenizer checksum mismatch' >&2; exit 1; }
mkdir -p "$dest"
case "$asset" in
 source)
  tar -xzf "$archive" -C "$work"
  (cd "$work/tiktoken-c-0.9.1" && cargo build --release)
  cp "$work/tiktoken-c-0.9.1/target/release/libtiktoken_c.a" "$dest/"
  cp "$work/tiktoken-c-0.9.1/tiktoken.h" "$dest/"
  cp "$work/tiktoken-c-0.9.1/LICENSE.txt" "$dest/LICENSE.txt"
  ;;
 *.zip)
  unzip -q "$archive" -d "$dest"
  # Only these C ABI functions cross the MSYS/native DLL boundary.
  cat > "$work/tiktoken.def" <<'DEF'
LIBRARY tiktoken_c.dll
EXPORTS
 tiktoken_o200k_base
 tiktoken_destroy_corebpe
 tiktoken_corebpe_count_ordinary
DEF
  dlltool -d "$work/tiktoken.def" -l "$dest/libtiktoken_c.a"
  ;;
 *) tar -xzf "$archive" -C "$dest" ;;
esac
