<#
  Tür per Desktop-Symbol öffnen (Windows).

  Einrichten - legt "Tür öffnen" auf dem Desktop an:
    powershell -ExecutionPolicy Bypass -File tuer-oeffnen.ps1 -Install -Address 192.168.20.50

  Ein Doppelklick sendet POST /open an den Türöffner. Ist ein Tür-Passwort gesetzt,
  fragt das Skript beim ersten Mal nach Benutzer und Passwort und speichert sie
  verschlüsselt (Windows-DPAPI: lesbar nur für diesen Windows-Benutzer auf diesem PC).
  Am besten einen eigenen Benutzer anlegen (Weboberfläche: Tür-Zugang oder weitere
  Benutzer), nie den Admin. -Reset vergisst die gespeicherte Anmeldung.
#>
param(
  [string]$Address = 'tueroeffner.local',
  [switch]$Install,
  [switch]$Reset
)

$ErrorActionPreference = 'Stop'
$dir       = Join-Path $env:APPDATA 'Tueroeffner'
$credFile  = Join-Path $dir ('zugang-' + ($Address -replace '[^\w\.-]', '_') + '.xml')
$shell     = New-Object -ComObject WScript.Shell
$lastError = ''

# Kurze Rückmeldung (schließt sich selbst); icon 64 = Info, 16 = Fehler
function Show-Message([string]$text, [int]$icon, [int]$seconds) {
  [void]$shell.Popup($text, $seconds, 'Türöffner', $icon)
}

if ($Install) {
  # Skript an festen Ort kopieren, damit das Symbol nicht vom Download-Ordner abhängt
  $target = Join-Path $env:LOCALAPPDATA 'Tueroeffner\tuer-oeffnen.ps1'
  New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
  if ($PSCommandPath -ne $target) { Copy-Item $PSCommandPath $target -Force }
  $desktop = [Environment]::GetFolderPath('Desktop')
  $lnk = $shell.CreateShortcut((Join-Path $desktop 'Tür öffnen.lnk'))
  $lnk.TargetPath   = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
  $lnk.Arguments    = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$target`" -Address $Address"
  $lnk.IconLocation = (Join-Path $env:SystemRoot 'System32\shell32.dll') + ',47'   # Schloss
  $lnk.Description  = "Tür öffnen ($Address)"
  $lnk.Save()
  Write-Host "Symbol 'Tür öffnen' auf dem Desktop angelegt ($Address)."
  return
}

if ($Reset) {
  Remove-Item $credFile -ErrorAction SilentlyContinue
  Show-Message 'Gespeicherte Anmeldung gelöscht.' 64 3
  return
}

# POST /open; liefert den HTTP-Status (0 = nicht erreichbar), Fehlertext in $lastError
function Open-Door($cred) {
  $headers = @{}
  if ($cred) {
    $pair = $cred.UserName + ':' + $cred.GetNetworkCredential().Password
    $headers['Authorization'] = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pair))
  }
  try {
    Invoke-RestMethod -Method Post -Uri "http://$Address/open" -Headers $headers -TimeoutSec 5 | Out-Null
    return 200
  } catch {
    $code = 0
    if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
    $msg = $_.ErrorDetails.Message
    try { $msg = ($msg | ConvertFrom-Json).err } catch {}
    if (-not $msg) { $msg = $_.Exception.Message }
    $script:lastError = $msg
    return $code
  }
}

$cred = $null
if (Test-Path $credFile) {
  try { $cred = Import-Clixml $credFile } catch { Remove-Item $credFile -ErrorAction SilentlyContinue }
}
$code = Open-Door $cred
if ($code -eq 401) {
  # Tür-Passwort gesetzt (oder gespeicherte Anmeldung falsch): nachfragen und merken
  try { $cred = Get-Credential -Message "Anmeldung für den Türöffner ($Address)" } catch { $cred = $null }
  if (-not $cred) { return }
  $code = Open-Door $cred
  if ($code -eq 200) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $cred | Export-Clixml $credFile
  }
}

if ($code -eq 200)    { Show-Message 'Tür wird geöffnet.' 64 2 }
elseif ($code -eq 0)  { Show-Message "Türöffner nicht erreichbar ($Address).`n$lastError" 16 15 }
else                  { Show-Message "Nicht geöffnet: $lastError" 16 15 }
