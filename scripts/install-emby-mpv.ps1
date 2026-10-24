#requires -Version 7.0
param(
    [Parameter(Mandatory)][string] $MpvDirectory,
    [Parameter(Mandatory)][string] $PrivateProfile,
    [string] $RuntimeDirectory = '',
    [switch] $BackupOnly
)
. "$PSScriptRoot/common.ps1"
if (-not $RuntimeDirectory) { $RuntimeDirectory = Get-ProjectPath 'out/run/windows-release' }
$MpvDirectory = (Resolve-Path -LiteralPath $MpvDirectory).Path
$RuntimeDirectory = [IO.Path]::GetFullPath($RuntimeDirectory)
if (-not (Test-Path -LiteralPath (Join-Path $MpvDirectory 'mpv.exe'))) { throw 'mpv.exe not found' }
$profileText = Get-Content -LiteralPath $PrivateProfile -Raw
$profile = $profileText | ConvertFrom-Json
if (-not $profile.misaka.url -or -not $profile.misaka.token) { throw 'Missing Misaka profile' }

function Protect-Directory([string] $Path) {
    New-Item -ItemType Directory -Force $Path | Out-Null
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    & icacls.exe $Path /inheritance:r /grant:r "*${sid}:(OI)(CI)F" '*S-1-5-18:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot restrict bridge directory permissions' }
}
function Protect-File([string] $Path) {
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    & icacls.exe $Path /inheritance:r /grant:r "*${sid}:F" '*S-1-5-18:F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot restrict profile permissions' }
}
$backup = Get-ProjectPath ('.local/backups/emby-mpv-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
Protect-Directory $backup
$files = @('mpv-url-handler.py','danmax_bridge.py','danmax-bridge.json')
foreach ($name in $files) {
    $path = Join-Path $MpvDirectory $name
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination (Join-Path $backup $name) }
}
# Preserve the runtime and its configuration; caches and accumulated logs are not binaries.
if (Test-Path -LiteralPath $RuntimeDirectory) {
    $runtimeBackup = Join-Path $backup 'runtime'
    New-Item -ItemType Directory $runtimeBackup | Out-Null
    Get-ChildItem -LiteralPath $RuntimeDirectory -Force | Where-Object { $_.Name -notin @('cache','logs') } |
        Copy-Item -Destination $runtimeBackup -Recurse
}
@{ mpvDirectory=$MpvDirectory; runtimeDirectory=$RuntimeDirectory; savedFiles=@($files | Where-Object { Test-Path -LiteralPath (Join-Path $backup $_) }) } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding utf8NoBOM
Write-Host "Backup: $backup"
if ($BackupOnly) { return }

$sessions = Join-Path $RuntimeDirectory 'cache/external-sessions'
Protect-Directory $sessions
Copy-Item -LiteralPath (Get-ProjectPath 'integrations/emby-mpv/mpv-url-handler.py') -Destination $MpvDirectory -Force
Copy-Item -LiteralPath (Get-ProjectPath 'integrations/emby-mpv/danmax_bridge.py') -Destination $MpvDirectory -Force
$destination = Join-Path $RuntimeDirectory 'external-player.json'
[IO.File]::WriteAllText($destination, $profileText, [Text.UTF8Encoding]::new($false))
Protect-File $destination
@{
    enabled = $true
    executable = Join-Path $RuntimeDirectory 'DanmaX.exe'
    dataDirectory = $RuntimeDirectory
    cacheDirectory = Join-Path $RuntimeDirectory 'cache'
    sessionDirectory = $sessions
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $MpvDirectory 'danmax-bridge.json') -Encoding utf8NoBOM
Write-Host 'Emby mpv bridge enabled. Existing URL protocol registration is unchanged.'
