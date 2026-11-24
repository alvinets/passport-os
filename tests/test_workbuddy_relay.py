#!/usr/bin/env python3
"""Host tests for the WorkBuddy relay protocol and ACP text mapping."""
import importlib.util
import json
import os
import sys
import threading
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
os.environ.setdefault("WORKBUDDY_DEVICE_TOKEN", "0123456789abcdefdeviceToken")
os.environ.setdefault("WORKBUDDY_CLIENT_ID", "client")
os.environ.setdefault("WORKBUDDY_CLIENT_SECRET", "secret")
os.environ.setdefault("WORKBUDDY_REDIRECT_URI", "https://relay.example/oauth/callback")

spec = importlib.util.spec_from_file_location("workbuddy_relay", ROOT / "tools" / "workbuddy_relay.py")
relay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relay)


class FakeResponse:
    def __init__(self, payload, status=200, headers=None):
        self.payload = payload
        self.status = status
        self.headers = headers or {}

    def read(self):
        return json.dumps(self.payload).encode()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


class FakeSSE:
    lines = [
        b'data: {"jsonrpc":"2.0","method":"session/update","params":{"update":{"content":[{"type":"text","text":"Hello"}]}}}\n',
        b'data: {"jsonrpc":"2.0","method":"session/update","params":{"update":{"content":[{"type":"text","text":" world"}]}}}\n',
        b'data: {"jsonrpc":"2.0","id":3,"result":{"stopReason":"end_turn"}}\n',
    ]

    def __init__(self):
        self.posts = []

    def __call__(self, link, token, connection_id):
        return 200, {"Acp-Connection-Id": "conn-1"}, iter(self.lines)


def test_acp_text_mapping():
    sse = FakeSSE()
    posts = []

    def opener(req, timeout):
        posts.append(json.loads(req.data.decode()))
        return FakeResponse({})

    old = relay.urllib.request.urlopen
    relay.urllib.request.urlopen = opener
    try:
        text = relay.acp_text("https://acp.example/sessions/t1", "tok", "t1", "hello", opener, sse)
    finally:
        relay.urllib.request.urlopen = old
    assert text == "Hello world", text
    assert [p["method"] for p in posts] == ["initialize", "session/load", "session/prompt"]
    assert posts[2]["params"]["prompt"] == [{"type": "text", "text": "hello"}]
    assert posts[2]["params"]["sessionId"] == "t1"


def test_run_turn_without_asr_reports_error():
    turn = {"state": "recording", "chunks": [b"RIFF"], "cancelled": False}
    old_asr = relay.transcribe
    relay.transcribe = lambda wav, opener=None: ""
    try:
        relay.run_turn(turn)
    finally:
        relay.transcribe = old_asr
    assert turn["state"] == "error"
    assert "转写" in turn["error"]


def test_device_protocol_http():
    old_run = relay.run_turn
    relay.run_turn = lambda turn, opener=None, sse=None: turn.update(state="done", text="ok")
    token = relay.DEVICE_TOKEN
    server = relay.ThreadingHTTPServer(("127.0.0.1", 0), relay.Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    try:
        def call(method, path, payload=None, auth=True):
            data = json.dumps(payload).encode() if payload is not None else None
            headers = {"Authorization": "Bearer " + token} if auth else {}
            if data:
                headers["Content-Type"] = "application/json"
            req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
            try:
                with urllib.request.urlopen(req, timeout=5) as r:
                    return r.status, json.loads(r.read().decode())
            except urllib.error.HTTPError as e:
                return e.code, json.loads(e.read().decode())

        status, body = call("GET", "/v1/status", auth=False)
        assert status == 401 and body["error"] == "unauthorized"
        status, body = call("POST", "/v1/turn/begin", {})
        assert status == 200 and len(body["turn_id"]) == 32
        tid = body["turn_id"]
        status, body = call("POST", f"/v1/turn/{tid}/audio", {"chunk": "UklGRg=="})
        assert status == 200 and body["ok"]
        status, body = call("POST", f"/v1/turn/{tid}/end", {})
        assert status == 200 and body["state"] == "working"
        status, body = call("GET", f"/v1/turn/{tid}")
        assert status == 200 and body["state"] in ("working", "done")
        status, body = call("POST", f"/v1/turn/{tid}/cancel", {})
        assert status == 200 and body["ok"]
    finally:
        server.shutdown()
        server.server_close()
        relay.run_turn = old_run


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for test in tests:
        test()
        print("PASS", test.__name__)
    print(f"WorkBuddy relay tests: PASS ({len(tests)} cases)")


if __name__ == "__main__":
    main()
