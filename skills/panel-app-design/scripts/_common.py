"""Shared stdlib helpers for panel-app-design scripts.

No third-party imports here: every script must be able to print --help even
when Pillow/fontTools are not installed.
"""

from __future__ import annotations

import json
import os
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

FAT_CLUSTER = 4096
DEFAULT_TIMEOUT = 15


def fail(message: str, code: int = 1) -> "NoReturn":
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(code)


def warn(message: str) -> None:
    print(f"warning: {message}", file=sys.stderr)


def human_bytes(value: int) -> str:
    value = int(value)
    if value >= 1024 * 1024:
        return f"{value / (1024 * 1024):.2f} MiB"
    if value >= 1024:
        return f"{value / 1024:.1f} KiB"
    return f"{value} B"


def normalize_host(host: str) -> str:
    host = host.strip().rstrip("/")
    if not host:
        fail("host is empty")
    if not host.startswith(("http://", "https://")):
        host = "http://" + host
    return host


def _request(host: str, path: str, method: str = "GET", data: bytes | None = None,
             headers: dict[str, str] | None = None, timeout: int = DEFAULT_TIMEOUT):
    url = normalize_host(host) + path
    req = urllib.request.Request(url, data=data, method=method)
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        fail(f"cannot reach {url}: {exc}", 2)


def http_json(host: str, path: str, method: str = "GET", payload=None,
              timeout: int = DEFAULT_TIMEOUT):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    status, body = _request(host, path, method, data, headers, timeout)
    try:
        parsed = json.loads(body.decode("utf-8")) if body else None
    except (UnicodeDecodeError, json.JSONDecodeError):
        parsed = None
    return status, parsed, body


def http_upload(host: str, path: str, data: bytes, timeout: int = 60):
    return _request(host, path, "POST", data,
                    {"Content-Type": "application/octet-stream"}, timeout)


def http_download(host: str, path: str, timeout: int = 60) -> bytes:
    status, body = _request(host, path, timeout=timeout)
    if status != 200:
        fail(f"download {path} failed: HTTP {status}", 2)
    return body


def quote_query(value: str) -> str:
    return urllib.parse.quote(value, safe="/")


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail(f"file not found: {path}")
    except json.JSONDecodeError as exc:
        fail(f"invalid JSON in {path}: {exc}")


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8")


def package_bytes(root: Path) -> int:
    total = 0
    for path in root.rglob("*"):
        if path.is_file():
            total += path.stat().st_size
    return total


def package_bytes_rounded(root: Path, cluster: int = FAT_CLUSTER) -> int:
    total = 0
    for path in root.rglob("*"):
        if path.is_file():
            size = path.stat().st_size
            total += max(cluster, ((size + cluster - 1) // cluster) * cluster)
    return total


def jpeg_dimensions(path: Path) -> tuple[int, int] | None:
    """Read JPEG SOF dimensions without Pillow."""
    data = path.read_bytes()
    if len(data) < 4 or data[0:2] != b"\xff\xd8":
        return None
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        if i + 2 > len(data):
            return None
        length = struct.unpack(">H", data[i:i + 2])[0]
        if length < 2 or i + length > len(data):
            return None
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                      0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            if i + 7 > len(data):
                return None
            height = struct.unpack(">H", data[i + 3:i + 5])[0]
            width = struct.unpack(">H", data[i + 5:i + 7])[0]
            return width, height
        i += length
    return None


def is_relative_inside(value: str) -> bool:
    if not isinstance(value, str) or not value:
        return False
    if value.startswith(("/", "\\")) or ".." in value or "\\" in value:
        return False
    return ":" not in value


def find_files(root: Path) -> list[Path]:
    return sorted(p for p in root.rglob("*") if p.is_file())


def ensure_inside(root: Path, candidate: Path) -> Path:
    root = root.resolve()
    candidate = candidate.resolve()
    if root != candidate and root not in candidate.parents:
        fail(f"path escapes root: {candidate}")
    return candidate


def safe_join(root: Path, relative: str) -> Path:
    if not is_relative_inside(relative):
        fail(f"unsafe relative path: {relative!r}")
    return ensure_inside(root, root / relative)


def port_from_host(host: str) -> str:
    parsed = urllib.parse.urlparse(normalize_host(host))
    return parsed.netloc


def file_sha256(data: bytes) -> str:
    import hashlib
    return hashlib.sha256(data).hexdigest()


def try_json(host: str, path: str, method: str = "GET", payload=None,
             timeout: int = DEFAULT_TIMEOUT):
    """Like http_json, but returns (None, None, b"") without printing on error."""
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    url = normalize_host(host) + path
    req = urllib.request.Request(url, data=data, method=method)
    for key, value in headers.items():
        req.add_header(key, value)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            body = response.read()
            status = response.status
    except Exception:
        return None, None, b""
    try:
        parsed = json.loads(body.decode("utf-8")) if body else None
    except (UnicodeDecodeError, json.JSONDecodeError):
        parsed = None
    return status, parsed, body
