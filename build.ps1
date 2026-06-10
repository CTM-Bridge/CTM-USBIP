[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Platform = 'x64',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$WithUsbDisplay
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot

function Find-VisualStudio {
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vsWhere)) { return $null }
    & $vsWhere -latest -products * -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath
}

function Import-VsDevEnvironment([string]$VsInstall, [string]$Arch) {
    $vcvarsall = Join-Path $VsInstall 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path $vcvarsall)) { throw "vcvarsall.bat not found: $vcvarsall" }
    # A stray quote in PATH makes vcvarsall's batch conditionals fail with
    # "was unexpected at this time"; keep the cleanup local to this process.
    $env:Path = $env:Path -replace '"', ''
    $envLines = cmd.exe /c "`"$vcvarsall`" $Arch > nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw 'vcvarsall failed' }
    foreach ($line in $envLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:\$($Matches[1])" -Value $Matches[2]
        }
    }
}

function Find-MSBuild([string]$VsInstall) {
    $candidates = @(
        (Join-Path $VsInstall 'MSBuild\Current\Bin\amd64\MSBuild.exe'),
        (Join-Path $VsInstall 'MSBuild\Current\Bin\MSBuild.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$vs = Find-VisualStudio
if (-not $vs) { throw 'Visual Studio 2022 Native Desktop workload not found.' }
$vs = $vs.Trim()
Import-VsDevEnvironment -VsInstall $vs -Arch $Platform
$msbuild = Find-MSBuild -VsInstall $vs
if (-not $msbuild) { throw 'MSBuild.exe not found.' }

$project = Join-Path $Root 'app\ctm-usbip.vcxproj'
& $msbuild $project /m /p:Configuration=$Configuration /p:Platform=$Platform
if ($LASTEXITCODE -ne 0) { throw 'ctm-usbip build failed.' }

$out = Join-Path $Root "out\$Platform\$Configuration"
New-Item -ItemType Directory -Force -Path (Join-Path $out 'profiles\descriptors') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $out 'maps') | Out-Null
Copy-Item -Force -Path (Join-Path $Root 'profiles\descriptors\ds5_composite.profile') -Destination (Join-Path $out 'profiles\descriptors\ds5_composite.profile')
Copy-Item -Force -Path (Join-Path $Root 'maps\ds5_usb_over_ds5_bt.map') -Destination (Join-Path $out 'maps\ds5_usb_over_ds5_bt.map')
Copy-Item -Force -Path (Join-Path $Root 'profiles\descriptors\ds4_composite.profile') -Destination (Join-Path $out 'profiles\descriptors\ds4_composite.profile')
Copy-Item -Force -Path (Join-Path $Root 'maps\ds4_usb_over_ds4_bt.map') -Destination (Join-Path $out 'maps\ds4_usb_over_ds4_bt.map')
Copy-Item -Force -Path (Join-Path $Root 'profiles\descriptors\steam_puck.profile') -Destination (Join-Path $out 'profiles\descriptors\steam_puck.profile')
Copy-Item -Force -Path (Join-Path $Root 'maps\steam_puck_identity.map') -Destination (Join-Path $out 'maps\steam_puck_identity.map')
Copy-Item -Force -Path (Join-Path $Root 'maps\hid_identity.map') -Destination (Join-Path $out 'maps\hid_identity.map')
Copy-Item -Force -Path (Join-Path $Root 'profiles\descriptors\xbox_gip_usb.profile') -Destination (Join-Path $out 'profiles\descriptors\xbox_gip_usb.profile')
Copy-Item -Force -Path (Join-Path $Root 'maps\xbox_gip_usb_over_xbox_bt.map') -Destination (Join-Path $out 'maps\xbox_gip_usb_over_xbox_bt.map')
Copy-Item -Force -Path (Join-Path $Root 'third_party\ffmpeg\x64\release\bin\*.dll') -Destination $out

Write-Host "Built: $(Join-Path $out 'ctm-usbip.exe')"

if ($WithUsbDisplay) {
    $displayProject = Join-Path $Root 'app\ctm-usbdisplay.vcxproj'
    & $msbuild $displayProject /m /p:Configuration=$Configuration /p:Platform=$Platform
    if ($LASTEXITCODE -ne 0) { throw 'ctm-usbdisplay build failed.' }
    Write-Host "Built: $(Join-Path $out 'ctm-usbdisplay.exe')"
}
