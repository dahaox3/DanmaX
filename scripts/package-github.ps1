#requires -Version 7.0
param([Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')][string] $ReleaseId)
. "$PSScriptRoot/common.ps1"

$output = Get-ProjectPath 'out/github-release'
if (Test-Path -LiteralPath $output) { throw 'Use a fresh checkout/output directory for each release.' }
$stages = @(& "$PSScriptRoot/package.ps1" -Preset windows-release -Format directory)
if ($stages.Count -ne 1 -or -not (Test-Path -LiteralPath (Join-Path $stages[0] 'DanmaX.exe'))) {
    throw 'Packaging did not return exactly one clean deployment.'
}
$stage = [string]$stages[0]
$publicStage = Get-ProjectPath "out/stage/DanmaX-$ReleaseId-windows-x64"
$stageRoot = (Get-ProjectPath 'out/stage') + [IO.Path]::DirectorySeparatorChar
if (-not $stage.StartsWith($stageRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not $publicStage.StartsWith($stageRoot, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $publicStage)) { throw 'Release stage paths are not fresh project staging directories.' }
Move-Item -LiteralPath $stage -Destination $publicStage
$stage = $publicStage
$materials = Get-ProjectPath ('out/stage/materials-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $output,$materials | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'licenses') | Out-Null

# Reuse the exact SDK source archives and SHA-256 values already locked for Qt.
# Network preparation is explicit here; CMake configure/build never downloads.
$qtSources = Get-Content (Get-ProjectPath 'toolchain/qt-static.lock.json') -Raw | ConvertFrom-Json
$sourceCache = Get-ProjectPath '.cache/downloads'
$tar = Join-Path $env:SystemRoot 'System32/tar.exe'
foreach ($asset in $qtSources.assets | Where-Object { $_.module -ne 'qt5compat' }) {
    $archive = Join-Path $sourceCache $asset.name
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri $asset.url -OutFile "$archive.part" -TimeoutSec 300
        if ((Get-FileHash -LiteralPath "$archive.part").Hash.ToLowerInvariant() -ne $asset.sha256) {
            throw "Source checksum mismatch: $($asset.name)"
        }
        Move-Item -LiteralPath "$archive.part" -Destination $archive
    }
    if ((Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant() -ne $asset.sha256) {
        throw "Source checksum mismatch: $($asset.name)"
    }
    Copy-Item -LiteralPath $archive -Destination $materials
    $licenseStage = Join-Path $materials "$($asset.module)-notices"
    New-Item -ItemType Directory -Force $licenseStage | Out-Null
    $prefix = $asset.name -replace '\.tar\.xz$',''
    Invoke-Checked $tar @('-xf', $archive, '-C', $licenseStage, '--strip-components', '1', "$prefix/LICENSES")
    Copy-Item -LiteralPath $licenseStage -Destination (Join-Path $stage 'licenses') -Recurse -Force
}

# Preserve all tracked QWindowKit/submodule sources, never their .git metadata.
$qwk = Get-ProjectPath '.deps/qwindowkit'
$qwkFiles = & git -C $qwk ls-files --recurse-submodules
if ($LASTEXITCODE -ne 0) { throw 'Cannot inventory locked QWindowKit sources.' }
foreach ($relative in $qwkFiles) {
    $source = [IO.Path]::GetFullPath((Join-Path $qwk $relative))
    if (-not $source.StartsWith($qwk + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Dependency source path escaped its root.'
    }
    $destination = Join-Path $materials "qwindowkit/$relative"
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
}
foreach ($relative in @('LICENSE','qmsetup/LICENSE','qmsetup/src/stdcorelib/LICENSE')) {
    $source = Join-Path $qwk $relative
    if (-not (Test-Path -LiteralPath $source)) { throw "Missing dependency notice: $relative" }
    $destination = Join-Path $stage "licenses/qwindowkit/$relative"
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $stage 'anitomy') -Destination $materials -Recurse
Copy-Item -LiteralPath (Get-ProjectPath 'toolchain') -Destination $materials -Recurse
Invoke-Checked git @('archive', '--format=zip', "--output=$(Join-Path $materials 'application-source.zip')", 'HEAD')

# Copy only the public integration sources; personal profiles are never inputs.
foreach ($relative in @((& git ls-files -- integrations/emby-mpv)) + @('docs/EMBY_MPV.md', 'docs/LEGACY_MPV.md',
    'docs/FRAME_PACING.md', 'docs/USER_GUIDE.md', 'scripts/common.ps1',
    'scripts/install-emby-mpv.ps1', 'toolchain/dependencies.lock.json')) {
    $destination = Join-Path $stage $relative
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath (Get-ProjectPath $relative) -Destination $destination
}

# App-local official CRT files let a clean Windows machine run the ZIP without
# depending on the developer's already-installed VC++ redistributable.
Invoke-ProjectEnvironment {
    $tools = Initialize-Toolchain
    $crt = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
    if (-not (Test-Path -LiteralPath (Join-Path $crt 'vcruntime140.dll'))) { throw 'Official x64 CRT redistributables are missing.' }
    Get-ChildItem -LiteralPath $crt -Filter '*.dll' | Copy-Item -Destination $stage
}

$commit = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source commit.' }
$info = [ordered]@{
    release = $ReleaseId; commit = $commit; architecture = 'windows-x64'; format = 'dynamic-directory'
    fileVersion = (Get-Item (Join-Path $stage 'DanmaX.exe')).VersionInfo.FileVersion
    qt = $DependencyLock.qtVersion; msvc = $DependencyLock.msvcToolsetVersion; sdk = $DependencyLock.windowsSdkVersion
    executableSha256 = (Get-FileHash (Join-Path $stage 'DanmaX.exe')).Hash.ToLowerInvariant()
    githubRun = if ($env:GITHUB_RUN_ID) { "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID" } else { $null }
}
$info | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'build-info.json') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $output 'build-info.json') -Destination $stage
@'
DanmaX Windows x64 · legacy-mpv

解压整个目录，运行 DanmaX.exe。请保留同目录 DLL 和 qml/plugins 文件。
本包保留 mpv 会话选择，包含 Emby 自动外挂、小数字号取整、类内避让、限帧及帧节奏修复。
首次运行创建个人设置；包内没有服务器 Token、历史设置、日志或缓存。

Emby 接入需要现有 mpv、Python 和已注册的 mpv URL 协议。见 docs/EMBY_MPV.md。
从本目录启动 PowerShell 7，运行安装器时明确指定解压目录，例如：
  ./scripts/install-emby-mpv.ps1 -MpvDirectory '你的 mpv 目录' -PrivateProfile '你的私密配置.json' -RuntimeDirectory (Get-Location).Path
安装器会备份现有脚本。请勿把私密配置上传到 GitHub。

licenses/ 为第三方许可；官方 VC++ x64 CRT 按 Visual Studio 分发许可随包提供。
Qt DLL 可以替换；匹配的依赖源码、应用源码与锁定清单见同一 Release 的 sources.zip。
仅自动化启动/回归检查运行于云端；真实显卡压力、穿透和跨屏行为仍以本机验收为准。
'@ | Set-Content -LiteralPath (Join-Path $stage '开始使用.txt') -Encoding utf8

$forbidden = @(Get-ChildItem -LiteralPath $stage -Recurse -Force | Where-Object {
    $_.Name -in @('settings.ini','external-player.json','external-associations.json','danmax-bridge.json','.git','cache','logs','__pycache__') -or
    $_.Extension -in @('.log','.pdb','.pyc')
})
if ($forbidden.Count) { throw 'Release staging contains user data or development artifacts.' }

# Validate with the deployed Qt/QML paths and only system directories on PATH.
Invoke-ProjectEnvironment {
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $env:QT_QPA_PLATFORM = 'offscreen'; $env:QT_QUICK_BACKEND = 'software'
    $validation = Get-ProjectPath 'out/validation/github-release'
    New-Item -ItemType Directory -Force $validation | Out-Null
    Invoke-Checked (Join-Path $stage 'DanmaX.exe') @('--smoke-test', '--data-dir', (Join-Path $validation 'data'),
        '--cache-dir', (Join-Path $validation 'cache'), '--report', (Join-Path $validation 'smoke.json'))
}
$name = "DanmaX-$ReleaseId-windows-x64"
Compress-Archive -LiteralPath $stage -DestinationPath (Join-Path $output "$name.zip") -CompressionLevel Optimal
Compress-Archive -Path (Join-Path $materials '*') -DestinationPath (Join-Path $output "$name-sources.zip") -CompressionLevel Optimal
Get-ChildItem -LiteralPath $output -Filter '*.zip' | ForEach-Object {
    "$((Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant())  $($_.Name)"
} | Set-Content -LiteralPath (Join-Path $output 'checksums.txt') -Encoding ascii
@"
支持 mpv 的 Windows x64 免安装包，由 GitHub Actions 从提交 $commit 编译。

- 保留旧版 mpv 选择与 Emby 点击 MPV 后自动外挂弹幕、关闭清理。
- 保留小数字号四舍五入、滚动/固定分类避让与帧率上限。
- 修复覆盖层重复等待垂直同步导致的帧间隔抖动，增加呈现 P99/1% low 诊断。
- 运行包包含 Qt/QML、官方 VC++ x64 运行库、许可及通用 Emby 接入脚本。

下载 **$name.zip**，完整解压后运行 DanmaX.exe；不要单独移动 EXE。
**$name-sources.zip** 为对应依赖源码、应用源码及重建材料。
Emby 服务器和 Token 需要自行在本机配置，个人配置不会随包发布。

自动化检查包含 20 项相关 CTest 与部署目录启动检查。云端不验证真实显卡压力或光学帧输出。
SHA-256 见 checksums.txt，编译来源与工具版本见 build-info.json。
"@ | Set-Content -LiteralPath (Join-Path $output 'release-notes.md') -Encoding utf8
Write-Host "GitHub release assets ready: $output"
