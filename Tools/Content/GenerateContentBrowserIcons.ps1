param(
    [string]$OutputDirectory = "Engine/Resources/Icons/ContentBrowser"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$repositoryRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$resolvedOutput = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
    [System.IO.Path]::GetFullPath($OutputDirectory)
}
else {
    [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $OutputDirectory))
}
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null

function New-RoundedRectanglePath {
    param(
        [float]$X,
        [float]$Y,
        [float]$Width,
        [float]$Height,
        [float]$Radius
    )

    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $Radius * 2.0
    $path.AddArc($X, $Y, $diameter, $diameter, 180.0, 90.0)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270.0, 90.0)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0.0, 90.0)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90.0, 90.0)
    $path.CloseFigure()
    return $path
}

function New-Icon {
    param(
        [string]$Name,
        [scriptblock]$Draw
    )

    $bitmap = [System.Drawing.Bitmap]::new(
        64,
        64,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $pen = [System.Drawing.Pen]::new([System.Drawing.Color]::White, 4.0)
    $brush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

        & $Draw $graphics $pen $brush
        $outputPath = Join-Path $resolvedOutput ($Name + ".png")
        $bitmap.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $brush.Dispose()
        $pen.Dispose()
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

New-Icon "PrefabIcon" {
    param($g, $pen, $brush)
    $g.DrawLine($pen, 32, 22, 32, 34)
    $g.DrawLine($pen, 16, 34, 48, 34)
    $g.DrawLine($pen, 16, 34, 16, 42)
    $g.DrawLine($pen, 48, 34, 48, 42)
    foreach ($rect in @(
        @(23, 7, 18, 15),
        @(7, 42, 18, 15),
        @(39, 42, 18, 15))) {
        $path = New-RoundedRectanglePath $rect[0] $rect[1] $rect[2] $rect[3] 3
        try { $g.FillPath($brush, $path) } finally { $path.Dispose() }
    }
}

New-Icon "EffectIcon" {
    param($g, $pen, $brush)
    $spark = [System.Drawing.Drawing2D.GraphicsPath]::new()
    try {
        $spark.AddPolygon([System.Drawing.PointF[]]@(
            [System.Drawing.PointF]::new(32, 6),
            [System.Drawing.PointF]::new(37, 24),
            [System.Drawing.PointF]::new(54, 31),
            [System.Drawing.PointF]::new(37, 37),
            [System.Drawing.PointF]::new(31, 56),
            [System.Drawing.PointF]::new(25, 38),
            [System.Drawing.PointF]::new(8, 31),
            [System.Drawing.PointF]::new(26, 24)))
        $g.FillPath($brush, $spark)
    }
    finally { $spark.Dispose() }
    $g.FillEllipse($brush, 49, 9, 7, 7)
    $g.FillEllipse($brush, 8, 46, 6, 6)
}

New-Icon "UiIcon" {
    param($g, $pen, $brush)
    $window = New-RoundedRectanglePath 6 9 52 46 5
    try { $g.DrawPath($pen, $window) } finally { $window.Dispose() }
    $g.DrawLine($pen, 8, 20, 56, 20)
    $g.FillEllipse($brush, 12, 14, 3, 3)
    $g.FillEllipse($brush, 18, 14, 3, 3)
    $g.FillRectangle($brush, 13, 28, 15, 19)
    $g.FillRectangle($brush, 34, 28, 17, 6)
    $g.FillRectangle($brush, 34, 40, 17, 7)
}

New-Icon "BehaviorTreeIcon" {
    param($g, $pen, $brush)
    $g.DrawLine($pen, 32, 16, 32, 29)
    $g.DrawLine($pen, 15, 29, 49, 29)
    $g.DrawLine($pen, 15, 29, 15, 44)
    $g.DrawLine($pen, 32, 29, 32, 44)
    $g.DrawLine($pen, 49, 29, 49, 44)
    $g.FillEllipse($brush, 25, 6, 14, 14)
    $g.FillEllipse($brush, 9, 43, 12, 12)
    $g.FillEllipse($brush, 26, 43, 12, 12)
    $g.FillEllipse($brush, 43, 43, 12, 12)
}

New-Icon "ScriptIcon" {
    param($g, $pen, $brush)
    $page = [System.Drawing.Drawing2D.GraphicsPath]::new()
    try {
        $page.AddLines([System.Drawing.PointF[]]@(
            [System.Drawing.PointF]::new(13, 6),
            [System.Drawing.PointF]::new(40, 6),
            [System.Drawing.PointF]::new(53, 19),
            [System.Drawing.PointF]::new(53, 58),
            [System.Drawing.PointF]::new(13, 58),
            [System.Drawing.PointF]::new(13, 6)))
        $g.DrawPath($pen, $page)
    }
    finally { $page.Dispose() }
    $g.DrawLine($pen, 40, 7, 40, 19)
    $g.DrawLine($pen, 40, 19, 52, 19)
    $g.DrawLines($pen, [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(28, 29),
        [System.Drawing.PointF]::new(21, 36),
        [System.Drawing.PointF]::new(28, 43)))
    $g.DrawLines($pen, [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(38, 29),
        [System.Drawing.PointF]::new(45, 36),
        [System.Drawing.PointF]::new(38, 43)))
}

New-Icon "ShaderIcon" {
    param($g, $pen, $brush)
    $chip = New-RoundedRectanglePath 14 14 36 36 5
    try { $g.DrawPath($pen, $chip) } finally { $chip.Dispose() }
    foreach ($position in @(21, 32, 43)) {
        $g.DrawLine($pen, $position, 7, $position, 14)
        $g.DrawLine($pen, $position, 50, $position, 57)
        $g.DrawLine($pen, 7, $position, 14, $position)
        $g.DrawLine($pen, 50, $position, 57, $position)
    }
    $g.FillEllipse($brush, 25, 25, 14, 14)
}

New-Icon "FontIcon" {
    param($g, $pen, $brush)
    $fontPen = [System.Drawing.Pen]::new([System.Drawing.Color]::White, 6.0)
    try {
        $fontPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $fontPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $fontPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
        $g.DrawLine($fontPen, 12, 54, 31, 9)
        $g.DrawLine($fontPen, 31, 9, 51, 54)
        $g.DrawLine($fontPen, 20, 38, 43, 38)
    }
    finally { $fontPen.Dispose() }
}

New-Icon "UiMarkupIcon" {
    param($g, $pen, $brush)
    $page = New-RoundedRectanglePath 7 8 50 48 5
    try { $g.DrawPath($pen, $page) } finally { $page.Dispose() }
    $g.DrawLines($pen, [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(27, 23),
        [System.Drawing.PointF]::new(18, 32),
        [System.Drawing.PointF]::new(27, 41)))
    $g.DrawLines($pen, [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(37, 23),
        [System.Drawing.PointF]::new(46, 32),
        [System.Drawing.PointF]::new(37, 41)))
    $g.DrawLine($pen, 35, 19, 29, 45)
}

New-Icon "MeshSourceIcon" {
    param($g, $pen, $brush)
    $front = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(10, 23),
        [System.Drawing.PointF]::new(32, 11),
        [System.Drawing.PointF]::new(54, 23),
        [System.Drawing.PointF]::new(32, 35),
        [System.Drawing.PointF]::new(10, 23))
    $g.DrawLines($pen, $front)
    $g.DrawLine($pen, 10, 23, 10, 44)
    $g.DrawLine($pen, 54, 23, 54, 44)
    $g.DrawLine($pen, 32, 35, 32, 56)
    $g.DrawLines($pen, [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(10, 44),
        [System.Drawing.PointF]::new(32, 56),
        [System.Drawing.PointF]::new(54, 44),
        [System.Drawing.PointF]::new(32, 35)))
    $g.DrawLine($pen, 32, 11, 32, 35)
}

New-Icon "ConfigIcon" {
    param($g, $pen, $brush)
    foreach ($y in @(16, 32, 48)) {
        $g.DrawLine($pen, 9, $y, 55, $y)
    }
    $g.FillEllipse($brush, 18, 10, 12, 12)
    $g.FillEllipse($brush, 39, 26, 12, 12)
    $g.FillEllipse($brush, 25, 42, 12, 12)
}

Write-Output "Generated content-browser icons in $resolvedOutput"
