#!/usr/bin/env python3
"""Launch or self-test an installed App through the device Web API.

This closes the loop for external AI tools: upload the package, ask the device
to run it for a few seconds, and read the exact Lua error without a human
tapping the screen or a serial console.

    # list installed Apps (FAT and SD)
    python scripts/run_app.py --host http://192.168.1.179 --list

    # upload + refresh + run for 3 s + stop, then report pass/fail
    python scripts/run_app.py --host http://192.168.1.179 \
        --app-id weather_panel --duration 3 --refresh

    # just start it and print the job id
    python scripts/run_app.py --host http://192.168.1.179 \
        --app-id weather_panel --launch

Exit code: 0 when the App did not crash, 1 when it failed (the error text is
printed), 2 on request/setup errors.
"""

from __future__ import annotations

import argparse
import json
import sys

from _common import fail, http_json, normalize_host

RESULT_LABELS = {
    "running": "PASS (still running, stopped by the test)",
    "finished": "PASS (exited cleanly on its own)",
    "failed": "FAIL (Lua error)",
    "timeout": "FAIL (job timed out)",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, help="Device IP or http://host")
    parser.add_argument("--list", action="store_true", help="List installed Apps and exit")
    parser.add_argument("--app-id", help="App id to launch/test")
    parser.add_argument("--duration", type=float, default=3.0,
                        help="Seconds to let the App run before stopping it (default 3)")
    parser.add_argument("--launch", action="store_true",
                        help="Only launch the App; do not stop it")
    parser.add_argument("--refresh", action="store_true",
                        help="Rescan the app roots first (needed after an upload)")
    parser.add_argument("--json", action="store_true", help="Emit raw JSON")
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    args = parser.parse_args()

    host = normalize_host(args.host)

    if args.refresh:
        status, body, raw = http_json(host, "/api/apps/refresh", "POST", {})
        if status != 200:
            fail(f"refresh failed: HTTP {status} {raw[:200]!r}", 2)

    if args.list:
        status, body, raw = http_json(host, "/api/apps")
        if status != 200 or not isinstance(body, dict):
            fail(f"cannot list Apps: HTTP {status} {raw[:200]!r}", 2)
        items = body.get("items", [])
        if args.json:
            print(json.dumps(items, ensure_ascii=False, indent=2))
            return 0
        if not items:
            print("no Apps installed")
            return 0
        print(f"{'id':24} {'version':9} {'volume':6} name")
        for item in items:
            print(f"{item.get('id', ''):24} {item.get('version') or '-':9} "
                  f"{item.get('volume', ''):6} {item.get('name', '')}")
        return 0

    if not args.app_id:
        fail("pass --list or --app-id", 2)

    if args.launch:
        status, body, raw = http_json(host, f"/api/apps/{args.app_id}/launch", "POST", {})
        if status != 200 or not isinstance(body, dict):
            fail(f"launch failed: HTTP {status} {raw[:200]!r}", 2)
        if args.json:
            print(json.dumps(body, ensure_ascii=False, indent=2))
        else:
            print(f"launched {body.get('name', args.app_id)} "
                  f"(job {body.get('job_id', '?')})")
        return 0

    duration_ms = max(300, int(args.duration * 1000))
    url = f"/api/apps/{args.app_id}/test?duration_ms={duration_ms}"
    status, body, raw = http_json(host, url, "POST", {}, timeout=max(10.0, args.duration + 10))
    if status != 200 or not isinstance(body, dict):
        fail(f"test request failed: HTTP {status} {raw[:200]!r}", 2)

    if args.json:
        print(json.dumps(body, ensure_ascii=False, indent=2))
    else:
        result = body.get("result", "unknown")
        print(f"{body.get('name', args.app_id)} [{body.get('id', '')}]: "
              f"{RESULT_LABELS.get(result, result)}")
        print(f"  job={body.get('job_id', '?')} duration_ms={body.get('duration_ms', '?')}")
        if body.get("error"):
            print(f"  error: {body['error'].strip()}")

    return 0 if body.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
