#!/usr/bin/env python3
"""Native webview/server smoke check. Run with a desktop, or xvfb-run on Linux."""
import http.client
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='symbiosic-desktop-') as temporary:
    base = Path(temporary)
    env = dict(os.environ, DATA_DIR=str(base/'data'), CLAUDE_ROOT=str(base/'claude'),
               CODEX_ROOT=str(base/'codex'), PORT='0', OPENAI_API_KEY='',
               ANTHROPIC_API_KEY='', OPENROUTER_API_KEY='', GEMINI_API_KEY='')
    process = subprocess.Popen([str(BINARY)], cwd=ROOT, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        url = process.stdout.readline().strip()
        parsed = urlsplit(url)
        assert parsed.hostname == '127.0.0.1' and parsed.port, url
        # The server loop starts after native window creation. An HTTP response
        # proves both event loops have started, rather than merely a bound socket.
        connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=20)
        connection.request('GET', '/', headers={'Origin': url})
        response = connection.getresponse()
        assert response.status == 200 and b'Symbiosic' in response.read()
        connection.close()
        process.send_signal(signal.SIGTERM)
        _, error = process.communicate(timeout=20)
        assert process.returncode == 0, error
        with socket.socket() as probe:
            assert probe.connect_ex(('127.0.0.1', parsed.port)) != 0
    finally:
        if process.poll() is None:
            process.kill()
            _, error = process.communicate()
            print(error, file=sys.stderr)
print('PASS: native window initialization, background HTTP loop and coordinated exit')
