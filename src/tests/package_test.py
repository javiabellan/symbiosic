#!/usr/bin/env python3
"""Launch the packaged server from its own resources and check its UI assets."""
import http.client
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile

ATTACH_TRIES = 5
ATTACH_DELAY = 2

artifact = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='symbiosic-bundle-') as temporary:
    base = Path(temporary)
    mounted = False
    process = None
    try:
        if artifact.suffix == '.dmg':
            mount = base/'mount'
            mount.mkdir()
            # DiskImages may still hold the freshly created image on hosted macOS runners.
            for attempt in range(ATTACH_TRIES):
                result = subprocess.run(['hdiutil','attach',str(artifact),'-nobrowse','-readonly','-mountpoint',str(mount)],stdout=subprocess.DEVNULL)
                if result.returncode == 0:
                    break
                if attempt == ATTACH_TRIES - 1:
                    result.check_returncode()
                time.sleep(ATTACH_DELAY)
            mounted = True
            root = mount/'Symbiosic.app/Contents/Resources'
        elif artifact.suffix == '.zip':
            with zipfile.ZipFile(artifact) as archive:
                archive.extractall(base)
            root = base/'Symbiosic'
        else:
            with tarfile.open(artifact) as archive:
                archive.extractall(base)
            root = base/'symbiosic'
        binary = next((root/'bin').glob('symbiosic*'))
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0))
            port = listener.getsockname()[1]
        env = dict(os.environ, DATA_DIR=str(base/'data'), CLAUDE_ROOT=str(base/'claude'), CODEX_ROOT=str(base/'codex'), OPENAI_API_KEY='', ANTHROPIC_API_KEY='', OPENROUTER_API_KEY='', GEMINI_API_KEY='', PORT=str(port), LD_LIBRARY_PATH=str(root/'lib'))
        process = subprocess.Popen([str(binary)],cwd=root,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        line = process.stdout.readline().strip()
        assert line == f'http://127.0.0.1:{port}', process.stderr.read() if process.poll() is not None else line
        for resource in ['/', '/app.js', '/styles.css', '/icon.svg', '/api/settings']:
            connection = http.client.HTTPConnection('127.0.0.1',port,timeout=5)
            connection.request('GET',resource)
            response = connection.getresponse()
            assert response.status == http.client.OK, resource
            assert response.read(), resource
            connection.close()
        print('PASS: packaged server and UI assets')
    finally:
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=15)
        if mounted:
            subprocess.run(['hdiutil','detach',str(base/'mount')],check=True,stdout=subprocess.DEVNULL)
