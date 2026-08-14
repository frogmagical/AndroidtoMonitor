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

$label = New-Object System.Windows.Forms.Label
$label.Font = New-Object System.Drawing.Font('Consolas', 40, [System.Drawing.FontStyle]::Bold)
$label.ForeColor = [System.Drawing.Color]::Lime
$label.AutoSize = $true
$label.Location = New-Object System.Drawing.Point(40, 200)
$form.Controls.Add($label)

# bouncing box for motion/judder check
$box = New-Object System.Windows.Forms.Panel
$box.Size = New-Object System.Drawing.Size(120, 120)
$box.BackColor = [System.Drawing.Color]::Red
$box.Location = New-Object System.Drawing.Point(0, 500)
$form.Controls.Add($box)
$script:dx = 8

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 10
$timer.Add_Tick({
    $label.Text = (Get-Date).ToString('HH:mm:ss.fff')
    $nx = $box.Location.X + $script:dx
    if ($nx -lt 0 -or $nx -gt ($form.ClientSize.Width - $box.Width)) { $script:dx = -$script:dx; $nx = $box.Location.X + $script:dx }
    $box.Location = New-Object System.Drawing.Point($nx, 500)
})
$timer.Start()

[void]$form.ShowDialog()
