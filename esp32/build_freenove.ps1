# Build, flash or monitor the Muse gadget firmware for the Freenove ESP32-S3 Display 2.8" on Windows.
# The repo's helpers are bash/macOS; this does the same idf.py call from PowerShell.
#
#   .\build_freenove.ps1 build
#   .\build_freenove.ps1 flash COM7
#   .\build_freenove.ps1 monitor COM7
#   .\build_freenove.ps1 flash-monitor COM7
#
# The SDK token lives in sdkconfig.token (git-ignored):  CONFIG_GADGET_SDK_TOKEN="mgst_..."
param(
    [ValidateSet("build", "reconfigure", "flash", "monitor", "flash-monitor", "menuconfig", "erase-flash", "size")]
    [string]$Action = "build",
    [string]$Port = "",
    [ValidateSet("28", "35")]
    [string]$Board = "35"
)
$ErrorActionPreference = "Continue"
$idf = "C:\esp\esp-idf-v6.0.1"
$env:Path = "C:\Users\mat_n\AppData\Local\Programs\Python\Python312;C:\Users\mat_n\AppData\Local\Programs\Python\Python312\Scripts;" + $env:Path
$env:IDF_PATH = $idf
. "$idf\export.ps1" | Out-Null

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$build = "build-muse-freenove-s3-$Board"
$defaults = "sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-freenove-s3-$Board"
if (Test-Path "$here\sdkconfig.token") { $defaults += ";sdkconfig.token" }

$common = @("-B", $build, "-DIDF_TARGET=esp32s3", "-DSDKCONFIG=$build/sdkconfig", "-DSDKCONFIG_DEFAULTS=$defaults")
$portArgs = @()
if ($Port) { $portArgs = @("-p", $Port) }
switch ($Action) {
    "build"         { idf.py @common build }
    "reconfigure"   { idf.py @common reconfigure }   # after a Kconfig edit: ninja alone does not re-read it
    "size"          { idf.py @common size }
    "menuconfig"    { idf.py @common menuconfig }
    "erase-flash"   { idf.py @common @portArgs erase-flash }
    "flash"         { idf.py @common @portArgs -b 460800 flash }
    "monitor"       { idf.py @common @portArgs monitor }
    "flash-monitor" { idf.py @common @portArgs -b 460800 flash monitor }
}
