#!/usr/bin/env python3
"""Move every FAT launcher App to /sdcard/apps, then remove the FAT copies.

The firmware scans /fatfs/claw/data/apps first and /sdcard/apps second; a FAT
package with the same id shadows the SD one.  This tool copies the *deployed*
FAT packages (not the repository sources) over the Web API, so on-device changes
are preserved, then removes the SD probe app and the FAT app directories.

Safety: nothing is deleted until every app has been copied and size-verified.
Use --dry-run first.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKILL_SCRIPTS = ROOT / "skills" / "panel-app-design" / "scripts"
sys.path.insert(0, str(SKILL_SCRIPTS))

from _common import (fail, http_download, http_json, http_upload,  # noqa: E402
                     human_bytes, normalize_host, quote_query, try_json)

FAT_APPS = "/data/apps"
SD_APPS = "/apps"
SD_PROBE = "/apps/sd_probe"
SKIP_NAMES = {"sd_manifest.json"}
SKIP_SUFFIXES = (".md", ".py", ".zip")


def list_dir(host: str, api: str, path: str):
    status, data, body = http_json(host, f"{api}?path=" + quote_query(path))
    if status != 200 or not isinstance(data, dict):
        fail(f"list {path} failed: HTTP {status} {body[:200]!r}")
    return data.get("entries", [])


def collect_fat_files(host: str, app_id: str):
    root = f"{FAT_APPS}/{app_id}"
    files = []

    def walk(path: str):
        for entry in list_dir(host, "/api/files", path):
            if entry.get("is_dir"):
                walk(entry["path"])
                continue
            rel = entry["path"][len(root) + 1:]
            base = rel.rsplit("/", 1)[-1]
            if base in SKIP_NAMES or base.endswith(SKIP_SUFFIXES):
                continue
            files.append((rel, entry["path"], int(entry.get("size", 0))))

    walk(root)
    return files


def sd_app_files(host: str, app_id: str):
    root = f"{SD_APPS}/{app_id}"
    found = {}

    def walk(path: str):
        for entry in list_dir(host, "/api/sd-files", path):
            if entry.get("is_dir"):
                walk(entry["path"])
                continue
            rel = entry["path"][len(root) + 1:]
            found[rel] = int(entry.get("size", 0))

    walk(root)
    return found


def read_fat_file(host: str, fat_path: str) -> bytes:
    return http_download(host, "/files" + fat_path, timeout=90)


def ensure_sd_dirs(host: str, app_id: str, rels):
    dirs = {f"{SD_APPS}/{app_id}"}
    for rel in rels:
        parent = rel.rsplit("/", 1)[0] if "/" in rel else ""
        if parent:
            dirs.add(f"{SD_APPS}/{app_id}/{parent}")
    for directory in sorted(dirs, key=lambda value: value.count("/")):
        status, _, body = http_json(host, "/api/sd-files/mkdir", "POST",
                                    {"path": directory, "recursive": True})
        if status != 200:
            fail(f"mkdir {directory} failed: HTTP {status} {body[:200]!r}")


def copy_app(host: str, app_id: str, files, dry_run: bool):
    launcher = [f for f in files if f[0] == "launcher.json"]
    if len(launcher) != 1:
        fail(f"{app_id}: launcher.json missing or duplicated")
    ordered = [f for f in files if f[0] != "launcher.json"] + launcher
    rels = [f[0] for f in ordered]

    # Validate the manifest id without trusting the directory name.
    manifest = json.loads(read_fat_file(host, launcher[0][1]).decode("utf-8"))
    if manifest.get("id") != app_id:
        fail(f"{app_id}: launcher id is {manifest.get('id')!r}")
    entry = manifest.get("entry")
    if entry not in rels:
        fail(f"{app_id}: launcher entry {entry!r} is not in the package")

    total = sum(f[2] for f in ordered)
    print(f"{app_id}: {len(ordered)} files, {human_bytes(total)}")
    for rel, fat_path, size in ordered:
        print(f"  {rel} ({human_bytes(size)})")
    if dry_run:
        return

    ensure_sd_dirs(host, app_id, rels)

    for rel, fat_path, size in ordered:
        data = read_fat_file(host, fat_path)
        if len(data) != size:
            fail(f"{app_id}/{rel}: downloaded {len(data)} bytes, expected {size}")
        remote = f"{SD_APPS}/{app_id}/{rel}"
        status, body = http_upload(
            host, "/api/sd-files/upload?path=" + quote_query(remote), data,
            timeout=90)
        if status != 200:
            fail(f"upload {remote} failed: HTTP {status} {body[:200]!r}")
        print(f"  -> /sdcard{remote}")

    # Size-verify the whole SD package before anything is deleted.
    got = sd_app_files(host, app_id)
    expected = {rel: size for rel, _, size in ordered}
    missing = sorted(set(expected) - set(got))
    extra = sorted(set(got) - set(expected))
    wrong = sorted(rel for rel in expected if rel in got and got[rel] != expected[rel])
    if missing or extra or wrong:
        fail(f"{app_id}: SD verify failed missing={missing[:4]} "
             f"extra={extra[:4]} wrong={wrong[:4]}")
    print(f"{app_id}: SD copy verified ({len(expected)} files)")


def delete_sd_probe(host: str, dry_run: bool):
    if dry_run:
        print(f"would delete SD probe: {SD_PROBE}")
        return
    status, data, body = http_json(
        host, "/api/sd-files?path=" + quote_query(SD_PROBE) + "&recursive=1",
        "DELETE")
    if status == 404:
        print("SD probe already absent")
    elif status != 200:
        fail(f"delete {SD_PROBE} failed: HTTP {status} {body[:200]!r}")
    else:
        print(f"deleted {SD_PROBE}")


def delete_fat_apps(host: str, app_ids, dry_run: bool):
    for app_id in app_ids:
        path = f"{FAT_APPS}/{app_id}"
        if dry_run:
            print(f"would delete FAT app: {path}")
            continue
        status, data, body = http_json(
            host, "/api/files?path=" + quote_query(path) + "&recursive=1",
            "DELETE")
        if status != 200:
            fail(f"delete {path} failed: HTTP {status} {body[:200]!r}")
        print(f"deleted {path}")


def wait_for_reboot(host: str, previous_boot: str, timeout: int = 60) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(1)
        status, parsed, _ = try_json(host, "/api/status", timeout=3)
        if status == 200 and isinstance(parsed, dict):
            boot = parsed.get("boot_id")
            if boot and boot != previous_boot:
                print(f"device rebooted: boot_id={boot}")
                return True
    return False


def restart(host: str, dry_run: bool):
    if dry_run:
        print("would POST /api/restart and wait for a new boot_id")
        return
    _, before, _ = http_json(host, "/api/status")
    boot_id = before.get("boot_id", "") if isinstance(before, dict) else ""
    status, _, body = http_json(host, "/api/restart", "POST", {})
    if status != 200:
        fail(f"restart failed: HTTP {status} {body[:200]!r}")
    if not wait_for_reboot(host, boot_id):
        fail("device did not report a new boot_id within 60 s", 2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Device IP or http://host")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the plan; do not touch the device")
    parser.add_argument("--copy-only", action="store_true",
                        help="Copy apps to SD but do not delete the SD probe, "
                             "FAT app dirs, or restart")
    parser.add_argument("--no-restart", action="store_true",
                        help="Do not restart after deleting the FAT copies")
    args = parser.parse_args()

    host = normalize_host(args.host)
    status, parsed, _ = http_json(host, "/api/status")
    if not isinstance(parsed, dict):
        fail("cannot read /api/status")

    apps = sorted(e["name"] for e in list_dir(host, "/api/files", FAT_APPS)
                  if e.get("is_dir"))
    if not apps:
        print("no FAT apps to move")

    print(f"FAT apps: {', '.join(apps) if apps else '-'}")

    packages = {}
    for app_id in apps:
        files = collect_fat_files(host, app_id)
        packages[app_id] = files
        copy_app(host, app_id, files, args.dry_run)

    if args.copy_only:
        print("copy-only: SD copies are ready; FAT apps and SD probe were "
              "left untouched and the device was not restarted")
        print("pending approval: delete /apps/sd_probe, delete FAT "
              + ", ".join(f"/data/apps/{a}" for a in apps) + ", then restart")
        return 0

    # All copies are verified before this point; now remove the probe and the
    # shadowing FAT packages.
    delete_sd_probe(host, args.dry_run)
    delete_fat_apps(host, apps, args.dry_run)

    if not args.no_restart:
        restart(host, args.dry_run)
    else:
        print("restart skipped; the launcher will still show the FAT apps "
              "until the next boot")

    if args.dry_run:
        print("dry-run: nothing was changed")
    else:
        print("done: apps now live on /sdcard/apps, FAT app dirs removed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
