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
VERSION = '0.1.1'
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
    shutil.copytree(SRC/'packaging/licenses', licenses, dirs_exist_ok=True)
    for dependency in ['yyjson','llhttp']:
        source = SRC/'build/_deps'/f'{dependency}-src'
        for name in ['LICENSE','LICENSE-MIT']:
            if (source/name).exists():
                shutil.copy2(source/name, licenses/f'{dependency}.txt')
                break
    (licenses/'SOURCES.txt').write_text('Dependency sources and licenses: https://github.com/javiabellan/symbiosic/tree/main/src/README.md#dependencies\n')
    (root/'README.txt').write_text('Symbiosic\nBrowse local Claude Code and Codex sessions in a native desktop window.\nClose the window to stop the local server.\nLinux requires GTK 3 and WebKitGTK 4.1; Windows requires Microsoft Edge WebView2 Runtime.\nSettings: provider keys and model defaults.\nAnalysis sends selected content to your provider.\nhttps://github.com/javiabellan/symbiosic\n')

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
            reference = '@loader_path/../Resources/lib/'+name if target == binary else '@loader_path/'+name
            run('install_name_tool','-change',source,reference,target)
    for target in [*directory.iterdir(), binary]:
        run('codesign','--force','--sign','-',target)

def macos(stage, out, target, binary):
    app = stage/(APP+'.app')
    contents = app/'Contents'
    resources = contents/'Resources'
    copy_assets(resources,binary)
    native = contents/'MacOS'
    native.mkdir()
    shutil.move(resources/'bin'/binary.name, native/APP)
    # Keep a headless entry point for package tests and command-line use.
    (resources/'bin'/binary.name).symlink_to('../../MacOS/'+APP)
    mac_libs(native/APP, resources/'lib')
    iconset = stage/'AppIcon.iconset'
    iconset.mkdir()
    for size in [16,32,128,256,512]:
        for multiplier in [1,2]:
            suffix = '@2x' if multiplier == 2 else ''
            (iconset/f'icon_{size}x{size}{suffix}.png').write_bytes(png(size*multiplier))
    run('iconutil','-c','icns',iconset,'-o',resources/'AppIcon.icns')
    # Match the actual executable deployment target rather than guessing an OS version.
    versions = []
    for executable in [native/APP, resources/'bin'/binary.name, *(resources/'lib').iterdir()]:
        versions.extend(tuple(map(int, value.split('.'))) for value in re.findall(r'\bminos\s+([\d.]+)', run('otool', '-l', executable)))
    minimum_os = '.'.join(map(str, max(versions)))
    with (contents/'Info.plist').open('wb') as file:
        plistlib.dump({'CFBundleName':APP,'CFBundleDisplayName':APP,'CFBundleIdentifier':'io.github.javiabellan.symbiosic','CFBundleVersion':VERSION,'CFBundleShortVersionString':VERSION,'CFBundlePackageType':'APPL','CFBundleExecutable':APP,'CFBundleIconFile':'AppIcon','LSMinimumSystemVersion':minimum_os,'NSHighResolutionCapable':True,'NSLocalNetworkUsageDescription':'Symbiosic connects to its local workspace server.','NSAppTransportSecurity':{'NSAllowsLocalNetworking':True}},file)
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
    # WebKitGTK/GTK and their helper processes, resources and drivers come from
    # the OS. Bundling their .so closure alone would produce a broken runtime.
    webview_runtime = ('libwebkit', 'libjavascriptcore', 'libgtk', 'libgdk',
                       'libglib', 'libgobject', 'libgio', 'libgmodule', 'libgthread')
    # ldd lists transitive GUI dependencies too; let their system packages own
    # that closure. Bundle just the backend dependency closure separately.
    backend_libraries = set()
    for module in ['libcurl', 'libuv']:
        directory = Path(run('pkg-config', '--variable=libdir', module).strip())
        name = 'libcurl.so' if module == 'libcurl' else 'libuv.so'
        backend_libraries.add(str((directory/name).resolve()))
    dependency_lines = [line for library in backend_libraries for line in run('ldd', library).splitlines()]
    dependency_lines += [f'=> {library}' for library in backend_libraries]
    for line in dependency_lines:
        match = re.search(r'=> (/\S+)',line)
        if not match or Path(match[1]).name in SYSTEM_LIBS or Path(match[1]).name.startswith(webview_runtime):
            continue
        # Use the SONAME so copied symlink targets remain discoverable.
        source = Path(match[1])
        soname = re.search(r'\(SONAME\).*?\[(.*?)\]', run('readelf', '-d', source))
        shutil.copy2(source, libs/(soname[1] if soname else source.name))
    launcher = root/'AppRun'
    launcher.write_text('''#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$base/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DATA_DIR="${DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/symbiosic}"
cd "$base"
exec ./bin/symbiosic "$@"
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
    cert_paths = [Path(run('curl-config', '--ca').strip()), Path('/usr/ssl/certs/ca-bundle.crt'), Path('/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem'), Path('/etc/ssl/cert.pem')]
    certificate = next((path for path in cert_paths if path.is_file()), None)
    if certificate is None:
        raise RuntimeError('The configured curl CA bundle is missing')
    shutil.copy2(certificate, root/'assets/cacert.pem')
    pending = [root/'bin'/binary.name,root/'bin/tiktoken_c.dll']
    copied = set()
    while pending:
        library = pending.pop()
        for name in re.findall(r'DLL Name: (\S+)', run('objdump','-p',library)):
            if name.lower() in copied:
                continue
            copied.add(name.lower())
            source = next((p/name for p in [Path('/usr/bin'),Path('/mingw64/bin')] if (p/name).exists()),None)
            if source is None and name.lower().startswith(('vcruntime', 'msvcp')):
                runtime = Path('/c/Windows/System32')/name
                if not runtime.exists():
                    raise RuntimeError('Missing redistributable runtime: '+name)
                source = runtime
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
    icon_path = run('cygpath', '-m', icon).strip()
    resource.write_text('1 ICON "'+icon_path+'"\n')
    # Native binutils need native paths and the MinGW compiler on PATH.
    os.environ['PATH'] = '/mingw64/bin:' + os.environ['PATH']
    native_resource = run('cygpath', '-m', resource).strip()
    # The MSYS server remains C17. Only this tiny webview host uses native MinGW.
    host_build = SRC/'build/windows-host'
    run('/mingw64/bin/cmake', '-S', run('cygpath', '-m', SRC/'packaging/windows').strip(),
        '-B', run('cygpath', '-m', host_build).strip(), '-G', 'MinGW Makefiles',
        '-DCMAKE_BUILD_TYPE=Release', '-DSYMBIOSIC_ICON_RESOURCE='+native_resource)
    run('/mingw64/bin/cmake', '--build', run('cygpath', '-m', host_build).strip(), '--parallel', '4')
    shutil.copy2(host_build/'Symbiosic.exe', root/'Symbiosic.exe')
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
    if args.target.startswith('windows') and binary.suffix != '.exe':
        binary = binary.with_suffix('.exe')
    with tempfile.TemporaryDirectory(prefix='symbiosic-package-') as temporary:
        build = macos if args.target.startswith('macos') else linux if args.target.startswith('linux') else windows
        artifact = build(Path(temporary),args.out.resolve(),args.target,binary)
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    artifact.with_name(artifact.name+'.sha256').write_text(digest+'  '+artifact.name+'\n')
    print(artifact)

if __name__ == '__main__':
    main()
