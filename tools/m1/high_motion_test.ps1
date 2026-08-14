# Temporary high-motion test pattern for M2 bitrate-behavior measurement (tools/m1 M2 addendum).
# Fills the virtual display with full-screen randomized noise blocks every frame (~60Hz timer),
# approximating worst-case desktop-usage motion (window drag / video playback) for CBR bitrate
# testing. Not part of the shipped product -- delete/ignore after use.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$vs = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.DeviceName -eq '\\.\DISPLAY10' }
if (-not $vs) { exit 1 }

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point($vs.Bounds.X, $vs.Bounds.Y)
$form.Size = New-Object System.Drawing.Size($vs.Bounds.Width, $vs.Bounds.Height)
$form.BackColor = [System.Drawing.Color]::Black
$form.TopMost = $true
$form.DoubleBuffered = $true

$rand = New-Object System.Random
$blockSize = 40

$form.Add_Paint({
    param($s, $e)
    $g = $e.Graphics
    for ($y = 0; $y -lt $form.ClientSize.Height; $y += $blockSize) {
        for ($x = 0; $x -lt $form.ClientSize.Width; $x += $blockSize) {
            $c = [System.Drawing.Color]::FromArgb($rand.Next(256), $rand.Next(256), $rand.Next(256))
            $brush = New-Object System.Drawing.SolidBrush($c)
            $g.FillRectangle($brush, $x, $y, $blockSize, $blockSize)
            $brush.Dispose()
        }
    }
})

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 16
$timer.Add_Tick({ $form.Invalidate() })
$timer.Start()

[void]$form.ShowDialog()
