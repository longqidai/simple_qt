$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build-qmake-debug'
$qmake = 'D:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin\qmake.exe'
$make = 'D:\Qt\Qt5.14.2\Tools\mingw730_64\bin\mingw32-make.exe'
$qtBin = 'D:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin'
$mingwBin = 'D:\Qt\Qt5.14.2\Tools\mingw730_64\bin'
$opencvBin = Join-Path $projectRoot 'third_party\opencv-3.4.5\x64\mingw\bin'
$opencvRuntime = @(
    'libopencv_core345.dll',
    'libopencv_imgproc345.dll',
    'libopencv_imgcodecs345.dll',
    'libopencv_videoio345.dll',
    'opencv_ffmpeg345_64.dll'
)

foreach ($tool in @($qmake, $make)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "缺少构建工具: $tool"
    }
}

$env:Path = "$qtBin;$mingwBin;$env:Path"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

# qmake 的 QMAKE_POST_LINK 只在重新链接时运行。这里每次构建都先补齐运行库，
# 避免增量构建后按 F5 因缺少 OpenCV 的间接依赖而立即退出。
New-Item -ItemType Directory -Force -Path (Join-Path $buildDir 'debug') | Out-Null
foreach ($dll in $opencvRuntime) {
    $source = Join-Path $opencvBin $dll
    if (-not (Test-Path -LiteralPath $source)) {
        throw "缺少 OpenCV 运行库: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $buildDir 'debug') -Force
}

Push-Location $buildDir
try {
    & $qmake (Join-Path $projectRoot 'simple_qt.pro') 'CONFIG+=debug'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $make -j4
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    exit 0
}
finally {
    Pop-Location
}
