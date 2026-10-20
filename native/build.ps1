param([string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Visual C++ x64 build tools required' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $env:LOCALAPPDATA 'MyWallpaper/BuildCache/audio-system-release' }
& $cmake -S $projectRoot -B $BuildDirectory -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& $cmake --build $BuildDirectory --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Windows compilation failed' }
& $ctest --test-dir $BuildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Component tests failed' }
$outputRoot = Join-Path $projectRoot 'native/out/windows-x86_64'
New-Item -ItemType Directory -Path (Join-Path $outputRoot 'notices') -Force | Out-Null
Copy-Item (Join-Path $BuildDirectory 'Release/companion.exe') $outputRoot
Copy-Item (Join-Path $projectRoot 'LICENSE') $outputRoot
Copy-Item (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') $outputRoot
Copy-Item (Join-Path $projectRoot 'third_party/nlohmann/LICENSE.MIT') (Join-Path $outputRoot 'notices/nlohmann-json.LICENSE.MIT')
