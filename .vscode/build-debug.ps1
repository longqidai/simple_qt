$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build-qmake-debug'
$qmake = 'D:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin\qmake.exe'
$make = 'D:\Qt\Qt5.14.2\Tools\mingw730_64\bin\mingw32-make.exe'
$qtBin = 'D:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin'
$mingwBin = 'D:\Qt\Qt5.14.2\Tools\mingw730_64\bin'

foreach ($tool in @($qmake, $make)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "缺少构建工具: $tool"
    }
}

$env:Path = "$qtBin;$mingwBin;$env:Path"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

Push-Location $buildDir
try {
    & $qmake (Join-Path $projectRoot 'simple_qt.pro') 'CONFIG+=debug'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $make -j4
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
