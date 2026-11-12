# Symbiosic

Browse local Claude Code and Codex sessions. Inspect files, review work with AI, and discuss findings.

[![macOS Apple silicon](https://img.shields.io/badge/macOS-Apple_silicon-276749?style=for-the-badge&logo=apple)](https://github.com/javiabellan/symbiosic/releases/latest/download/symbiosic-macos-arm64.dmg)
[![macOS Intel](https://img.shields.io/badge/macOS-Intel-276749?style=for-the-badge&logo=apple)](https://github.com/javiabellan/symbiosic/releases/latest/download/symbiosic-macos-x86_64.dmg)
[![Linux x86_64](https://img.shields.io/badge/Linux-x86__64-276749?style=for-the-badge&logo=linux)](https://github.com/javiabellan/symbiosic/releases/latest/download/symbiosic-linux-x86_64.tar.gz)
[![Windows x86_64](https://img.shields.io/badge/Windows-x86__64-276749?style=for-the-badge&logo=windows)](https://github.com/javiabellan/symbiosic/releases/latest/download/symbiosic-windows-x86_64.zip)

## Install

- **macOS:** open the DMG; drag Symbiosic into Applications. Builds are ad-hoc signed, without Apple notarization.
- **Linux:** extract; run `symbiosic/install.sh`, then launch Symbiosic. Requires glibc 2.35+.
- **Windows:** extract; run `Symbiosic/Symbiosic.exe`. Keep the bundled files together.

## Use

1. Launch; the workspace opens in your browser.
2. Select a directory or session.
3. Set provider keys in **Settings**. Choose provider → model → thinking.
4. Select **Estimate analysis**, then **Start analysis**. Ask follow-up questions in **Chat**.

Browsing stays local. Analysis sends selected evidence to your provider and may incur charges. Keys entered in Settings last until restart.

[Build, run, and test from source](src/README.md) · [Releases and checksums](https://github.com/javiabellan/symbiosic/releases)
