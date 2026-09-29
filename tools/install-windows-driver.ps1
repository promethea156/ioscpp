# Builds and runs `ioscpp_windows_driver`, which binds a libusb-compatible
# driver to the attached device without Zadig.
#
# The step itself is in the library (`src/usb/windows_driver.cpp`); this wrapper
# only builds the tool and elevates, because installing a kernel driver needs an
# elevated process. See `docs/14-windows-driver-automation.md`.
#
# Usage:
#   tools/install-windows-driver.ps1              # bind the attached device
#   tools/install-windows-driver.ps1 -List           # report, change nothing
#   tools/install-windows-driver.ps1 -Force             # rebind even if already libusb0
#   tools/install-windows-driver.ps1 -Package C:\libusb-win32
#   tools/install-windows-driver.ps1 -Uninstall

param(
    [string]$Configuration = "Release",
    [string]$Package,
    [switch]$List,
    [switch]$Force,
    [switch]$Uninstall,
    [switch]$NoElevate,
    [string]$Log
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin) -and -not $NoElevate) {
    Write-Host 'Not elevated; re-launching with RunAs...'
    $exe = (Get-Process -Id $PID).Path
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
        '-Configuration', "`"$Configuration`"")
    if ($Package) { $arguments += @('-Package', "`"$Package`"") }
    if ($List) { $arguments += '-List' }
    if ($Force) { $arguments += '-Force' }
    if ($Uninstall) { $arguments += '-Uninstall' }
    $log = Join-Path $env:TEMP "ioscpp-driver-$([guid]::NewGuid().ToString('N')).log"
    $arguments += @('-Log', "`"$log`"")
    # The elevated child runs in its own window, so it transcribes to a file the
    # parent prints once it exits.
    $child = Start-Process -FilePath $exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru -WhatIf:$false
    if (Test-Path -LiteralPath $log) {
        Get-Content -Raw -LiteralPath $log
        Remove-Item -LiteralPath $log
    }
    exit $child.ExitCode
}

if ($Log) {
    try { Start-Transcript -Path $Log -Force -WhatIf:$false | Out-Null } catch { }
}

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'

cmake -S $root -B $build -DIOSCPP_BUILD_TOOLS=ON | Out-Null
cmake --build $build --config $Configuration --target ioscpp_windows_driver

$tool = Join-Path $build "tools/$Configuration/ioscpp_windows_driver.exe"
if (-not (Test-Path -LiteralPath $tool)) {
    $tool = Join-Path $build 'tools/ioscpp_windows_driver'
}

$arguments = @()
if ($Package) { $arguments += @('--package', $Package) }
if ($List) { $arguments += '--list' }
if ($Force) { $arguments += '--force' }
if ($Uninstall) { $arguments += '--uninstall' }

& $tool @arguments
exit $LASTEXITCODE
