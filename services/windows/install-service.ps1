# Installs jarvisd as a user logon task or Windows service
param(
    [string]$Prefix = "$HOME\.local",
    [switch]$AsService
)

$JarvisBin = Join-Path $Prefix "bin\jarvisd.exe"
if (-not (Test-Path $JarvisBin)) {
    $JarvisBin = (Get-Command jarvisd.exe -ErrorAction SilentlyContinue)?.Source
}

if ($AsService) {
    Write-Host "Registering jarvisd Windows Service..."
    New-Service -Name "jarvisd" -BinaryPathName "`"$JarvisBin`" -j" -DisplayName "Jarvis Voice Assistant Daemon" -StartupType Automatic
    Start-Service -Name "jarvisd"
} else {
    Write-Host "Registering jarvisd Scheduled Task at logon..."
    $XmlTemplate = Get-Content (Join-Path $PSScriptRoot "jarvisd.xml") -Raw
    $XmlContent = $XmlTemplate.Replace("@PREFIX@", $Prefix)
    $XmlPath = [System.IO.Path]::GetTempFileName() + ".xml"
    [System.IO.File]::WriteAllText($XmlPath, $XmlContent)
    schtasks.exe /Create /TN "Jarvis\jarvisd" /XML $XmlPath /F
    Remove-Item $XmlPath
    schtasks.exe /Run /TN "Jarvis\jarvisd"
}
Write-Host "jarvisd service registered successfully."
