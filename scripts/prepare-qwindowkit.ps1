#requires -Version 7.0
param([switch]$Offline, [switch]$FromGit)
. "$PSScriptRoot/common.ps1"
Invoke-ProjectEnvironment {
    $lock = Get-Content (Get-ProjectPath 'toolchain/qwindowkit.lock.json') -Raw | ConvertFrom-Json
    $cache = Get-ProjectPath '.cache/qwindowkit'
    $deps = Get-ProjectPath '.deps/qwindowkit'
    New-Item -ItemType Directory -Force $cache,$deps | Out-Null
    if ($FromGit) {
        if ($Offline) { throw '-FromGit and -Offline cannot be combined.' }
        # The original cache-only ZIP is not publicly downloadable. CI uses the
        # same locked commit and the submodule gitlinks contained in that commit.
        if (-not (Test-Path -LiteralPath (Join-Path $deps '.git'))) {
            Invoke-Checked git @('clone', '--no-checkout', $lock.repository, $deps)
        }
        Invoke-Checked git @('-C', $deps, 'checkout', '--detach', $lock.commit)
        Invoke-Checked git @('-C', $deps, 'submodule', 'update', '--init', '--recursive')
        $actual = (& git -C $deps rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0 -or $actual -ne $lock.commit) { throw 'QWindowKit commit differs from lock.' }
        $submodules = & git -C $deps submodule status --recursive
        if ($LASTEXITCODE -ne 0 -or @($submodules | Where-Object { $_ -match '^[-+U]' }).Count) {
            throw 'QWindowKit submodules differ from the locked gitlinks.'
        }
        Write-Host "Prepared QWindowKit commit $actual with pinned submodules."
        return
    }
    $file = Join-Path $cache $lock.archive
    if (!(Test-Path -LiteralPath $file)) {
        if ($Offline) { throw "Missing cached archive: $file" }
        Write-Host "Locked QWindowKit archive missing in cache."
    }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $lock.sha256) {
        throw "SHA-256 mismatch: $file"
    }
    Expand-Archive -LiteralPath $file -DestinationPath $deps -Force
    Write-Host 'Locked QWindowKit source prepared. Configure/build do not download.'
}
