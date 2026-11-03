---
{
  "name": "app_creator",
  "description": "Create, update, publish, or remove an on-device launcher App when the user explicitly requests a launcher-visible Lua application; not for Skills or generic Lua scripts.",
  "metadata": {
    "cap_groups": [
      "cap_files",
      "cap_lua",
      "cap_app_manage"
    ]
  }
}
---

# App Creator

Use this skill only when the user explicitly requests creation, update, or removal of a launcher-visible runtime App. Apps are independent packages consumed by the System UI Launcher; they are not Skills. Use `skill_creator` when the requested behavior should be invoked conversationally by the model instead of launched from the UI.

This workflow manages runtime Apps under the writable DATA root. It does not edit firmware-baked SYSTEM Apps or create native capabilities.

## Board Constraints

This product has no screenshot or image-inspection path:

- `claw.screenshot` does not exist. Do not capture the framebuffer, read canvas pixels, or render glyph grids to verify fonts or layout.
- For timer/countdown UIs use ASCII digits and built-in Montserrat fonts by name: `font = "montserrat_48"` (available `montserrat_14`/`20`/`24`/`32`/`48`; `"default"` means 14). Do not call `lvgl.font_load()` without a path: this product does not ship the default DATA font. If the App ships a TTF for Chinese, pass its path and stay within its documented subset; never fabricate binary font data.
- The style key is `font` (`text_font` is accepted as an alias) and takes a built-in name string or a `lvgl.font_load()` object. Other size names are not compiled in and are ignored, so use only the names above.
- Do not call `restart_device` while creating an App. `publish_app` refreshes the launcher; if a restart is truly required, ask the user.
- Validate the package by reading files and checking the manifest, imports and paths, then publish once.

## Voice / TTS

Apps may speak with `claw.speak(text[, timeout_ms])` from the Claw runtime.

- It is explicit and works even when automatic Agent-reply speech is off.
- It blocks the Lua task until playback finishes; use short phrases and keep it
  out of animation/timer loops.
- Doubao credentials are global device settings (Claw → 设置 → 豆包语音 /
  `POST /api/tts/config`). Never embed keys in an App package.
- TTS is separate from `audio.play()`: use `claw.speak` for arbitrary text,
  `audio` for bundled OGG/MP3 effects and music.

## Resolve The Runtime Apps Root

Before reading or writing a runtime App, run:

```json
{"path":"{CUR_SKILL_DIR}/scripts/resolve_apps_root.lua","args":{}}
```

Treat the returned absolute path as `<APPS_ROOT>`. Do not assume `/fatfs`; DATA may be mounted on an SD card. If the path cannot be resolved, report the error and stop.

## App Contract

Before creating or updating an App, read `{CUR_SKILL_DIR}/references/app-package-spec.md` and follow the complete package contract. Do not load the reference for removal-only requests.

Store each runtime App under `<APPS_ROOT>/<app_id>/`. Do not overwrite an existing App unless the user explicitly requested an update to that id.

Minimal manifest:

```json
{
  "schema_version": 1,
  "id": "example_app",
  "display_name": "Example App",
  "entry": "scripts/main.lua",
  "visible": true
}
```

## Quick Path (small Apps)

For a single-screen App, scaffold a known-good package instead of authoring the
manifest and boilerplate by hand:

1. Run the scaffold with `lua_run_script`:

```json
{"path":"{CUR_SKILL_DIR}/scripts/scaffold_app.lua","args":{"app_id":"my_app","display_name":"我的应用","title":"MY APP"}}
```

It creates `<APPS_ROOT>/my_app/launcher.json` and a working
`scripts/main.lua` (dark screen, ASCII title in `montserrat_20`, live clock in
`montserrat_32`, `claw.abort_requested()` loop). `display_name` may be Chinese;
`title` must be ASCII because the App ships no TTF.

2. If the request needs a custom screen, replace only `scripts/main.lua` with
`write_file`, importing just the modules it needs.
3. Call `publish_app` with the id.

For the quick path, skip the full package-spec read and read only the doc of a
module you actually add (for example `lua_module_lvgl.md` when using widgets).
Do not read every module doc. Use the full `Create Or Update` flow when the App
needs assets, a TTF, a manifest icon, or several files.

## Create Or Update

1. Resolve `<APPS_ROOT>` and inspect the exact target package before changing it.
2. Read the package spec, then activate `lua_package_authoring` and follow its App-script rules.
3. Write scripts and existing accessible assets first, then write `launcher.json` last. Do not fabricate binary JPEG data through a text file tool; omit the icon when no valid asset is available.
4. Read the completed text files back and verify the manifest types, id, relative paths, entry existence, Lua imports, and packaged asset references.
5. Call `publish_app` with only the final id:

```json
{"app_id":"example_app"}
```

6. Treat the tool result as authoritative. Report publication errors directly and do not claim success when publication fails.

The Launcher starts the App entry asynchronously with manifest `args`, a stable job name derived from the App id, and exclusive display ownership. Do not run a long-lived UI App merely to validate its package.

## Remove

Remove an App only when the user explicitly requests deletion of that exact runtime App. Inspect the target first, then call `remove_app` with only its `app_id`. If the tool reports `readonly_app`, explain that firmware-baked SYSTEM Apps cannot be removed at runtime.
