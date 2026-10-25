# Generate the 30px network icon font subset from Material Design Icons.
# Source: tools/fonts/mdi/materialdesignicons-webfont.ttf (Pictogrammers, Apache-2.0)
# Output: main/ui/fonts/font_net_30.c
$ErrorActionPreference = 'Stop'

$here = (Resolve-Path (Split-Path -Parent $MyInvocation.MyCommand.Path)).Path
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$cli = Join-Path $here 'node_modules\lv_font_conv\lv_font_conv.js'
$font = Join-Path $here 'mdi\materialdesignicons-webfont.ttf'
$out = Join-Path $root 'main\ui\fonts\font_net_30.c'

if (-not (Test-Path $font)) { throw "Missing icon font: $font" }
if (-not (Test-Path $cli)) { throw "Missing lv_font_conv. Run: npm install --prefix tools/fonts lv_font_conv" }

& node $cli --font $font `
    --range 0xF091F --range 0xF0922 --range 0xF0925 --range 0xF0928 --range 0xF0200 `
    --size 30 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h `
    --force-fast-kern-format -o $out

if ($LASTEXITCODE -ne 0) { throw "lv_font_conv failed" }
Write-Host "Generated $out"
