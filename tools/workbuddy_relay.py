#!/usr/bin/env python3
"""WorkBuddy relay for Passport OS.

Keeps the OAuth client_secret and access tokens on a server. The ESP32 device
only stores a device token issued here and uses a bounded streaming protocol.

Environment:
  WORKBUDDY_CLIENT_ID, WORKBUDDY_CLIENT_SECRET, WORKBUDDY_REDIRECT_URI
  WORKBUDDY_DEVICE_TOKEN          shared secret presented by the device
  WORKBUDDY_TOKEN_FILE            optional persisted OAuth token JSON (0600)
  WORKBUDDY_ASR_URL               optional OpenAI-compatible transcription URL
  WORKBUDDY_ASR_KEY               optional bearer key for the ASR endpoint
  WORKBUDDY_OPENAPI_BASE          default https://open.workbuddy.cn
  WORKBUDDY_ACP_HOST              default acp.workbuddy.cn
  WORKBUDDY_PORT                  default 8787
  WORKBUDDY_BIND                  default 127.0.0.1 (use TLS/reverse proxy in prod)

Endpoints:
  GET  /oauth/start, GET /oauth/callback
  GET  /v1/status
  POST /v1/turn/begin
  POST /v1/turn/{id}/audio
  POST /v1/turn/{id}/end
  GET  /v1/turn/{id}
  POST /v1/turn/{id}/cancel
"""
from __future__ import annotations

import base64
import json
import os
import secrets
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

OPENAPI = os.environ.get("WORKBUDDY_OPENAPI_BASE", "https://open.workbuddy.cn").rstrip("/")
ACP_HOST = os.environ.get("WORKBUDDY_ACP_HOST", "acp.workbuddy.cn")
SCOPES = "user.task.invokable user.task.readable"
DEVICE_TOKEN = os.environ.get("WORKBUDDY_DEVICE_TOKEN", "")
TOKEN_FILE = os.environ.get("WORKBUDDY_TOKEN_FILE", "")
ASR_URL = os.environ.get("WORKBUDDY_ASR_URL", "")
ASR_KEY = os.environ.get("WORKBUDDY_ASR_KEY", "")
CLIENT_ID = os.environ.get("WORKBUDDY_CLIENT_ID", "")
CLIENT_SECRET = os.environ.get("WORKBUDDY_CLIENT_SECRET", "")
REDIRECT_URI = os.environ.get("WORKBUDDY_REDIRECT_URI", "")

_oauth = {"access_token": "", "refresh_token": "", "expires_at": 0.0}
_oauth_lock = threading.Lock()
_turns: dict[str, dict] = {}
_turns_lock = threading.Lock()
_oauth_state = {"value": secrets.token_urlsafe(24)}


def load_tokens() -> None:
    if not TOKEN_FILE or not os.path.exists(TOKEN_FILE):
        return
    with open(TOKEN_FILE, "r", encoding="utf-8") as fh:
        _oauth.update(json.load(fh))


def save_tokens() -> None:
    if not TOKEN_FILE:
        return
    old = os.umask(0o077)
    try:
        with open(TOKEN_FILE, "w", encoding="utf-8") as fh:
            json.dump(_oauth, fh)
    finally:
        os.umask(old)


