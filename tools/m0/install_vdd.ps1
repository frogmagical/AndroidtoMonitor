$ErrorActionPreference = 'Continue'
$log = 'C:\Users\daiki\tools\vdd\install_log.txt'
Start-Transcript -Path $log -Force

# 1. Settings file (driver reads C:\VirtualDisplayDriver\vdd_settings.xml)
New-Item -ItemType Directory -Force 'C:\VirtualDisplayDriver' | Out-Null
Copy-Item 'C:\Users\daiki\tools\vdd\vdd_settings_custom.xml' 'C:\VirtualDisplayDriver\vdd_settings.xml' -Force
Write-Output 'Settings copied.'

# 2. Install driver and create root-enumerated device node
& 'C:\Users\daiki\tools\vdd\Dependencies\devcon.exe' install 'C:\Users\daiki\tools\vdd\SignedDrivers\x86\VDD\MttVDD.inf' 'Root\MttVDD'
Write-Output "devcon exit code: $LASTEXITCODE"

# 3. Verify
Start-Sleep -Seconds 3
Get-PnpDevice | Where-Object { $_.FriendlyName -match 'Virtual Display' } | Select-Object Status, Class, FriendlyName, InstanceId | Format-List

Stop-Transcript
