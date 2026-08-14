# Sharper high-motion test: full per-pixel random noise every frame (worst-case incompressible
# content for an H.264 encoder), refreshed at ~30Hz to match the sender's capture rate.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$vs = [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.DeviceName -eq '\\.\DISPLAY10' }
if (-not $vs) { exit 1 }

$w = $vs.Bounds.Width
$h = $vs.Bounds.Height

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point($vs.Bounds.X, $vs.Bounds.Y)
$form.Size = New-Object System.Drawing.Size($w, $h)
$form.BackColor = [System.Drawing.Color]::Black
$form.TopMost = $true
$form.DoubleBuffered = $true

$rand = New-Object System.Random
$stride = $w * 4
$bytes = New-Object byte[] ($stride * $h)
$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)

$form.Add_Paint({
    param($s, $e)
    $rand.NextBytes($bytes)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
    [System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $data.Scan0, $bytes.Length)
    $bmp.UnlockBits($data)
    $e.Graphics.DrawImageUnscaled($bmp, 0, 0)
})

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 33
$timer.Add_Tick({ $form.Invalidate() })
$timer.Start()

[void]$form.ShowDialog()
