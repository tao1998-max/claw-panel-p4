#!/usr/bin/env python3
"""从 i18n.c 里抽出所有界面文案用到的汉字，生成 charset.txt。

用法：
    python tools/fonts/collect_chars.py

界面文案只允许写在 main/core/i18n.c 里，所以这个脚本只扫那一个文件。
任何时候改了文案，都要重新跑本脚本 + gen_fonts.ps1，否则新增的汉字
不在字库子集里，屏上会显示成方框。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
I18N_C = ROOT / "main" / "core" / "i18n.c"
OUT = Path(__file__).resolve().parent / "charset.txt"

# 除了汉字，再补几个界面里会用到的全角/特殊符号
# • 是密码输入框的掩码字符（lv_textarea 密码模式）
EXTRA = "·—…%℃•"

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}


def unescape(raw: str) -> str:
    out = []
    i = 0
    while i < len(raw):
        ch = raw[i]
        if ch == "\\" and i + 1 < len(raw):
            out.append(ESCAPES.get(raw[i + 1], raw[i + 1]))
            i += 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def main() -> int:
    # Windows 控制台默认 GBK，直接 print 汉字会抛 UnicodeEncodeError。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    if not I18N_C.exists():
        print(f"找不到 {I18N_C}", file=sys.stderr)
        return 1

    text = I18N_C.read_text(encoding="utf-8")
    # 注释里的汉字不进字库，先把注释剥掉再扫字符串
    text = COMMENT_RE.sub("", text)
    chars = set()
    for raw in STRING_RE.findall(text):
        for ch in unescape(raw):
            if ord(ch) > 0x7F:
                chars.add(ch)
    chars.update(EXTRA)

    ordered = "".join(sorted(chars))
    OUT.write_text(ordered + "\n", encoding="utf-8")
    print(f"字符集：{len(ordered)} 个 -> {OUT}")
    print(ordered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