def api(method: str, path: str, body: dict | None = None, access: str = "", opener=None):
    url = path if path.startswith("http") else OPENAPI + path
    data = json.dumps(body).encode("utf-8") if body is not None else None
    headers = {"Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    if access:
        headers["Authorization"] = "Bearer " + access
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    fn = opener or urllib.request.urlopen
    with fn(req, timeout=20) as resp:
        return json.loads(resp.read().decode("utf-8"))


def exchange_code(code: str, opener=None) -> dict:
    return api("POST", "/openapi/v2/token", {
        "grant_type": "authorization_code",
        "client_id": CLIENT_ID,
        "client_secret": CLIENT_SECRET,
        "code": code,
        "redirect_uri": REDIRECT_URI,
    }, opener=opener)


def refresh_access(opener=None) -> str:
    with _oauth_lock:
        if not _oauth.get("refresh_token"):
            return ""
        result = api("POST", "/openapi/v2/token", {
            "grant_type": "refresh_token",
            "client_id": CLIENT_ID,
            "client_secret": CLIENT_SECRET,
            "refresh_token": _oauth["refresh_token"],
        }, opener=opener)
        _oauth["access_token"] = result["access_token"]
        if result.get("refresh_token"):
            _oauth["refresh_token"] = result["refresh_token"]
        _oauth["expires_at"] = time.time() + max(60, int(result.get("expires_in", 3600)) - 120)
        save_tokens()
        return _oauth["access_token"]


def current_access(opener=None) -> str:
    with _oauth_lock:
        token = _oauth.get("access_token", "")
        valid = token and time.time() < _oauth.get("expires_at", 0)
    if valid:
        return token
    return refresh_access(opener)


def transcribe(wav: bytes, opener=None) -> str:
    if not ASR_URL:
        return ""
    boundary = "----wb" + secrets.token_hex(8)
    body = (
        f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n"
    ).encode() + wav + f"\r\n--{boundary}--\r\n".encode()
    headers = {"Content-Type": f"multipart/form-data; boundary={boundary}"}
    if ASR_KEY:
        headers["Authorization"] = "Bearer " + ASR_KEY
    req = urllib.request.Request(ASR_URL, data=body, headers=headers, method="POST")
    fn = opener or urllib.request.urlopen
    with fn(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8")).get("text", "")


def acp_text(link: str, acp_token: str, task_id: str, prompt: str, opener=None, sse=None) -> str:
    """Drive ACP: initialize -> session/load -> session/prompt, collect text."""
    connection_id = ""
    if sse is not None:
        status, headers, stream = sse(link, acp_token, connection_id)
        connection_id = headers.get("Acp-Connection-Id", "")
    else:
        req = urllib.request.Request(link, headers={
            "Authorization": "Bearer " + acp_token,
            "Accept": "text/event-stream",
        })
        fn = opener or urllib.request.urlopen
        with fn(req, timeout=30) as resp:
            status = resp.status
            connection_id = resp.headers.get("Acp-Connection-Id", "")
            stream = resp

    def rpc(rid: int, method: str, params: dict) -> None:
        payload = {"jsonrpc": "2.0", "id": rid, "method": method, "params": params}
        post = urllib.request.Request(link, data=json.dumps(payload).encode(), headers={
            "Authorization": "Bearer " + acp_token,
            "Content-Type": "application/json",
            "Acp-Connection-Id": connection_id,
        }, method="POST")
        (opener or urllib.request.urlopen)(post, timeout=20)

    rpc(1, "initialize", {"protocolVersion": 1, "clientCapabilities": {"fs": {"readTextFile": False, "writeTextFile": False}}})
    rpc(2, "session/load", {"sessionId": task_id, "cwd": "/workspace", "mcpServers": []})
    rpc(3, "session/prompt", {"sessionId": task_id, "prompt": [{"type": "text", "text": prompt}]})

    pieces: list[str] = []
    done = False
    for raw in stream:
        line = raw.decode("utf-8", "replace").strip()
        if not line.startswith("data:"):
            continue
        try:
            msg = json.loads(line[5:].strip())
        except ValueError:
            continue
        method = msg.get("method")
        if method == "session/update":
            update = (msg.get("params") or {}).get("update") or {}
            for block in update.get("content", []) or []:
                if block.get("type") == "text" and block.get("text"):
                    pieces.append(block["text"])
        elif msg.get("id") == 3 and "result" in msg:
            done = True
            break
    try:
        stream.close()
    except Exception:
        pass
    return "".join(pieces) if (done or pieces) else ""


def run_turn(turn: dict, opener=None, sse=None) -> None:
    try:
        wav = b"".join(turn["chunks"])
        prompt = transcribe(wav, opener)
        if not prompt:
            turn.update(state="error", error="未配置语音转写服务，无法把语音交给 WorkBuddy")
            return
        access = current_access(opener)
        if not access:
            turn.update(state="error", error="WorkBuddy 尚未完成 OAuth 授权")
            return
        task = api("POST", "/openapi/v2/tasks", {"prompt": prompt, "name": "Passport OS WorkBuddy"}, access, opener)
        task_id = task.get("task_id", "")
        link = task.get("link", "")
        token = task.get("token", "")
        for _ in range(20):
            if link and token:
                break
            time.sleep(0.5)
            detail = api("GET", f"/openapi/v2/tasks/{task_id}", access=access, opener=opener)
            link = detail.get("link", link)
            token = detail.get("token", token)
        if not link or not token:
            turn.update(state="error", error="WorkBuddy 任务未返回 ACP 连接信息")
            return
        text = acp_text(link, token, task_id, prompt, opener, sse)
        if turn.get("cancelled"):
            return
        if text:
            turn.update(state="done", text=text[:1023], prompt=prompt)
        else:
            turn.update(state="error", error="WorkBuddy 未返回文本回复，请在电脑端查看")
    except Exception as exc:  # noqa: BLE001 - relay must always answer the device
        turn.update(state="error", error=f"中继错误: {type(exc).__name__}")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def send_json(self, status: int, payload: dict) -> None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def authorized(self) -> bool:
        if not DEVICE_TOKEN:
            return False
        return self.headers.get("Authorization") == "Bearer " + DEVICE_TOKEN

    def body(self) -> dict:
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length <= 0 or length > 2_000_000:
            return {}
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def do_GET(self) -> None:  # noqa: N802
        path = urllib.parse.urlparse(self.path).path
        query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if path == "/oauth/start":
            url = (OPENAPI + "/openapi/v2/authorize?" + urllib.parse.urlencode({
                "response_type": "code", "client_id": CLIENT_ID, "redirect_uri": REDIRECT_URI,
                "scope": SCOPES, "state": _oauth_state["value"], "prompt": "consent",
            }))
            self.send_response(302)
            self.send_header("Location", url)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/oauth/callback":
            if query.get("state", [""])[0] != _oauth_state["value"]:
                self.send_json(400, {"error": "invalid_state"})
                return
            code = query.get("code", [""])[0]
            if not code:
                self.send_json(400, {"error": "missing_code"})
                return
            result = exchange_code(code)
            with _oauth_lock:
                _oauth["access_token"] = result["access_token"]
                _oauth["refresh_token"] = result.get("refresh_token", "")
                _oauth["expires_at"] = time.time() + max(60, int(result.get("expires_in", 3600)) - 120)
                save_tokens()
            self.send_json(200, {"ok": True})
            return
        if not self.authorized():
            self.send_json(401, {"error": "unauthorized"})
            return
        if path == "/v1/status":
            self.send_json(200, {"ok": bool(current_access()), "agent": "WorkBuddy"})
            return
        if path.startswith("/v1/turn/"):
            turn_id = path.rsplit("/", 1)[-1]
            with _turns_lock:
                turn = _turns.get(turn_id)
                if not turn:
                    self.send_json(404, {"error": "not_found"})
                    return
                payload = {k: turn[k] for k in ("state", "text", "error") if k in turn}
            self.send_json(200, payload)
            return
        self.send_json(404, {"error": "not_found"})

    def do_POST(self) -> None:  # noqa: N802
        if not self.authorized():
            self.send_json(401, {"error": "unauthorized"})
            return
        path = urllib.parse.urlparse(self.path).path
        try:
            body = self.body()
        except ValueError:
            self.send_json(400, {"error": "invalid_json"})
            return
        if path == "/v1/turn/begin":
            turn_id = uuid.uuid4().hex
            with _turns_lock:
                _turns[turn_id] = {"state": "recording", "chunks": [], "cancelled": False,
                                   "created": time.time()}
            self.send_json(200, {"turn_id": turn_id})
            return
        parts = path.split("/")
        if len(parts) >= 5 and parts[2] == "turn":
            turn_id = parts[3]
            action = parts[4]
            with _turns_lock:
                turn = _turns.get(turn_id)
            if not turn:
                self.send_json(404, {"error": "not_found"})
                return
            if action == "audio":
                chunk = body.get("chunk", "")
                try:
                    turn["chunks"].append(base64.b64decode(chunk, validate=True))
                except (ValueError, TypeError):
                    self.send_json(400, {"error": "invalid_chunk"})
                    return
                self.send_json(200, {"ok": True})
                return
            if action == "cancel":
                turn["cancelled"] = True
                self.send_json(200, {"ok": True})
                return
            if action == "end":
                turn["state"] = "working"
                threading.Thread(target=run_turn, args=(turn,), daemon=True).start()
                self.send_json(200, {"state": "working"})
                return
        self.send_json(404, {"error": "not_found"})


def main() -> None:
    load_tokens()
    if len(DEVICE_TOKEN) < 16:
        raise SystemExit("WORKBUDDY_DEVICE_TOKEN must be at least 16 characters")
    port = int(os.environ.get("WORKBUDDY_PORT", "8787"))
    bind = os.environ.get("WORKBUDDY_BIND", "127.0.0.1")
    server = ThreadingHTTPServer((bind, port), Handler)
    print(f"WorkBuddy relay listening on http://{bind}:{port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
