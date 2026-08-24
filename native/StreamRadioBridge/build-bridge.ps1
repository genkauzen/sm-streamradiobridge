$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$llvmRoot = Get-ChildItem -LiteralPath "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Directory |
    Where-Object { $_.Name -like 'MartinStorsjo.LLVM-MinGW*' } |
    ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Directory -Filter 'llvm-mingw-*' } |
    Select-Object -First 1

if (-not $llvmRoot) { throw 'LLVM-MinGW was not found. Install MartinStorsjo.LLVM-MinGW.MSVCRT first.' }
$cxx = Join-Path $llvmRoot.FullName 'bin\clang++.exe'
$out = Join-Path $root 'bin'
New-Item -ItemType Directory -Force -Path $out | Out-Null

& $cxx -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ `
    (Join-Path $root 'StreamRadioBridge.cpp') `
    -o (Join-Path $out 'StreamRadioBridge.dll') `
    '-Wl,--subsystem,windows' '-lkernel32' '-luser32' '-lws2_32'
if ($LASTEXITCODE -ne 0) { throw "StreamRadioBridge.dll build failed: $LASTEXITCODE" }

& $cxx -std=c++17 -O2 -static -static-libgcc -static-libstdc++ `
    (Join-Path $root 'StreamRadioBridgeInject.cpp') `
    '-municode' -o (Join-Path $out 'StreamRadioBridgeInject.exe') `
    '-Wl,--subsystem,console' '-lkernel32'
if ($LASTEXITCODE -ne 0) { throw "StreamRadioBridgeInject.exe build failed: $LASTEXITCODE" }

Write-Host "Built native bridge in $out"
