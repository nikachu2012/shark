# build_win.ps1 — Windows で shark と sharkvm を作る（Visual Studio の C++）
#
#   tools\build_win.bat          作る
#   tools\build_win.bat test     作ってから tests\ を走らせる（sh が要る）
#   tools\build_win.bat clean    作ったものを消す
#
# PowerShell から直に呼ぶときは:
#   powershell -ExecutionPolicy Bypass -File tools\build_win.ps1
#
# 作る中身は Makefile と同じ。ソースの一覧は Makefile の RT_SRC / FE_SRC が正で、
# ここはその写し（増やしたら両方を直す。make print-core-src が一覧を出す）。
#
# 要るもの: Visual Studio 2019 以降の「C++ によるデスクトップ開発」。
# 外のライブラリは1つも要らない（窓は user32.dll を実行時に取りに行く）。
# 日本語の字を出す FreeType は任意（README の「日本語の字を出す」）。
# 入れたときだけ、置き場所を渡す:
#   tools\build_win.bat build -FtInclude C:\freetype\include -FtLib C:\freetype\lib\freetype.lib
param([string]$Task = "build", [string]$FtInclude = "", [string]$FtLib = "")

$ErrorActionPreference = "Stop"
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "build\win"

if ($Task -eq "clean") {
  if (Test-Path $out) { Remove-Item -Recurse -Force $out }
  foreach ($f in @("shark.exe", "sharkvm.exe", "tests\memcheck.exe", "tests\bytecheck.exe",
                 "tests\imecheck.exe", "tests\uicheck.exe")) {
    $p = Join-Path $root $f
    if (Test-Path $p) { Remove-Item -Force $p }
  }
  Write-Host "消しました"
  exit 0
}

# --- Visual Studio の道具を使える状態にする --------------------------------
# vcvars64.bat は環境変数をたくさん置いていく。cmd で呼んでから、
# 置いていった環境変数をこちらに写す（PowerShell から .bat は呼べないため）
function Enter-MsvcEnv {
  if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) {
    Write-Host "Visual Studio が見つかりません。"
    Write-Host "  入れ方: https://visualstudio.microsoft.com/ から"
    Write-Host "          「C++ によるデスクトップ開発」を選んで入れます"
    exit 1
  }
  $vs = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
  if (-not $vs) {
    Write-Host "Visual Studio に C++ の道具が入っていません。"
    Write-Host "  直し方: Visual Studio Installer で「C++ によるデスクトップ開発」を足します"
    exit 1
  }
  $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
  if (-not (Test-Path $vcvars)) {
    Write-Host "vcvars64.bat が見つかりません: $vcvars"
    exit 1
  }
  cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
  }
  if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "コンパイラ（cl.exe）を使える状態にできませんでした"
    exit 1
  }
}

# --- FreeType（日本語などの字形。任意）-------------------------------------
#
# 唯一の外部ライブラリで、入れなくても処理系は作れて動く（日本語が □ になるだけ）。
# Windows には pkg-config が無いので、ここで元を取ってきて**静的に**作り、
# shark.exe の中に入れてしまう（配るときに DLL が付いて回らない）。
#
#   tools\build_win.bat freetype   一度だけ作る
#   tools\build_win.bat            作ってあれば、自動で使う
$ftVer = "VER-2-13-3"
$ftSrc = Join-Path $root "build\freetype-src"
$ftInc = Join-Path $ftSrc "include"
$ftLib = Join-Path $root "build\freetype\freetype.lib"

