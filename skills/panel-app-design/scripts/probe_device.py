#!/usr/bin/env python3
"""Probe a panel over the LAN and report FAT/SD storage state."""

from __future__ import annotations

import argparse
import json
import sys

from _common import human_bytes, http_json


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Device IP or http://host")
    parser.add_argument("--require-sd", action="store_true",
                        help="Exit non-zero unless /sdcard is mounted")
    parser.add_argument("--json", action="store_true", help="Emit JSON")
    parser.add_argument("--timeout", type=int, default=8, help="HTTP timeout seconds")
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    args = parser.parse_args()

    status, app_status, _ = http_json(args.host, "/api/status", timeout=args.timeout)
    if status != 200 or not isinstance(app_status, dict):
        print(f"error: /api/status returned HTTP {status}", file=sys.stderr)
        return 2

    storage_status, storage, _ = http_json(args.host, "/api/storage", timeout=args.timeout)
    if storage_status != 200 or not isinstance(storage, dict):
        result = {
            "host": args.host,
            "status": app_status,
            "storage": None,
            "warning": "firmware does not expose /api/storage (update firmware or copy SD assets manually)",
        }
        if args.json:
            print(json.dumps(result, ensure_ascii=False, indent=2))
        else:
            print(f"device: {app_status.get('ip') or args.host}")
            print("FAT/SD: /api/storage unavailable on this firmware")
            print("hint: update firmware, or copy sd/panel-app-assets to the card manually")
        return 2

    fat = storage.get("fat", {})
    sd = storage.get("sd", {})
    result = {"host": args.host, "status": app_status, "storage": storage}

    # SD-hosted apps live in <mount>/apps. Older firmware returns 404; treat
    # that as "not supported" rather than an error.
    sd_apps = None
    if sd.get("mounted"):
        apps_http, apps_body, _ = http_json(args.host, "/api/sd-files?path=/apps",
                                            timeout=args.timeout)
        if apps_http == 200 and isinstance(apps_body, dict):
            sd_apps = sorted(entry.get("name") for entry in apps_body.get("entries", [])
                             if entry.get("is_dir"))
    result["sd_apps"] = sd_apps

    # Network modules shipped with this firmware generation. Older firmware
    # simply lacks /api/lua-modules (404) or does not list them.
    network_modules = {}
    modules_http, modules_body, _ = http_json(args.host, "/api/lua-modules",
                                              timeout=args.timeout)
    if modules_http == 200 and isinstance(modules_body, dict):
        ids = {item.get("module_id") for item in modules_body.get("items", [])}
        network_modules = {name: name in ids
                           for name in ("net", "mqtt", "websocket")}
    result["lua_network_modules"] = network_modules

    # Recent Lua job failures (App debugging: ask the user to launch the App,
    # then read the exact error here instead of the serial console).
    failures = None
    fail_http, fail_body, _ = http_json(args.host, "/api/lua-failures?limit=3",
                                        timeout=args.timeout)
    if fail_http == 200 and isinstance(fail_body, dict):
        failures = fail_body.get("items", [])
    result["lua_failures"] = failures

    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print(f"device: {app_status.get('ip') or args.host} (boot {app_status.get('boot_id', '?')})")
        print(f"FAT {fat.get('mount', '?')}: mounted={fat.get('mounted')} "
              f"total={human_bytes(fat.get('total', 0))} free={human_bytes(fat.get('free', 0))}")
        print(f"SD  {sd.get('mount', '?')}: mounted={sd.get('mounted')} "
              f"total={human_bytes(sd.get('total', 0))} free={human_bytes(sd.get('free', 0))}")
        assets = storage.get("sd_assets", {})
        print(f"SD assets: {assets.get('root', '?')} exists={assets.get('exists')}")
        if sd_apps is not None:
            print(f"SD apps: {len(sd_apps)}" + (f" -> {', '.join(sd_apps)}" if sd_apps else ""))
        elif sd.get("mounted"):
            print("SD apps: /api/sd-files unavailable on this firmware")
        if network_modules:
            state = ", ".join(f"{name}={'yes' if present else 'no'}"
                              for name, present in network_modules.items())
            print(f"Lua net modules: {state}")
        else:
            print("Lua net modules: /api/lua-modules unavailable (old firmware)")
        if failures is None:
            print("Lua failures: /api/lua-failures unavailable (old firmware)")
        elif not failures:
            print("Lua failures: none")
        else:
            print(f"Lua failures: {len(failures)}")
            for failure in failures:
                print(f"  - {failure.get('name', '?')} "
                      f"[{failure.get('status', '?')}] {failure.get('path', '')}")
                print(f"    {failure.get('error', '')}")

    if args.require_sd and not sd.get("mounted"):
        print("error: SD card is not mounted", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
