# sd_fallback example

A 1024x600 poster-style App that works with or without an SD card.

- `app/` is the FAT package. It contains a 1024x600 q48 background and a
  180x180 opaque sigil fallback, plus the Chinese TTF subset.
- `sd/panel-assets/sd_fallback/` is the optional SD tree: a q90 background and
  a transparent raw ARGB8888 sigil.
- `sd_source/` holds the source art; `sd_plan.json` is the input to
  `prepare_sd_assets.py`.
- `sd_manifest.json` is tooling-only; `upload_app.py` does not upload it.

Run without SD assets: the App logs `bg=fat sigil=fat` and renders the fallback.
Upload the SD tree and relaunch: it logs `bg=sd sigil=sd`.

Regenerate:

```bash
python ../../../scripts/prepare_sd_assets.py --spec sd_plan.json
```