# docs/INSTALL.ANY が挙げているもの。include/freetype/config/ftmodule.h が
# 名指しする組み立て部品は、ぜんぶ揃えておく（欠けると繋ぐときに足りなくなる）
$ftFiles = @(
  "base\ftsystem.c", "base\ftinit.c", "base\ftdebug.c", "base\ftbase.c",
  "base\ftbbox.c", "base\ftglyph.c", "base\ftbdf.c", "base\ftbitmap.c",
  "base\ftcid.c", "base\ftfstype.c", "base\ftgasp.c", "base\ftgxval.c",
  "base\ftmm.c", "base\ftotval.c", "base\ftpatent.c", "base\ftpfr.c",
  "base\ftstroke.c", "base\ftsynth.c", "base\fttype1.c", "base\ftwinfnt.c",
  "bdf\bdf.c", "cff\cff.c", "cid\type1cid.c", "pcf\pcf.c", "pfr\pfr.c",
  "sfnt\sfnt.c", "truetype\truetype.c", "type1\type1.c", "type42\type42.c",
  "winfonts\winfnt.c",
  "smooth\smooth.c", "raster\raster.c", "sdf\sdf.c", "svg\svg.c",
  "autofit\autofit.c", "cache\ftcache.c", "gzip\ftgzip.c", "lzw\ftlzw.c",
  "psaux\psaux.c", "pshinter\pshinter.c", "psnames\psnames.c")

function Build-Freetype {
  Enter-MsvcEnv
  New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null
  if (-not (Test-Path (Join-Path $ftInc "ft2build.h"))) {
    $tgz = Join-Path $root "build\freetype.tar.gz"
    Write-Host "FreeType $ftVer を取ってきています..."
    $url = "https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/$ftVer"
    try {
      Invoke-WebRequest -Uri $url -OutFile $tgz -UseBasicParsing
    } catch {
      Write-Host "取ってこられませんでした: $url"
      Write-Host "  直し方: 自分で FreeType の元を $ftSrc に置いてから、もう一度呼びます"
      exit 1
    }
    # Windows のものを名指しで呼ぶ。PATH に MSYS の tar があると、
    # C:\... の : を「別の機械」と読んでしまう
    $tar = Join-Path $env:SystemRoot "System32\tar.exe"
    if (-not (Test-Path $tar)) { $tar = "tar" }
    Push-Location (Join-Path $root "build")
    & $tar -xzf "freetype.tar.gz"
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { Write-Host "広げられませんでした: $tgz"; exit 1 }
    $un = Join-Path $root "build\freetype-$ftVer"
    if (-not (Test-Path $un)) { Write-Host "広げられませんでした: $tgz"; exit 1 }
    if (Test-Path $ftSrc) { Remove-Item -Recurse -Force $ftSrc }
    Move-Item $un $ftSrc
    Remove-Item -Force $tgz
  }

  $ftObj = Join-Path $root "build\freetype\obj"
  New-Item -ItemType Directory -Force -Path $ftObj | Out-Null
  $srcs = $ftFiles | ForEach-Object { Join-Path $ftSrc "src\$_" }
  Write-Host "FreeType を作っています..."
  # /DFT2_BUILD_LIBRARY は「ライブラリ本体を作っている側」の目印
  & cl /nologo /O2 /W0 /MP /DFT2_BUILD_LIBRARY "/I$ftInc" /c @srcs "/Fo:$ftObj\"
  if ($LASTEXITCODE -ne 0) { exit 1 }
  $objs = Get-ChildItem -Path $ftObj -Filter *.obj | ForEach-Object { $_.FullName }
  & lib /nologo "/OUT:$ftLib" @objs
  if ($LASTEXITCODE -ne 0) { exit 1 }
  Write-Host "できました: build\freetype\freetype.lib"
}

if ($Task -eq "freetype") {
  Build-Freetype
  Write-Host ""
  Write-Host "このあと tools\build_win.bat で作り直すと、日本語の字が出るようになります"
  exit 0
}

Enter-MsvcEnv
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $root

# --- 作るときの決めごと -----------------------------------------------------
#   /utf-8                     ソースも文字列も UTF-8（無いと日本語が化ける）
#   /EHs-c- /GR-               例外と RTTI を使わない（spec/skeleton.md）
#   /D_CRT_SECURE_NO_WARNINGS  fopen などの「危ないかも」の知らせを止める
#   /MP                        使える分だけ同時に作る
$cflags = @("/nologo", "/std:c++17", "/utf-8", "/O2", "/W3", "/EHs-c-", "/GR-",
            "/D_CRT_SECURE_NO_WARNINGS", "/MP")
