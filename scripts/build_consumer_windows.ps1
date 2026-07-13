param(
    [string]$OrcaRoot = "C:\code\2_build_OrcaSlicer",
    [string]$ProjectDir = (Resolve-Path (Join-Path $PSScriptRoot "..")),
    [string]$SdkDir = "",
    [string]$BuildDir = "",
    [string]$PackageDir = ""
)

$ProjectDir = (Resolve-Path $ProjectDir).Path
if (-not $SdkDir) { $SdkDir = Join-Path $ProjectDir "slic3r_sdk_consumer" }
if (-not $BuildDir) { $BuildDir = Join-Path $ProjectDir "build_consumer_fresh" }
if (-not $PackageDir) { $PackageDir = Join-Path $ProjectDir "package_consumer_windows" }

$ErrorActionPreference = "Stop"

function Require-Path([string]$Path, [string]$Description) {
    if (-not (Test-Path $Path)) {
        throw "$Description not found: $Path"
    }
}

$Slic3rDll = Join-Path $ProjectDir "build_dll\Release\slic3r.dll"
$Slic3rLib = Join-Path $ProjectDir "build_dll\Release\slic3r.lib"
$DepsBin = Join-Path $OrcaRoot "deps\build\OrcaSlicer_dep\usr\local\bin"
$OcctBin = Join-Path $DepsBin "occt"
$Resources = Join-Path $OrcaRoot "resources"

Require-Path $Slic3rDll "slic3r.dll"
Require-Path $Slic3rLib "slic3r.lib"
Require-Path $DepsBin "OrcaSlicer dependency bin directory"
Require-Path $OcctBin "OCCT bin directory"
Require-Path $Resources "OrcaSlicer resources directory"

Write-Host "=== [1/4] Assemble SDK ==="
Remove-Item $SdkDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $SdkDir "bin"), (Join-Path $SdkDir "lib") | Out-Null
Copy-Item $Slic3rDll (Join-Path $SdkDir "bin") -Force
Copy-Item $Slic3rLib (Join-Path $SdkDir "lib") -Force
Copy-Item (Join-Path $DepsBin "*.dll") (Join-Path $SdkDir "bin") -Force
Copy-Item (Join-Path $OcctBin "TK*.dll") (Join-Path $SdkDir "bin") -Force
Copy-Item $Resources (Join-Path $SdkDir "resources") -Recurse -Force
$SdkDllCount = (Get-ChildItem (Join-Path $SdkDir "bin") -Filter *.dll).Count
Write-Host "SDK: $SdkDir ($SdkDllCount DLLs)"

Write-Host "=== [2/4] Configure consumer ==="
Remove-Item $BuildDir -Recurse -Force -ErrorAction SilentlyContinue
$SdkDirCmake = $SdkDir.Replace("\", "/")
$Slic3rLibCmake = (Join-Path $SdkDir "lib\slic3r.lib").Replace("\", "/")
& cmake -S $ProjectDir -B $BuildDir -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=$SdkDirCmake" "-DSLIC3R_LIBRARY:FILEPATH=$Slic3rLibCmake" -DBUILD_SLIC3R_DLL=OFF
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

Write-Host "=== [3/4] Build consumer ==="
& cmake --build $BuildDir --config Release --target orca-slice-engine -- /m:2
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }

Write-Host "=== [4/4] Package runtime ==="
Remove-Item $PackageDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $PackageDir "bin") | Out-Null
Copy-Item (Join-Path $BuildDir "Release\orca-slice-engine.exe") (Join-Path $PackageDir "bin") -Force
Copy-Item (Join-Path $SdkDir "bin\*.dll") (Join-Path $PackageDir "bin") -Force
Copy-Item (Join-Path $SdkDir "resources") (Join-Path $PackageDir "resources") -Recurse -Force

$RunScript = @"
`$ScriptDir = Split-Path -Parent `$MyInvocation.MyCommand.Path
& "`$ScriptDir\bin\orca-slice-engine.exe" -r "`$ScriptDir\resources" @args
exit `$LASTEXITCODE
"@
Set-Content -Path (Join-Path $PackageDir "run.ps1") -Value $RunScript -Encoding UTF8

Write-Host "Package: $PackageDir"
Write-Host "Run: powershell -ExecutionPolicy Bypass -File `"$PackageDir\run.ps1`" <input.3mf>"
