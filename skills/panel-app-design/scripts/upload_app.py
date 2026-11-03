#!/usr/bin/env python3
"""Upload a validated App package to FAT or SD over the Web API.

After uploading, `POST /api/apps/refresh` is called so the launcher picks the
package up without a reboot (older firmware: use --restart).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from _common import (fail, http_json, http_upload, human_bytes,
                     normalize_host, package_bytes_rounded, quote_query, try_json)

RESERVE = 64 * 1024
SD_RESERVE = 256 * 1024  # firmware keeps this much headroom on SD uploads

# volume -> (API prefix, remote base dir, storage key)
VOLUMES = {
    "fat": ("/api/files", "/data/apps", "fat"),
    "sd": ("/api/sd-files", "/apps", "sd"),
}


def read_app_id(app_dir: Path) -> str:
    try:
        manifest = json.loads((app_dir / "launcher.json").read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail("launcher.json not found")
    except json.JSONDecodeError as exc:
        fail(f"launcher.json is invalid: {exc}")
    app_id = manifest.get("id")
    if not isinstance(app_id, str) or not app_id:
        fail("launcher.json id is missing")
    return app_id


def validate_package(app_dir: Path, app_id: str, fat_free_kb: int | None, force: bool):
    script = Path(__file__).with_name("validate_app.py")
    cmd = [sys.executable, str(script), "--app-dir", str(app_dir), "--remote-id", app_id]
    if fat_free_kb is not None:
        cmd += ["--fat-free-kb", str(fat_free_kb)]
    if force:
        cmd += ["--allow-large"]
    result = subprocess.run(cmd, text=True)
    if result.returncode != 0:
        fail("package validation failed", 1)


def upload_files(host: str, app_dir: Path, app_id: str, volume: str,
                 dry_run: bool):
    api, base_dir, _ = VOLUMES[volume]
    root = f"{base_dir}/{app_id}"

    files = []
    for path in sorted(app_dir.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(app_dir).as_posix()
        if rel == "launcher.json":
            continue
        # Tooling-only manifest; never uploaded.
        if rel == "sd_manifest.json":
            continue
        if rel.endswith((".md", ".py", ".zip")):
            continue
        files.append((rel, path))
    files.append(("launcher.json", app_dir / "launcher.json"))

    dirs = {root}
    for rel, _ in files:
        parent = Path(rel).parent.as_posix()
        if parent and parent != ".":
            dirs.add(f"{root}/{parent}")

    print(f"app={app_id} volume={volume} files={len(files)} dirs={len(dirs)}")
    for directory in sorted(dirs, key=lambda value: value.count("/")):
        print(f"  mkdir {directory}")

    if dry_run:
        for rel, path in files:
            print(f"  upload {rel} ({human_bytes(path.stat().st_size)})")
        return

    for directory in sorted(dirs, key=lambda value: value.count("/")):
        status, _, body = http_json(host, api + "/mkdir", "POST",
                                    {"path": directory, "recursive": True})
        if status != 200:
            fail(f"mkdir {directory} failed: HTTP {status} {body[:200]!r}")

    for rel, path in files:
        remote = f"{root}/{rel}"
        print(f"  upload {rel} ({human_bytes(path.stat().st_size)})")
        status, body = http_upload(host, api + "/upload?path=" + quote_query(remote),
                                   path.read_bytes())
        if status != 200:
            fail(f"upload {rel} failed: HTTP {status} {body[:200]!r}")


def wait_for_reboot(host: str, previous_boot: str, timeout: int = 45) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(1)
        status, parsed, _ = try_json(host, "/api/status", timeout=3)
        if status == 200 and isinstance(parsed, dict):
            boot_id = parsed.get("boot_id")
            if boot_id and boot_id != previous_boot:
                print(f"device rebooted: boot_id={boot_id}")
                return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Device IP or http://host")
    parser.add_argument("--app-dir", required=True, type=Path, help="Local App package")
    parser.add_argument("--volume", choices=sorted(VOLUMES), default="fat",
                        help="Deploy to FAT (/fatfs/claw/data/apps) or SD (/sdcard/apps)")
    parser.add_argument("--remote-id", help="Deployed id (default: launcher.json id)")
    parser.add_argument("--restart", action="store_true",
                        help="Reboot the device instead of refreshing the app registry")
    parser.add_argument("--no-refresh", action="store_true",
                        help="Do not call POST /api/apps/refresh after uploading")
    parser.add_argument("--force", action="store_true",
                        help="Overwrite an existing app and allow a large package")
    parser.add_argument("--dry-run", action="store_true", help="Print the plan only")
    args = parser.parse_args()

    app_dir = args.app_dir.resolve()
    if not app_dir.is_dir():
        fail(f"not a directory: {app_dir}")
    app_id = args.remote_id or read_app_id(app_dir)
    if app_id != app_dir.name:
        print(f"note: deploying {app_dir.name!r} as {app_id!r}", file=sys.stderr)

    host = normalize_host(args.host)
    _, status, _ = http_json(host, "/api/status")
    if not isinstance(status, dict):
        fail("cannot read /api/status")

    storage_http, storage, _ = http_json(host, "/api/storage")
    api, base_dir, storage_key = VOLUMES[args.volume]
    free_bytes = None
    if storage_http == 200 and isinstance(storage, dict):
        vol = storage.get(storage_key, {})
        if vol.get("mounted"):
            free_bytes = int(vol.get("free", 0))
            print(f"{args.volume.upper()} free: {human_bytes(free_bytes)}")
        else:
            fail(f"{args.volume.upper()} volume is not mounted", 2)
    else:
        print("warning: /api/storage unavailable; skipping free-space preflight", file=sys.stderr)

    reserve = SD_RESERVE if args.volume == "sd" else RESERVE
    fat_free_kb = None
    if args.volume == "fat" and free_bytes is not None:
        fat_free_kb = free_bytes // 1024
    if not args.force and free_bytes is not None:
        need = package_bytes_rounded(app_dir) + reserve
        if need > free_bytes:
            fail(f"package needs {human_bytes(need)} including reserve; "
                 f"only {human_bytes(free_bytes)} {args.volume.upper()} free (use --force)")

    # Show the version transition (the home grid displays it).
    local_version = ""
    try:
        local_manifest = json.loads((app_dir / "launcher.json").read_text(encoding="utf-8"))
        local_version = str(local_manifest.get("version") or "")
    except (OSError, ValueError):
        pass
    apps_http, apps_body, _ = http_json(host, "/api/apps", timeout=10)
    if apps_http == 200 and isinstance(apps_body, dict):
        for item in apps_body.get("items", []):
            if item.get("id") == app_id:
                remote_version = item.get("version") or "-"
                print(f"version: {remote_version} -> {local_version or '-'}")
                break

    target = f"{base_dir}/{app_id}"
    existing_http, _, _ = http_json(host, api + "?path=" + quote_query(target))
    if existing_http == 200 and not args.force and not args.dry_run:
        fail(f"app {app_id!r} already exists on {args.volume.upper()}; use --force to overwrite")
    if existing_http == 200:
        print(f"warning: app {app_id!r} exists on {args.volume.upper()}"
              + (" (dry-run)" if args.dry_run else "; overwriting"), file=sys.stderr)

    # The 300 KiB target is a FAT budget; SD uploads are only bounded by the
    # firmware limit (64 MiB), so large packages are allowed there.
    validate_package(app_dir, app_id, fat_free_kb,
                     args.force or args.volume == "sd")
    upload_files(host, app_dir, app_id, args.volume, args.dry_run)

    if args.dry_run:
        print("dry-run: nothing was uploaded")
        return 0

    print(f"uploaded {app_id}")
    if args.restart:
        _, before, _ = http_json(host, "/api/status")
        boot_id = before.get("boot_id", "") if isinstance(before, dict) else ""
        status_http, _, body = http_json(host, "/api/restart", "POST", {})
        if status_http != 200:
            fail(f"restart failed: HTTP {status_http} {body[:200]!r}")
        if not wait_for_reboot(host, boot_id):
            fail("device did not report a new boot_id within 45s", 2)
    elif not args.no_refresh:
        # The launcher scans the app roots at boot; refresh rescans them in
        # place so no reboot is needed. Older firmware returns 404 here.
        status, body, raw = http_json(host, "/api/apps/refresh", "POST", {},
                                      timeout=15)
        if status == 200:
            print("app registry refreshed (no reboot needed)")
        else:
            print("hint: this firmware has no /api/apps/refresh; "
                  "POST /api/restart (or pass --restart) to rescan", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
