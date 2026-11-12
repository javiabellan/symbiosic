#!/usr/bin/env python3
"""Create self-contained OS packages. Only build outputs enter the distribution."""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
import zipfile
import zlib

SRC = Path(__file__).resolve().parent.parent
APP = 'Symbiosic'
SYSTEM_LIBS = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1', 'ld-linux-x86-64.so.2'}

def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True)

def png(size):
    # Rasterize the same two interlocking strokes as assets/icon.svg without a build dependency.
    scale = 256 / size
    pixels = bytearray()
    for y in range(size):
        pixels.append(0)
        for x in range(size):
            px, py = (x + .5) * scale, (y + .5) * scale
            dx, dy = max(abs(px - 128) - 72, 0), max(abs(py - 128) - 72, 0)
            color = (39, 103, 73, 255) if dx*dx + dy*dy <= 56*56 else (0, 0, 0, 0)
            horizontal = min(((px - min(max(px, lo), hi))**2 + (py - yy)**2)**.5 for lo,hi,yy in [(68,138,76),(118,138,148),(118,188,180),(118,138,108)])
            arc1 = abs(((px-138)**2+(py-112)**2)**.5 - 36) if px >= 138 else 256
            arc2 = abs(((px-118)**2+(py-144)**2)**.5 - 36) if px <= 118 else 256
            if min(horizontal, arc1, arc2) <= 9:
                color = (255,255,255,255)
            if min((px-68)**2+(py-76)**2, (px-188)**2+(py-180)**2) <= 13**2:
                color = (184,216,197,255)
            pixels.extend(color)
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind+data))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',size,size,8,6,0,0,0)) + chunk(b'IDAT',zlib.compress(pixels)) + chunk(b'IEND',b'')

def copy_assets(root, binary):
    (root/'bin').mkdir(parents=True)
    shutil.copy2(binary, root/'bin'/binary.name)
    for name in ['web','prompts','assets']:
        shutil.copytree(SRC/name, root/name)
    for name in ['tools.json','rubric.json','env.template']:
        shutil.copy2(SRC/name, root/name)
    licenses = root/'licenses'
    licenses.mkdir()
    shutil.copy2(SRC/'.deps/LICENSE.txt', licenses/'tiktoken.txt')
    for dependency in ['yyjson','llhttp']:
        source = SRC/'build/_deps'/f'{dependency}-src'
        for name in ['LICENSE','LICENSE-MIT']:
            if (source/name).exists():
                shutil.copy2(source/name, licenses/f'{dependency}.txt')
                break
    (licenses/'SOURCES.txt').write_text('Dependency sources and licenses: https://github.com/javiabellan/symbiosic/tree/main/src/README.md#dependencies\n')
    (root/'README.txt').write_text('Symbiosic\nBrowse local Claude Code and Codex sessions.\nLaunch the app, then use the browser workspace.\nSettings: provider keys and model defaults.\nAnalysis sends selected content to your provider.\nhttps://github.com/javiabellan/symbiosic\n')

def mac_libs(binary, directory):
    directory.mkdir()
    pending = [binary]
    copied = {}
    while pending:
        target = pending.pop()
        for line in run('otool','-L',target).splitlines()[1:]:
            source = line.strip().split(' (')[0]
            if not source.startswith('/') or source.startswith(('/usr/lib/','/System/')):
                continue
            name = Path(source).name
            if name not in copied:
                copied[name] = source
                shutil.copy2(source, directory/name)
                os.chmod(directory/name, 0o755)
                pending.append(directory/name)
                run('install_name_tool','-id','@loader_path/'+name,directory/name)
            elif copied[name] != source:
                raise RuntimeError('Conflicting library names: '+name)
            reference = '@loader_path/../lib/'+name if target == binary else '@loader_path/'+name
            run('install_name_tool','-change',source,reference,target)
    for target in [*directory.iterdir(), binary]:
        run('codesign','--force','--sign','-',target)

def macos(stage, out, target, binary):
    app = stage/(APP+'.app')
    contents = app/'Contents'
    resources = contents/'Resources'
    copy_assets(resources,binary)
    mac_libs(resources/'bin'/binary.name, resources/'lib')
    native = contents/'MacOS'
    native.mkdir()
    run('clang','-fobjc-arc','-framework','Cocoa',SRC/'packaging/macos.m','-o',native/APP)
    iconset = stage/'AppIcon.iconset'
    iconset.mkdir()
    for size in [16,32,128,256,512]:
        for multiplier in [1,2]:
            suffix = '@2x' if multiplier == 2 else ''
            (iconset/f'icon_{size}x{size}{suffix}.png').write_bytes(png(size*multiplier))
    run('iconutil','-c','icns',iconset,'-o',resources/'AppIcon.icns')
    with (contents/'Info.plist').open('wb') as file:
        plistlib.dump({'CFBundleName':APP,'CFBundleDisplayName':APP,'CFBundleIdentifier':'io.github.javiabellan.symbiosic','CFBundleVersion':'1','CFBundleShortVersionString':'0.1.0','CFBundlePackageType':'APPL','CFBundleExecutable':APP,'CFBundleIconFile':'AppIcon','LSMinimumSystemVersion':'14.0','NSHighResolutionCapable':True},file)
    run('codesign','--force','--deep','--sign','-',app)
    image = stage/'image'
    image.mkdir()
    shutil.move(app,image/app.name)
    (image/'Applications').symlink_to('/Applications')
    artifact = out/f'symbiosic-{target}.dmg'
    run('hdiutil','create','-volname',APP,'-srcfolder',image,'-ov','-format','UDZO',artifact)
    return artifact