$ldlibs = @()
# 場所を渡されていなければ、build_win.bat freetype で作ったものを探す
if ($FtInclude -eq "" -and (Test-Path $ftLib) -and (Test-Path (Join-Path $ftInc "ft2build.h"))) {
  $FtInclude = $ftInc
  $FtLib = $ftLib
}
if ($FtInclude -ne "") {
  # FreeType を使う（無ければ内蔵の 5×7 の字形だけになり、日本語は □ になる）
  if ($FtLib -eq "") { Write-Host "-FtInclude を渡すときは -FtLib も要ります"; exit 1 }
  $cflags += @("/DSHARK_FREETYPE", "/I$FtInclude")
  $ldlibs += $FtLib
  Write-Host "FreeType: $FtLib"
} else {
  Write-Host "FreeType なし（日本語の字は □ になります。tools\build_win.bat freetype で足せます）"
}

# RT_SRC — バイトコードを動かすのに要るもの（＝実行装置。sharkvm はこれだけ）
$rtSrc = @(
  "core\support.cpp", "core\value.cpp", "core\program.cpp", "core\types.cpp", "core\diag.cpp",
  "core\vm.cpp", "core\registry.cpp", "core\bytecode.cpp", "core\runtime.cpp",
  "core\platform\desktop.cpp", "core\platform\console.cpp",
  "core\lib\format.cpp", "core\lib\builtin.cpp", "core\lib\math.cpp", "core\lib\time.cpp",
  "core\lib\task.cpp", "core\lib\fmt.cpp", "core\lib\path.cpp", "core\lib\file.cpp",
  "core\lib\os.cpp", "core\lib\text.cpp", "core\lib\json.cpp", "core\lib\test.cpp",
  "core\lib\crypto.cpp", "core\lib\ui.cpp")

# FE_SRC — ソースからバイトコードを作るところ
$feSrc = @("core\lexer.cpp", "core\parser.cpp", "core\check.cpp", "core\codegen.cpp",
           "core\fmt_src.cpp", "core\shark.cpp")

# tests\ の C++ 側の検査。Makefile の test が要るものと同じ4つを作る
#   memcheck  … 後始末とメモリの上限
#   bytecheck … 壊れたバイトコードを断るか
#   imecheck  … 変換つきの文字入力（IME）
#   uicheck   … 部品を押した・合わせたときの動き
$testSrc = @("tests\memcheck.cpp", "tests\bytecheck.cpp",
             "tests\imecheck.cpp", "tests\uicheck.cpp")

$allSrc = $rtSrc + $feSrc + @("frontend\main.cpp", "frontend\vm_main.cpp") + $testSrc

Write-Host "コアを作っています..."
& cl @cflags /c @allSrc "/Fo:$out\"
if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }

# .cpp の名前から .obj の名前を作る（cl は /Fo にまとめると平らに並べる）
function ObjOf($files) { $files | ForEach-Object { Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($_) + ".obj") } }
$rtObj = ObjOf $rtSrc
$feObj = ObjOf $feSrc

Write-Host "繋いでいます..."
& cl @cflags "/Fe:$root\shark.exe" @rtObj @feObj (Join-Path $out "main.obj") @ldlibs
if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }
& cl @cflags "/Fe:$root\sharkvm.exe" @rtObj (Join-Path $out "vm_main.obj") @ldlibs
if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }
foreach ($t in @("memcheck", "bytecheck", "imecheck", "uicheck")) {
  & cl @cflags "/Fe:$root\tests\$t.exe" @rtObj @feObj (Join-Path $out "$t.obj") @ldlibs
  if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }
}

Write-Host "できました: shark.exe / sharkvm.exe"
Pop-Location

if ($Task -eq "test") {
  if (-not (Get-Command sh.exe -ErrorAction SilentlyContinue)) {
    Write-Host ""
    Write-Host "tests\run.sh を走らせるには sh が要ります（Git for Windows に付いてきます）"
    exit 1
  }
  Write-Host ""
  Push-Location $root
  & sh tests/run.sh
  $code = $LASTEXITCODE
  Pop-Location
  exit $code
}
