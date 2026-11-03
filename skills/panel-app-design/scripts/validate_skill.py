#!/usr/bin/env python3
"""Static validation for the panel-app-design Agent Skill."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

from _common import fail


def parse_frontmatter(text: str) -> dict[str, str]:
    if not text.startswith("---"):
        return {}
    end = text.find("\n---", 3)
    if end < 0:
        return {}
    block = text[3:end].strip().splitlines()
    result = {}
    for line in block:
        if ":" in line:
            key, value = line.split(":", 1)
            result[key.strip()] = value.strip().strip("\"'")
    return result


def run_help(script: Path) -> tuple[bool, str]:
    try:
        result = subprocess.run([sys.executable, str(script), "--help"],
                                text=True, capture_output=True, timeout=20)
    except subprocess.TimeoutExpired:
        return False, "timeout"
    if result.returncode != 0:
        return False, (result.stderr or result.stdout)[-400:]
    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skill-dir", required=True, type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    skill_dir = args.skill_dir.resolve()
    if not skill_dir.is_dir():
        fail(f"not a directory: {skill_dir}")
    issues: list[dict[str, str]] = []

    def error(message: str):
        issues.append({"level": "error", "message": message})

    def warn(message: str):
        issues.append({"level": "warning", "message": message})

    def ok(message: str):
        issues.append({"level": "ok", "message": message})

    skill_md = skill_dir / "SKILL.md"
    if not skill_md.is_file():
        error("SKILL.md is missing")
        text = ""
    else:
        text = skill_md.read_text(encoding="utf-8")
        lines = text.count("\n") + 1
        if lines >= 500:
            error(f"SKILL.md has {lines} lines (must be <500)")
        else:
            ok(f"SKILL.md has {lines} lines")
        meta = parse_frontmatter(text)
        if not meta.get("name"):
            error("SKILL.md frontmatter is missing name")
        if not meta.get("description"):
            error("SKILL.md frontmatter is missing description")
        else:
            description = meta["description"].lower()
            for word in ("panel", "sd", "fallback"):
                if word not in description:
                    warn(f"SKILL.md description does not mention {word!r}")
        for linked in re.findall(r"\]\((references/[^)]+)\)", text):
            if not (skill_dir / linked).is_file():
                error(f"SKILL.md links missing reference: {linked}")

    for name in ("README.md", "CHANGELOG.md"):
        if not (skill_dir / name).is_file():
            error(f"{name} is missing")

    scripts_dir = skill_dir / "scripts"
    scripts = [p for p in sorted(scripts_dir.glob("*.py")) if p.name != "_common.py"] if scripts_dir.is_dir() else []
    if not scripts:
        error("scripts/ is empty")
    for script in scripts:
        good, detail = run_help(script)
        if good:
            ok(f"script --help: {script.name}")
        else:
            error(f"script --help failed: {script.name}: {detail}")

    refs = skill_dir / "references"
    if not refs.is_dir():
        error("references/ is missing")
    else:
        for path in sorted(refs.glob("*.md")):
            if not path.read_text(encoding="utf-8").strip():
                error(f"empty reference: {path.name}")

    evals_path = skill_dir / "evals" / "evals.json"
    if not evals_path.is_file():
        error("evals/evals.json is missing")
    else:
        try:
            evals = json.loads(evals_path.read_text(encoding="utf-8"))
            prompts = evals.get("evals", evals if isinstance(evals, list) else [])
            if not 3 <= len(prompts) <= 5:
                error(f"evals.json should contain 3-5 prompts, found {len(prompts)}")
            else:
                ok(f"evals.json has {len(prompts)} prompts")
        except json.JSONDecodeError as exc:
            error(f"evals.json is invalid: {exc}")

    triggers_path = skill_dir / "evals" / "trigger_queries.json"
    if not triggers_path.is_file():
        error("evals/trigger_queries.json is missing")
    else:
        try:
            triggers = json.loads(triggers_path.read_text(encoding="utf-8"))
            items = triggers.get("queries", triggers if isinstance(triggers, list) else [])
            if not 10 <= len(items) <= 20:
                error(f"trigger_queries.json should have 10-20 queries, found {len(items)}")
            else:
                ok("trigger_queries.json has 10-20 queries")
        except json.JSONDecodeError as exc:
            error(f"trigger_queries.json is invalid: {exc}")

    app_validator = scripts_dir / "validate_app.py"
    examples = skill_dir / "assets" / "examples"
    if app_validator.is_file() and examples.is_dir():
        targets = [("countdown", examples / "countdown", None, None),
                   ("mqtt_panel", examples / "mqtt_panel", None, None),
                   ("network_radio", examples / "network_radio", None, None),
                   ("anim_lab", examples / "anim_lab" / "app", "anim_lab", None),
                   ("sd_fallback", examples / "sd_fallback" / "app", "sd_fallback",
                    examples / "sd_fallback" / "sd")]
        for name, app_dir, remote_id, sd_dir in targets:
            if not app_dir.is_dir():
                error(f"example missing: {name}")
                continue
            cmd = [sys.executable, str(app_validator), "--app-dir", str(app_dir)]
            if remote_id:
                cmd += ["--remote-id", remote_id]
            if sd_dir:
                cmd += ["--sd-dir", str(sd_dir)]
            result = subprocess.run(cmd, text=True, capture_output=True)
            if result.returncode != 0:
                error(f"example validation failed: {name}\n{result.stdout}{result.stderr}")
            else:
                ok(f"example validates: {name}")

    if args.json:
        print(json.dumps({"skill_dir": str(skill_dir), "issues": issues}, indent=2))
    else:
        for issue in issues:
            print(f"[{issue['level']}] {issue['message']}")
    return 1 if any(i["level"] == "error" for i in issues) else 0


if __name__ == "__main__":
    sys.exit(main())