def linux(stage, out, target, binary):
    root = stage/'symbiosic'
    copy_assets(root,binary)
    libs = root/'lib'
    libs.mkdir()
    # Bundle the dependency closure; use the target system's glibc and dynamic loader.
    for line in run('ldd',binary).splitlines():
        match = re.search(r'=> (/\S+)',line)
        if not match or Path(match[1]).name in SYSTEM_LIBS:
            continue
        shutil.copy2(match[1],libs/Path(match[1]).name)
    launcher = root/'AppRun'
    launcher.write_text('''#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$base/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DATA_DIR="${DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/symbiosic}"
cd "$base"
log=$(mktemp)
./bin/symbiosic > "$log" &
server=$!
trap 'kill "$server" 2>/dev/null || true; rm -f "$log"' EXIT HUP INT TERM
while kill -0 "$server" 2>/dev/null; do
 url=$(sed -n '1p' "$log")
 if [ -n "$url" ]; then
  printf '%s\\n' "$url"
  xdg-open "$url" >/dev/null 2>&1 &
  break
 fi
 sleep .2
done
wait "$server"
''')
    launcher.chmod(0o755)
    install = root/'install.sh'
    install.write_text('''#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
cat > "$apps/symbiosic.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Symbiosic
Comment=Browse and review AI sessions
Exec="$base/AppRun"
Icon=$base/assets/icon.svg
Terminal=false
Categories=Development;
EOF
''')
    install.chmod(0o755)
    artifact = out/f'symbiosic-{target}.tar.gz'
    with tarfile.open(artifact,'w:gz') as file:
        file.add(root,arcname='symbiosic')
    return artifact

def windows(stage, out, target, binary):
    root = stage/'Symbiosic'
    copy_assets(root,binary)
    shutil.copy2(SRC/'.deps/tiktoken_c.dll',root/'bin/tiktoken_c.dll')
    shutil.copy2('/etc/ssl/certs/ca-bundle.crt', root/'assets/cacert.pem')
    pending = [root/'bin'/binary.name,root/'bin/tiktoken_c.dll']
    copied = set()
    while pending:
        library = pending.pop()
        for name in re.findall(r'DLL Name: (\S+)', run('objdump','-p',library)):
            if name.lower() in copied:
                continue
            copied.add(name.lower())
            source = next((p/name for p in [Path('/usr/bin'),Path('/mingw64/bin')] if (p/name).exists()),None)
            if source:
                shutil.copy2(source,root/'bin'/name)
                pending.append(root/'bin'/name)
    images = [png(size) for size in [16,32,48,256]]
    offset = 6 + 16*len(images)
    entries = bytearray(struct.pack('<HHH',0,1,len(images)))
    for size,data in zip([16,32,48,256],images):
        entries.extend(struct.pack('<BBBBHHII',size%256,size%256,0,0,1,32,len(data),offset))
        offset += len(data)
    icon = stage/'icon.ico'
    icon.write_bytes(entries+b''.join(images))
    resource = stage/'icon.rc'
    resource.write_text('1 ICON "'+icon.as_posix()+'"\n')
    run('/mingw64/bin/windres',resource,'-o',stage/'icon.o')
    run('/mingw64/bin/gcc','-municode','-static',SRC/'packaging/windows.c',stage/'icon.o','-lshell32','-o',root/'Symbiosic.exe')
    # Distribute MSYS package notices alongside the runtime.
    notices = Path('/usr/share/licenses')
    if notices.exists():
        shutil.copytree(notices,root/'licenses/msys',dirs_exist_ok=True)
    artifact = out/f'symbiosic-{target}.zip'
    with zipfile.ZipFile(artifact,'w',zipfile.ZIP_DEFLATED) as file:
        for path in root.rglob('*'):
            if path.is_file():
                file.write(path,path.relative_to(stage))
    return artifact

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('target',choices=['macos-arm64','macos-x86_64','linux-x86_64','windows-x86_64'])
    parser.add_argument('--binary',type=Path,default=SRC/'build/symbiosic')
    parser.add_argument('--out',type=Path,default=SRC/'dist')
    args = parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    binary = args.binary.resolve()
    if args.target.startswith('windows') and not binary.exists():
        binary = binary.with_suffix('.exe')
    with tempfile.TemporaryDirectory(prefix='symbiosic-package-') as temporary:
        build = macos if args.target.startswith('macos') else linux if args.target.startswith('linux') else windows
        artifact = build(Path(temporary),args.out.resolve(),args.target,binary)
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    artifact.with_name(artifact.name+'.sha256').write_text(digest+'  '+artifact.name+'\n')
    print(artifact)

if __name__ == '__main__':
    main()
