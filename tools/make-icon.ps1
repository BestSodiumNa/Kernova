# Generates res\app.ico: the sidebar logo (rounded cyan -> violet gradient tile with the
# white "performance" glyph) rendered at every size Windows asks for, packed as PNG entries.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "res\app.ico"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$cyan = [System.Drawing.Color]::FromArgb(255, 0, 212, 255)
$violet = [System.Drawing.Color]::FromArgb(255, 139, 92, 246)
$glyph = [string][char]0xE9D9   # same glyph as the sidebar logo
$fontName = if ((New-Object System.Drawing.Text.InstalledFontCollection).Families.Name -contains "Segoe Fluent Icons") { "Segoe Fluent Icons" } else { "Segoe MDL2 Assets" }

function RoundedRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

$images = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    # Small sizes use the whole square so the tile stays legible in the taskbar.
    $pad = if ($s -le 24) { 0 } else { [single]($s * 0.04) }
    $w = [single]($s - 2 * $pad)
    $radius = [single]($w * 0.24)
    $path = RoundedRect $pad $pad $w $w $radius
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF $pad, $pad), (New-Object System.Drawing.PointF ($pad + $w), ($pad + $w)), $cyan, $violet
    $g.FillPath($brush, $path)
    # Soft top highlight for depth on the larger sizes.
    if ($s -ge 48) {
        # Clipped to the tile and drawn only over its top part, so the gradient never repeats.
        $hlH = [single]($w * 0.55)
        $hl = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, ($pad - 1)), (New-Object System.Drawing.PointF 0, ($pad + $hlH + 1)), ([System.Drawing.Color]::FromArgb(55, 255, 255, 255)), ([System.Drawing.Color]::FromArgb(0, 255, 255, 255))
        $g.SetClip($path)
        $g.FillRectangle($hl, $pad, $pad, $w, $hlH)
        $g.ResetClip()
    }
    if ($s -le 32) {
        # Small sizes: just a bold pulse line; the framed glyph turns to mush below ~40 px.
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([single][Math]::Max(1.6, $w * 0.11))
        $pen.LineJoin = 'Round'; $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
        $u = $w / 16.0
        $pts = @((2.6, 8.6), (5.6, 8.6), (7.0, 4.4), (9.2, 12.0), (10.6, 8.6), (13.4, 8.6)) | ForEach-Object { New-Object System.Drawing.PointF ([single]($pad + $_[0] * $u)), ([single]($pad + $_[1] * $u)) }
        $g.DrawLines($pen, [System.Drawing.PointF[]]$pts)
    } else {
        $font = New-Object System.Drawing.Font $fontName, ([single]($w * 0.58)), ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
        $fmt = New-Object System.Drawing.StringFormat
        $fmt.Alignment = 'Center'; $fmt.LineAlignment = 'Center'
        $rect = New-Object System.Drawing.RectangleF $pad, ($pad + $w * 0.02), $w, $w
        $g.DrawString($glyph, $font, [System.Drawing.Brushes]::White, $rect, $fmt)
    }
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , @($s, $ms.ToArray())
}

# ICO container: ICONDIR, one ICONDIRENTRY per image, then the PNG data.
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $s = $img[0]; $data = $img[1]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$data.Length); $bw.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($img in $images) { $bw.Write([byte[]]$img[1]) }
$bw.Close()
"Wrote $out ($($images.Count) sizes, font: $fontName)"
