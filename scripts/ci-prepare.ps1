#requires -Version 7.0
. "$PSScriptRoot/common.ps1"
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_OS -ne 'Windows') {
    throw 'This script only declares the toolchain on a GitHub-hosted Windows runner.'
}
$vs = 'C:/Program Files/Microsoft Visual Studio/2022/Enterprise'
$compiler = Join-Path $vs "VC/Tools/MSVC/$($DependencyLock.msvcToolsetVersion)/bin/Hostx64/x64/cl.exe"
$sdk = Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10/Include/$($DependencyLock.windowsSdkVersion)/um/Windows.h"
if (-not (Test-Path -LiteralPath $compiler) -or -not (Test-Path -LiteralPath $sdk)) {
    throw 'The hosted image no longer has the locked MSVC/SDK. Update the explicit toolchain lock; do not silently substitute a compiler.'
}
New-Item -ItemType Directory -Force (Get-ProjectPath '.local') | Out-Null
@{ allowExternalSystemToolchain = $true; visualStudioPath = $vs } |
    ConvertTo-Json | Set-Content -LiteralPath (Get-ProjectPath '.local/toolchain.local.json') -Encoding utf8
Write-Host "Declared CI system toolchain: $vs, MSVC $($DependencyLock.msvcToolsetVersion), SDK $($DependencyLock.windowsSdkVersion)"
