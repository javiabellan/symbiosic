#!/usr/bin/env python3
"""Black-box local HTTP, discovery and restart checks. Never supplies an API key."""
import http.client
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


with tempfile.TemporaryDirectory(prefix="symbiosic-http-") as temporary:
    base = Path(temporary)
    claude, codex, data = (base / p for p in ("claude", "codex", "data"))
    claude.mkdir()
    codex.mkdir()
    shutil.copy(ROOT / "tests/fixtures/claude.jsonl", claude / "main.jsonl")
    shutil.copy(ROOT / "tests/fixtures/subagent.jsonl", claude / "agent.jsonl")
    shutil.copy(ROOT / "tests/fixtures/codex.jsonl", codex / "main.jsonl")
    # Shared native IDs must remain scoped to their provider's sidecar index.
    codex_source = codex / "main.jsonl"
    codex_source.write_text(codex_source.read_text().replace("codex-session", "claude-session"))
    codex_index = base / "session_index.jsonl"
    codex_index.write_text(json.dumps({"id": "claude-session", "thread_name": "Codex native name"}))
    claude_index = claude / "sessions-index.json"
    claude_index.write_text(json.dumps({"entries": [{"sessionId": "claude-session", "customTitle": "Claude native name", "summary": "Old summary"}]}))
    (codex / "journal.jsonl").write_text('{"workflow":"unrelated"}\n')
    (codex / "linked.jsonl").symlink_to(codex / "main.jsonl")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, OPENAI_API_KEY="", DATA_DIR=str(data),
               CLAUDE_ROOT=str(claude), CODEX_ROOT=str(codex), PORT=str(port), POLL_MS="100")
    process = None

    def start():
        p = subprocess.Popen([str(ROOT / "symbiosic")], cwd=ROOT, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        line = p.stdout.readline().strip()
        if line != f"http://127.0.0.1:{port}":
            raise AssertionError(f"Server failed to start ({p.poll()}): {line} {p.stderr.read()}")
        return p

    def stop(p):
        p.send_signal(signal.SIGTERM)
        _, stderr = p.communicate(timeout=10)
        check(p.returncode == 0, f"Clean shutdown: {stderr}")

    def request(method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        data = json.dumps(body) if body is not None else None
        h = {"Content-Type": "application/json"} if data is not None else {}
        h.update(headers or {})
        conn.request(method, path, data, h)
        response = conn.getresponse()
        raw = response.read()
        check(int(response.getheader("Content-Length")) == len(raw), "Content-Length")
        check(response.getheader("Connection") == "close", "Connection closes")
        code = response.status
        conn.close()
        return code, json.loads(raw) if raw.startswith(b"{") else raw

    def wait_for(predicate):
        for _ in range(100):
            status, result = request("GET", "/api/state")
            if status == 200 and predicate(result):
                return result
            time.sleep(.05)
        raise AssertionError("Discovery did not settle")

    def raw_request(chunks):
        with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
            for chunk in chunks:
                sock.sendall(chunk)
                time.sleep(.003)
            result = b""
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                result += chunk
        return result

    try:
        process = start()
        state = wait_for(lambda s: len(s["traces"]) == 3 and not s["ingestion_running"])
        codex_row = next(t for t in state["traces"] if t["provider"] == "codex")
        claude_row = next(t for t in state["traces"] if t["provider"] == "claude" and not t["subagent"])
        check(codex_row["title"] == "Codex native name", "Final index row without newline imported")
        check(claude_row["title"] == "Claude native name", "Provider indexes stay separate; custom title wins")
        check(next(t for t in state["traces"] if t["subagent"])["title"] != "Claude native name", "Subagent does not inherit parent title")
        codex_index.write_text('{malformed}\n' + json.dumps({"id": "claude-session", "thread_name": "Renamed session"}))
        renamed = wait_for(lambda s: any(t["title"] == "Renamed session" for t in s["traces"]))
        check(next(t for t in renamed["traces"] if t["trace_id"] == codex_row["trace_id"])["revision"] == codex_row["revision"], "Title-only edit preserves evidence revision")
        codex_index.unlink()
        reverted = wait_for(lambda s: all(t["title"] != "Renamed session" for t in s["traces"]))
        check(next(t for t in reverted["traces"] if t["trace_id"] == codex_row["trace_id"])["title"] != "Codex native name", "Deleted title index restores source title")
        for asset in ("/app.js", "/styles.css", "/icon.svg"):
            check(request("GET", asset)[0] == 200, "UI asset serves: " + asset)
        check(request("GET", "/web/app.js")[0] == 404, "Filesystem paths remain private")
        check(not state["key_available"], "Missing key is supported")
        check(len(state["groups"]) == 1, "Providers share normalized cwd group")
        sub = next(t for t in state["traces"] if t["subagent"])
        check(sub["parent_trace_id"] is not None, "Explicit parent relationship resolved")
        check(request("GET", "/")[0] == 200, "UI serves")
        _, settings = request("GET", "/api/settings")
        check(settings["decision"]["model"] == settings["chat"]["model"] == "gpt-6-luna", "Default models")
        check(len(settings["providers"]) == 4, "Four selectable AI providers")
        check(request("GET", "/api/models?provider=invalid")[0] == 400, "Reject unknown provider")
        check(request("POST", "/api/provider", {"provider": "openrouter", "key": "test-session-only"})[0] == 200, "Set session key")
        _, public = request("GET", "/api/settings")
        check("test-session-only" not in json.dumps(public), "API keys are never returned")
        check(request("POST", "/api/provider", {"provider": "openrouter", "key": "bad\r\nheader"})[0] == 400, "Reject header injection")
        settings["chat"]["input_limit"] = 500000
        check(request("POST", "/api/settings", settings)[0] == 200, "Save model limits")
        saved_settings = (data / "ai-settings.json").read_text()
        check("test-session-only" not in saved_settings, "Session keys are never persisted")
        # NTFS ACLs are checked by the C suite; MSYS noacl stat bits are synthetic.
        if not sys.platform.startswith(("msys", "cygwin")):
            check((data / "ai-settings.json").stat().st_mode & 0o777 == 0o600, "Private model settings")
        check(request("GET", "/.env")[0] == 404, "No arbitrary files")
        check(request("GET", "/api/state", headers={"Host": "evil.example"})[0] == 403, "Host checked")
        check(request("POST", "/api/chat", {"scope": {"type": "all"}}, {"Origin": "https://evil.example"})[0] == 403, "Cross-origin mutation blocked")
        check(request("POST", "/api/chat", {}, {"Content-Type": "text/plain"})[0] == 415, "JSON content type required")
        check(request("GET", "/api/trace?id=..%2F.env")[0] == 400, "Opaque IDs checked")
        check(request("POST", "/api/analyze", {"quote_id": "0" * 32})[0] == 404, "Missing quote")
        check(request("POST", "/api/chat/message", {"chat_id": "0" * 32, "text": "x" * 17000})[0] == 413, "Oversize body blocked")
        head = f"GET /api/state HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n\r\n".encode()
        fragmented = raw_request([head[i:i+3] for i in range(0, len(head), 3)])
        check(fragmented.startswith(b"HTTP/1.1 200"), "Fragmented headers parsed")
        body = b'{"scope":{"type":"all"}}'
        head = f"POST /api/chat HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n".encode()
        fragmented = raw_request([head] + [body[i:i+2] for i in range(0, len(body), 2)])
        check(fragmented.startswith(b"HTTP/1.1 200"), "Fragmented body parsed")
        chat_id = json.loads(fragmented.split(b"\r\n\r\n", 1)[1])["chat_id"]
        check(request("POST", "/api/chat/message", {"chat_id": chat_id, "text": "Question"})[0] == 409, "Paid chat disabled without key")
        check(request("POST", "/api/estimate", {"all": True})[0] == 400, "Batch analysis removed")
        check(request("POST", "/api/estimate", {"trace_ids": [state["traces"][0]["trace_id"], state["traces"][1]["trace_id"]]})[0] == 400, "Multi-trace requests rejected")
        _, domains = request("GET", "/api/domains")
        check(len(domains) == 6, "Six editable domains")
        check(request("POST", "/api/domains", {"id":"../escape","name":"x","detection_prompt":"x","analysis_prompt":"x"})[0] == 400, "Domain path traversal rejected")
        _, quote = request("POST", "/api/estimate", {"trace_ids": [state["traces"][0]["trace_id"]], "domain":"frontend"})
        check(quote["total"] == 1 and quote["eligible"] == 1, "Single trace estimate")
        check(quote["entries"][0]["coverage"]["level"] == 5, "Full level 5 analysis excludes system prompts")
        check("source" not in quote["entries"][0] and "request_body" not in quote["entries"][0], "Private evidence omitted from estimate preview")
        check(request("POST", "/api/analyze", {"quote_id": quote["id"]})[0] == 409, "Paid analysis disabled without key")
        native = next(t for t in state["traces"] if t["provider"] == "codex")
        old_revision = native["revision"]
        snapshot = data / "traces" / (native["trace_id"] + ".jsonl")
        other = data / "traces" / (sub["trace_id"] + ".jsonl")
        unchanged_mtime = other.stat().st_mtime_ns
        with (codex / "main.jsonl").open("a") as f:
            f.write('{"type":"response_item","payload":{"type":"message","role":"user","content":"NEW MARKER"}}')
        time.sleep(.25)
        _, current = request("GET", f'/api/trace?id={native["trace_id"]}')
        check(current["meta"]["revision"] == old_revision, "Partial line does not change revision")
        with (codex / "main.jsonl").open("a") as f:
            f.write("\n")
        state = wait_for(lambda s: next(t for t in s["traces"] if t["trace_id"] == native["trace_id"])["revision"] == old_revision + 1)
        check(other.stat().st_mtime_ns == unchanged_mtime, "Unrelated snapshots are not rewritten")
        check("NEW MARKER" in snapshot.read_text(), "Completed final line appears")
        (codex / "main.jsonl").write_text('{"type":"session_meta","payload":{"id":"replacement","cwd":"/work/replaced"}}\n')
        wait_for(lambda s: next(t for t in s["traces"] if t["trace_id"] == native["trace_id"])["cwd"] == "/work/replaced")
        check("NEW MARKER" not in snapshot.read_text(), "Truncation rebuilds snapshot")
        (codex / "main.jsonl").unlink()
        wait_for(lambda s: next(t for t in s["traces"] if t["trace_id"] == native["trace_id"])["source_missing"])
        second = subprocess.run([str(ROOT / "symbiosic")], cwd=ROOT, env=env, capture_output=True, text=True, timeout=5)
        check(second.returncode != 0 and "instance.lock" in second.stderr, "Second writer rejected")
        stop(process)
        process = None
        job_id = "a" * 32
        (data / "jobs" / (job_id + ".json")).write_text(json.dumps({"id": job_id, "state": "running"}))
        process = start()
        _, restored = request("GET", "/api/settings")
        check(restored["chat"]["input_limit"] == 500000, "Model limits survive restart")
        check(not next(p for p in restored["providers"] if p["id"] == "openrouter")["key_available"], "Session key cleared on restart")
        _, recovered = request("GET", f"/api/job?id={job_id}")
        check(recovered["state"] == "interrupted", "Unfinished jobs interrupted on restart")
        check(request("GET", f"/api/chat?id={chat_id}")[0] == 200, "Chat survives restart")
        check(request("GET", f'/api/trace?id={native["trace_id"]}')[0] == 200, "Deleted source stays browsable after restart")
        print(f"PASS: {checks} HTTP/discovery checks; no API key and no paid requests")
    finally:
        if process is not None and process.poll() is None:
            stop(process)
