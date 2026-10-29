#!/usr/bin/env python3
"""Generate the AI asset folder for the XiaoZhi AI feature.

Outputs (committed, so a normal build does not need Python). The build packs
this folder as /ai into the 4 MB claw FAT image; flash it with
`idf.py claw-flash`.
    assets/ai/font_puhui_common_20_4.bin
    assets/ai/srmodels.bin
    assets/ai/THIRD_PARTY_NOTICES.txt

Run:
    python tools/ai_assets/build_assets.py
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets" / "ai"
SRC = ROOT / "assets" / "ai_src"
MODEL_DIR = ROOT / "managed_components" / "espressif__esp-sr" / "model" / "wakenet_model" / "wn9_nihaoxiaozhi_tts"
PACKER = ROOT / "managed_components" / "espressif__esp-sr" / "model" / "pack_model.py"
STAGE = ROOT / "build" / "ai_assets" / "srmodels"

NOTICES = """XiaoZhi AI assets
=================

- XiaoZhi-ESP32 client code (vendored, MIT):
  Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
  and Project Contributors. See main/ai/vendor/LICENSE.xiaozhi.

- Wake word model (wn9_nihaoxiaozhi_tts) comes from espressif/esp-sr 2.3.1.
  ESP-SR is distributed under the Espressif MIT License; use is limited to
  Espressif products. "你好小智" / "XiaoZhi" brand rights belong to their owners.

- Chat font font_puhui_common_20_4.bin comes from 78/xiaozhi-fonts 1.6.0
  (MIT component). The underlying Puhui/Noto glyph data may carry separate
  font terms; verify before commercial redistribution.

- Prompt OGG sounds (popup/success/activation/exclamation) come from the
  XiaoZhi-ESP32 assets (MIT).
- 78/esp-ml307 is Apache-2.0.
- esp_audio_codec / esp_audio_effects are Espressif Modified MIT, restricted
  to Espressif products.
- Material Design Icons (robot glyphs) are Apache-2.0, Pictogrammers.
"""


def main() -> int:
    if not MODEL_DIR.is_dir():
        print(f"missing model dir: {MODEL_DIR}", file=sys.stderr)
        return 1
    if not PACKER.is_file():
        print(f"missing packer: {PACKER}", file=sys.stderr)
        return 1

    OUT.mkdir(parents=True, exist_ok=True)
    font = SRC / "font_puhui_common_20_4.bin"
    if not font.is_file():
        print(f"missing font: {font}", file=sys.stderr)
        return 1
    shutil.copy2(font, OUT / font.name)

    sounds = SRC / "sounds"
    if sounds.is_dir():
        for ogg in sorted(sounds.glob("*.ogg")):
            shutil.copy2(ogg, OUT / ogg.name)

    if STAGE.exists():
        shutil.rmtree(STAGE)
    STAGE.mkdir(parents=True)
    shutil.copytree(MODEL_DIR, STAGE / MODEL_DIR.name)

    srmodels = OUT / "srmodels.bin"
    subprocess.run(
        [sys.executable, str(PACKER), "-m", str(STAGE), "-o", str(srmodels)],
        check=True,
    )

    (OUT / "THIRD_PARTY_NOTICES.txt").write_text(NOTICES, encoding="utf-8")

    for path in sorted(OUT.iterdir()):
        print(f"{path.name}: {path.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
