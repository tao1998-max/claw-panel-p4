#!/usr/bin/env python3
"""Upload sd/panel-assets/<app_id>/ to the device SD card.

The device endpoint is restricted to /sdcard/panel-assets and is optional:
older firmware returns 404 and this script prints manual-copy instructions.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

from _common import (fail, file_sha256, http_download, http_json, http_upload,
                     human_bytes, normalize_host, quote_query)

RESERVE = 256 * 1024


def resolve_local(sd_dir: Path, app_id: str | None):
    panel_root = sd_dir / "panel-assets"
    if app_id:
        candidate = panel_root / app_id
        if not candidate.is_dir():
            fail(f"SD app dir not found: {candidate}")
        return candidate.resolve(), app_id
    if panel_root.is_dir():
        children = [p for p in panel_root.iterdir() if p.is_dir()]
        if len(children) == 1:
            return children[0].resolve(), children[0].name
        if len(children) > 1:
            fail("multiple apps under panel-assets; pass --app-id")
    # Allow --sd-dir to point directly at the app directory.
    if (sd_dir / "img").is_dir() or (sd_dir / "sfx").is_dir():
        return sd_dir.resolve(), sd_dir.name
    fail("cannot find panel-assets/<app_id> under --sd-dir")


def manual_copy(local_app: Path, app_id: str) -> str:
    return (
        "The device does not expose /api/sd-assets. Copy manually with the device powered off:\n"
        f"  copy {local_app} to <SD>:/panel-assets/{app_id}/\n"
        "  insert the card before power-on (no hot-plug), then launch the App."
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Device IP or http://host")
    parser.add_argument("--sd-dir", required=True, type=Path,
                        help="Staging root containing panel-assets/<id>")
    parser.add_argument("--app-id", help="App id (auto-detected when unique)")
    parser.add_argument("--clean", action="store_true", help="Delete the remote app dir first")
    parser.add_argument("--no-verify", action="store_true", help="Skip SHA-256 verification")
    parser.add_argument("--dry-run", action="store_true", help="Print the plan only")
    args = parser.parse_args()

    sd_dir = args.sd_dir.resolve()
    if not sd_dir.is_dir():
        fail(f"not a directory: {sd_dir}")
    local_app, app_id = resolve_local(sd_dir, args.app_id)
    host = normalize_host(args.host)

    files = sorted(p for p in local_app.rglob("*") if p.is_file())
    if not files:
        fail(f"no files under {local_app}")
    total = sum(p.stat().st_size for p in files)

    storage_http, storage, _ = http_json(host, "/api/storage")
    if storage_http != 200 or not isinstance(storage, dict):
        print(manual_copy(local_app, app_id), file=sys.stderr)
        return 2
    sd = storage.get("sd", {})
    if not sd.get("mounted"):
        fail("SD card is not mounted on the device", 2)
    free = int(sd.get("free", 0))
    if total + RESERVE > free:
        fail(f"SD needs {human_bytes(total + RESERVE)} including reserve; "
             f"only {human_bytes(free)} free")

    print(f"app={app_id} files={len(files)} bytes={human_bytes(total)} sd_free={human_bytes(free)}")
    for path in files:
        print(f"  upload {path.relative_to(local_app).as_posix()} ({human_bytes(path.stat().st_size)})")
    if args.dry_run:
        return 0

    root_path = f"/{app_id}"
    if args.clean:
        status, _, _ = http_json(host, "/api/sd-assets?path=" + quote_query(root_path) + "&recursive=1",
                                 "DELETE")
        print(f"clean {root_path}: HTTP {status}")

    dirs = {root_path}
    for path in files:
        rel = path.relative_to(local_app).as_posix()
        parent = Path(rel).parent.as_posix()
        if parent and parent != ".":
            dirs.add(f"{root_path}/{parent}")
    for directory in sorted(dirs, key=lambda value: value.count("/")):
        status, _, body = http_json(host, "/api/sd-assets/mkdir", "POST",
                                    {"path": directory, "recursive": True})
        if status != 200:
            if status == 404:
                print(manual_copy(local_app, app_id), file=sys.stderr)
                return 2
            fail(f"mkdir {directory} failed: HTTP {status} {body[:200]!r}")

    for path in files:
        rel = path.relative_to(local_app).as_posix()
        remote = f"{root_path}/{rel}"
        data = path.read_bytes()
        status, body = http_upload(host, "/api/sd-assets/upload?path=" + quote_query(remote), data)
        if status == 404:
            print(manual_copy(local_app, app_id), file=sys.stderr)
            return 2
        if status != 200:
            fail(f"upload {rel} failed: HTTP {status} {body[:200]!r}")
        if not args.no_verify:
            downloaded = http_download(host, "/api/sd-assets/file?path=" + quote_query(remote))
            if hashlib.sha256(downloaded).hexdigest() != file_sha256(data):
                fail(f"verify failed for {rel}", 2)
        print(f"  ok {rel}")

    print(f"uploaded {app_id} to /sdcard/panel-assets/{app_id}")
    print("relaunch the App (or restart the device) to pick up new SD assets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
