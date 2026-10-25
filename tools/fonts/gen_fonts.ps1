# 生成中文字库子集（16 / 20 / 28 三档）
#
# 前置：python tools/fonts/collect_chars.py 生成 charset.txt
# 用法：
#   pwsh -File tools/fonts/gen_fonts.ps1
#   pwsh -File tools/fonts/gen_fonts.ps1 -CjkFont C:\Windows\Fonts\simhei.ttf
#
# 换字体只需要换 -CjkFont，不用改代码；生成的 .c 会被 main/CMakeLists.txt 直接编译。

param(
    [string]$CjkFont = 'C:\Windows\Fonts\deng.ttf',
    [int[]]$Sizes = @(16, 20, 28)
)

$ErrorActionPreference = 'Stop'

# 必须解析成绝对路径：PowerShell 的 .NET 静态文件 API 用的是进程 CWD，
# 从别的目录调用脚本时相对路径会读到空文件（--symbols 就变成空参数）。
$here = (Resolve-Path (Split-Path -Parent $MyInvocation.MyCommand.Path)).Path
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$cli = Join-Path $here 'node_modules\lv_font_conv\lv_font_conv.js'
$charsetPath = Join-Path $here 'charset.txt'
$outDir = Join-Path $root 'main\ui\fonts'

if (-not (Test-Path $CjkFont)) { throw "找不到字体文件: $CjkFont" }
if (-not (Test-Path $cli)) { throw "缺少 lv_font_conv，先运行: npm install --prefix tools/fonts lv_font_conv" }
if (-not (Test-Path $charsetPath)) { throw "缺少 charset.txt，先运行: python tools/fonts/collect_chars.py" }

# 去掉首尾空白和换行，否则会多带一个不可见字形
# 必须显式按 UTF-8 读：Windows PowerShell 5.1 的 Get-Content 默认是 ANSI，
# 会把汉字读成乱码，传给的 --symbols 就变成一堆不存在的码位。
$charset = [System.IO.File]::ReadAllText($charsetPath, [System.Text.Encoding]::UTF8).Trim()
Write-Host "字体: $CjkFont"
Write-Host "字符数: $($charset.Length)"

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

foreach ($size in $Sizes) {
    $out = Join-Path $outDir ("font_zh_$size.c")
    Write-Host "生成 font_zh_$size.c ..."
    & node $cli --font $CjkFont --range '0x20-0x7E' --symbols $charset `
        --size $size --bpp 4 --format lvgl --no-compress --lv-include lvgl.h `
        --force-fast-kern-format -o $out
    if ($LASTEXITCODE -ne 0) { throw "lv_font_conv 失败 (size=$size)" }
}

Get-ChildItem (Join-Path $outDir 'font_zh_*.c') |
    Select-Object Name, @{n = 'KB'; e = { [math]::Round($_.Length / 1KB, 1) } } |
    Format-Table -AutoSize
