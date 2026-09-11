param([ValidateSet('x64','arm64')][string]$Architecture='x64')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio C++ Build Tools and the Windows 10/11 SDK.' }
$developer=Join-Path $vs 'Common7/Tools/Launch-VsDevShell.ps1'
& $developer -Arch $Architecture -HostArch amd64 -SkipAutomaticLocation | Out-Null
$output=Join-Path $root "artifacts/native-$Architecture"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$exe=Join-Path $output 'WinDuo.exe'
& cl.exe /nologo /std:c++20 /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DNDEBUG /GL /Gy (Join-Path $PSScriptRoot 'app.cpp') "/Fo$output/" "/Fe$exe" /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT /MANIFEST:EMBED "/MANIFESTINPUT:$root/app.manifest" user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib runtimeobject.lib windowsapp.lib wtsapi32.lib powrprof.lib comctl32.lib d3d11.lib dxgi.lib bcrypt.lib
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
Write-Output $exe
