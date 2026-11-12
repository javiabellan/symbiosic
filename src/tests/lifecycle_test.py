#!/usr/bin/env python3
"""Exercise the desktop host's C17 lifecycle without requiring a display."""
import http.client
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(sys.argv[1]).resolve()

with tempfile.TemporaryDirectory(prefix='symbiosic-lifecycle-') as temporary:
    base = Path(temporary)
    env = dict(os.environ, DATA_DIR=str(base/'data'), CLAUDE_ROOT=str(base/'claude'),
               CODEX_ROOT=str(base/'codex'), PORT='0', OPENAI_API_KEY='',
               ANTHROPIC_API_KEY='', OPENROUTER_API_KEY='', GEMINI_API_KEY='')

    def start(*args, **overrides):
        return subprocess.Popen([str(BINARY), '--headless', *args], cwd=ROOT,
                                env=dict(env, **overrides), stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def ready(process):
        line = process.stdout.readline().strip()
        match = re.fullmatch(r'http://127\.0\.0\.1:([1-9][0-9]*)', line)
        assert match, f'Startup failed: {line}; {process.communicate(timeout=10)}'
        port = int(match[1])
        assert port <= 65535
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
        connection.request('GET', '/api/state')
        response = connection.getresponse()
        assert response.status == 200, response.read()
        response.read()
        connection.close()
        for origin, expected in [(line, 200), ('http://127.0.0.1:0', 403)]:
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=5)
            connection.request('GET', '/api/state', headers={'Origin': origin})
            response = connection.getresponse()
            assert response.status == expected, (origin, response.status)
            response.read()
            connection.close()
        return port

    def stopped(process, port):
        _, error = process.communicate(timeout=15)
        assert process.returncode == 0, error
        with socket.socket() as probe:
            assert probe.connect_ex(('127.0.0.1', port)) != 0, 'Listener survived shutdown'

    # EOF models both closing the window and an unexpectedly terminated host.
    # Reusing the same data directory also proves shutdown releases its lock.
    for shutdown in ['eof', 'command', 'signal']:
        process = start('--parent-stdin')
        try:
            port = ready(process)
            if shutdown == 'eof':
                process.stdin.close()
                process.stdin = None
            elif shutdown == 'command':
                process.stdin.write('quit\n')
                process.stdin.flush()
            else:
                process.send_signal(signal.SIGTERM)
            stopped(process, port)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    # Bind failures report an error and leave neither a live loop nor a data lock.
    with socket.socket() as occupied:
        occupied.bind(('127.0.0.1', 0))
        occupied.listen()
        process = start(PORT=str(occupied.getsockname()[1]))
        _, error = process.communicate(timeout=15)
        assert process.returncode != 0 and 'Cannot listen' in error, error

    process = start()
    try:
        port = ready(process)
        process.send_signal(signal.SIGTERM)
        stopped(process, port)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()

    process = start('--unknown')
    _, error = process.communicate(timeout=15)
    assert process.returncode != 0 and 'unknown argument' in error

print('PASS: ephemeral ports, host EOF, shutdown command, signals, bind failure and restart')
