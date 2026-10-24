[CmdletBinding()]
param([switch]$FetchAsioSdk, [switch]$BuildInstaller)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
if (!(Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'Install CMake 3.21+.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'Install Visual Studio 2022 with Desktop development with C++.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC x86/x64 tools were not found.' }
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (!(Test-Path $csc)) { throw "C# compiler not found: $csc" }
if ($FetchAsioSdk) { & (Join-Path $root 'tools\Fetch-ASIO-SDK.ps1') }
New-Item -ItemType Directory -Force (Join-Path $root 'dist'), (Join-Path $root 'dist\x64'), (Join-Path $root 'dist\x86') | Out-Null
$framework = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$guiOut = Join-Path $root 'dist\GameAudioBridge.exe'
& $csc /nologo /target:winexe /platform:anycpu "/out:$guiOut" "/reference:$(Join-Path $framework 'System.Windows.Forms.dll')" "/reference:$(Join-Path $framework 'System.Drawing.dll')" (Join-Path $root 'src\GameAudioBridgeGui.cs')
if ($LASTEXITCODE -ne 0) { throw 'GUI compilation failed.' }
foreach ($arch in @(@{Name='x64'; CMakeArch='x64'}, @{Name='x86'; CMakeArch='Win32'})) {
  $buildDir = Join-Path $root ('build-' + $arch.Name)
  $cmakeArgs = @('-S', $root, '-B', $buildDir, '-A', $arch.CMakeArch)
  if (Test-Path (Join-Path $root 'third_party\asio-sdk\common\asio.h')) { $cmakeArgs += @('-DGAB_ASIO_SDK_ROOT=' + (Join-Path $root 'third_party\asio-sdk')) }
  & cmake @cmakeArgs
  if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $($arch.Name)." }
  & cmake --build $buildDir --config Release --parallel
  if ($LASTEXITCODE -ne 0) { throw "Native build failed for $($arch.Name)." }
  $binDir = Join-Path $buildDir 'Release'
  Copy-Item (Join-Path $binDir 'GameAudioBridgeHook.dll') (Join-Path $root ('dist\' + $arch.Name + '\GameAudioBridgeHook.dll')) -Force
  Copy-Item (Join-Path $binDir 'GameAudioBridgeLauncher.exe') (Join-Path $root ('dist\' + $arch.Name + '\GameAudioBridgeLauncher.exe')) -Force
  if ($arch.Name -eq 'x64' -and (Test-Path (Join-Path $binDir 'GameAudioBridgeAsio.dll'))) { Copy-Item (Join-Path $binDir 'GameAudioBridgeAsio.dll') (Join-Path $root 'dist\GameAudioBridgeAsio.dll') -Force }
}
Copy-Item (Join-Path $root 'config\GameAudioBridge.ini') (Join-Path $root 'dist\GameAudioBridge.ini') -Force
Copy-Item (Join-Path $root 'README.md') (Join-Path $root 'dist\README.txt') -Force
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $root 'dist\LICENSE.txt') -Force
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $root 'dist\THIRD_PARTY_NOTICES.txt') -Force
if ($BuildInstaller) {
  $iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
  $isccPath = if ($iscc) { $iscc.Source } else { Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe' }
  if (!(Test-Path $isccPath)) { throw 'Install Inno Setup 6 or omit -BuildInstaller.' }
  & $isccPath (Join-Path $root 'installer\GameAudioBridge.iss')
  if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
}
Write-Host 'Build completed. Output: dist\'
Write-Host 'ASIO x64 backend is built when the ASIO SDK is present. The injected hook forwards supported 48 kHz WASAPI buffers to the ASIO host through a shared-memory float64 ring. WASAPI Exclusive remains unimplemented.'
