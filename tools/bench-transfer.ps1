# Times a push and a pull of a file to and from a device.
#
# Usage: tools/bench-transfer.ps1 [-SizeMb 16] [-Configuration Release]
#
# Generates a file of the given size, then runs `ioscpp_transfer_example`
# against the first attached device. Stop `usbmuxd` first.

param(
    [int]$SizeMb = 16,
    [string]$Configuration = "Release"
)

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

cmake -S $root -B $build -DIOSCPP_BUILD_EXAMPLES=ON | Out-Null
cmake --build $build --config $Configuration --target ioscpp_transfer_example

$local = Join-Path $env:TEMP "ioscpp_bench.bin"
$file = [System.IO.File]::Create($local)
$file.SetLength($SizeMb * 1MB)
$file.Close()

$example = Join-Path $build "examples/$Configuration/ioscpp_transfer_example.exe"
if (-not (Test-Path -LiteralPath $example)) {
    $example = Join-Path $build "examples/ioscpp_transfer_example"
}
& $example $local
