# Lists the attached iOS devices with their USB serials.
#
# Builds the example first, so the list reflects the current sources. Stop
# `usbmuxd` before running it, because it claims the same USB interface.

param(
    [string]$Configuration = "Release"
)

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

cmake -S $root -B $build -DIOSCPP_BUILD_EXAMPLES=ON | Out-Null
cmake --build $build --config $Configuration --target ioscpp_usb_example

$example = Join-Path $build "examples/$Configuration/ioscpp_usb_example.exe"
if (-not (Test-Path -LiteralPath $example)) {
    $example = Join-Path $build "examples/ioscpp_usb_example"
}
& $example
