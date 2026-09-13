# build_win.ps1 — Windows 環境で shark と sharkvm をビルドする（Visual Studio MSVC）
#
#   tools\build_win.bat          ビルドする
#   tools\build_win.bat test     ビルド後に tests\ を実行する（sh が必要）
#   tools\build_win.bat clean    ビルド成果物を削除する
#
# PowerShell から直接実行する場合:
#   powershell -ExecutionPolicy Bypass -File tools\build_win.ps1
#
# ビルド内容は Makefile と同一。ソース一覧は Makefile の RT_SRC / FE_SRC をマスターとし、
# 本スクリプトはその複製です（追加・削除時は両方を更新。make print-core-src で一覧を出力可能）。
#
# 必要環境: Visual Studio 2019 以降の「C++ によるデスクトップ開発」。
# 外部ライブラリは不要（ウィンドウ表示に必要な user32.dll は実行時に動的ロード）。
# 日本語フォント描画用の FreeType は任意（README の「日本語フォントの表示」を参照）。
# 組み込む場合のみ、パスを指定する:
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
  Write-Host "ビルド成果物を削除しました"
  exit 0
}

# --- Visual Studio のビルド環境（MSVC）を初期化する ---
# vcvars64.bat により設定された環境変数を取得し、
# 現在の PowerShell プロセスへ反映する（PowerShell から直接バッチファイル環境変数は継承されないため）
function Enter-MsvcEnv {
  if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) {
    Write-Host "Visual Studio が見つかりません。"
    Write-Host "  インストール方法: https://visualstudio.microsoft.com/ から"
    Write-Host "                    「C++ によるデスクトップ開発」をインストールしてください"
    exit 1
  }
  $vs = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
  if (-not $vs) {
    Write-Host "Visual Studio に C++ ビルドツールがインストールされていません。"
    Write-Host "  対処法: Visual Studio Installer で「C++ によるデスクトップ開発」を追加してください"
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
    Write-Host "コンパイラ（cl.exe）の環境設定に失敗しました"
    exit 1
  }
}

# --- FreeType（日本語フォント描画用。任意）---
#
# 唯一の外部ライブラリであり、未導入でもビルドおよび動作は可能です（日本語が □ 表示になるのみ）。
# Windows には pkg-config がないため、ソースコードを取得して静的ライブラリとしてビルドし、
# shark.exe に静的リンクします（単一バイナリ配布を可能にするため）。
#
#   tools\build_win.bat freetype   初回のみ実行してビルド
#   tools\build_win.bat            ビルド済みであれば自動検出してリンク
$ftVer = "VER-2-13-3"
$ftSrc = Join-Path $root "build\freetype-src"
$ftInc = Join-Path $ftSrc "include"
$ftLib = Join-Path $root "build\freetype\freetype.lib"

# docs/INSTALL.ANY に記載されたファイル一覧。include/freetype/config/ftmodule.h で
# 指定されるコンポーネントを網羅（不足時はリンクエラーとなります）
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
    Write-Host "FreeType $ftVer をダウンロードしています..."
    $url = "https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/$ftVer"
    try {
      Invoke-WebRequest -Uri $url -OutFile $tgz -UseBasicParsing
    } catch {
      Write-Host "ダウンロードに失敗しました: $url"
      Write-Host "  対処法: FreeType のソースコードを手動で $ftSrc に配置してから再実行してください"
      exit 1
    }
    # Windows 標準の tar を呼び出す（PATH に MSYS の tar がある場合のパス解釈不一致を回避）
    $tar = Join-Path $env:SystemRoot "System32\tar.exe"
    if (-not (Test-Path $tar)) { $tar = "tar" }
    Push-Location (Join-Path $root "build")
    & $tar -xzf "freetype.tar.gz"
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { Write-Host "アーカイブの展開に失敗しました: $tgz"; exit 1 }
    $un = Join-Path $root "build\freetype-$ftVer"
    if (-not (Test-Path $un)) { Write-Host "アーカイブの展開に失敗しました: $tgz"; exit 1 }
    if (Test-Path $ftSrc) { Remove-Item -Recurse -Force $ftSrc }
    Move-Item $un $ftSrc
    Remove-Item -Force $tgz
  }

  $ftObj = Join-Path $root "build\freetype\obj"
  New-Item -ItemType Directory -Force -Path $ftObj | Out-Null
  $srcs = $ftFiles | ForEach-Object { Join-Path $ftSrc "src\$_" }
  Write-Host "FreeType をビルドしています..."
  # /DFT2_BUILD_LIBRARY はライブラリ本体のビルド指定フラグ
  & cl /nologo /O2 /W0 /MP /DFT2_BUILD_LIBRARY "/I$ftInc" /c @srcs "/Fo:$ftObj\"
  if ($LASTEXITCODE -ne 0) { exit 1 }
  $objs = Get-ChildItem -Path $ftObj -Filter *.obj | ForEach-Object { $_.FullName }
  & lib /nologo "/OUT:$ftLib" @objs
  if ($LASTEXITCODE -ne 0) { exit 1 }
  Write-Host "ビルド完了: build\freetype\freetype.lib"
}

if ($Task -eq "freetype") {
  Build-Freetype
  Write-Host ""
  Write-Host "この後 tools\build_win.bat で再ビルドすると、日本語フォントが描画可能になります"
  exit 0
}

Enter-MsvcEnv
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $root

# --- コンパイルオプション ---
#   /utf-8                     ソースコードおよび実行文字セットを UTF-8 に設定
#   /EHs-c- /GR-               例外と RTTI を無効化（spec/skeleton.md）
#   /D_CRT_SECURE_NO_WARNINGS  fopen などの非推奨セキュリティ警告（C4996）を抑止
#   /MP                        並列コンパイルを有効化
$cflags = @("/nologo", "/std:c++17", "/utf-8", "/O2", "/W3", "/EHs-c-", "/GR-",
            "/D_CRT_SECURE_NO_WARNINGS", "/MP")
$ldlibs = @()
# パスが未指定の場合、build_win.bat freetype で生成されたライブラリを探索
if ($FtInclude -eq "" -and (Test-Path $ftLib) -and (Test-Path (Join-Path $ftInc "ft2build.h"))) {
  $FtInclude = $ftInc
  $FtLib = $ftLib
}
if ($FtInclude -ne "") {
  # FreeType をリンク（未指定時は内蔵 5×7 フォントのみとなり、日本語は □ 表示）
  if ($FtLib -eq "") { Write-Host "-FtInclude を指定する場合は -FtLib の指定も必要です"; exit 1 }
  $cflags += @("/DSHARK_FREETYPE", "/I$FtInclude")
  $ldlibs += $FtLib
  Write-Host "FreeType: $FtLib"
} else {
  Write-Host "FreeType なし（日本語は □ 表示となります。tools\build_win.bat freetype で追加可能）"
}

# RT_SRC — ランタイム用ソース（sharkvm に必要な最小限のセット）
$rtSrc = @(
  "core\support.cpp", "core\value.cpp", "core\program.cpp", "core\types.cpp", "core\diag.cpp",
  "core\vm.cpp", "core\registry.cpp", "core\bytecode.cpp", "core\runtime.cpp",
  "core\platform\desktop.cpp", "core\platform\console.cpp",
  "core\lib\format.cpp", "core\lib\builtin.cpp", "core\lib\math.cpp", "core\lib\time.cpp",
  "core\lib\task.cpp", "core\lib\fmt.cpp", "core\lib\path.cpp", "core\lib\file.cpp",
  "core\lib\os.cpp", "core\lib\text.cpp", "core\lib\json.cpp", "core\lib\test.cpp",
  "core\lib\crypto.cpp", "core\lib\ui.cpp")

# FE_SRC — フロントエンド用ソース（字句解析・構文解析・型検査・コード生成）
$feSrc = @("core\lexer.cpp", "core\parser.cpp", "core\check.cpp", "core\codegen.cpp",
           "core\fmt_src.cpp", "core\shark.cpp")

# tests\ の C++ ユニットテスト。Makefile の test 対象と同一の4バイナリをビルド
#   memcheck  … メモリ解放と上限チェック
#   bytecheck … 不正なバイトコードの拒否検証
#   imecheck  … IME による文字入力テスト
#   uicheck   … UIウィジェットのクリック・ホバー動作テスト
$testSrc = @("tests\memcheck.cpp", "tests\bytecheck.cpp",
             "tests\imecheck.cpp", "tests\uicheck.cpp")

$allSrc = $rtSrc + $feSrc + @("frontend\main.cpp", "frontend\vm_main.cpp") + $testSrc

Write-Host "コアをコンパイルしています..."
& cl @cflags /c @allSrc "/Fo:$out\"
if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }

# .cpp 名から .obj 名を解決
function ObjOf($files) { $files | ForEach-Object { Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($_) + ".obj") } }
$rtObj = ObjOf $rtSrc
$feObj = ObjOf $feSrc

Write-Host "リンクしています..."
& cl @cflags "/Fe:$root\shark.exe" @rtObj @feObj (Join-Path $out "main.obj") @ldlibs
if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }
& cl @cflags "/Fe:$root\sharkvm.exe" @rtObj (Join-Path $out "vm_main.obj") @ldlibs
if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }
foreach ($t in @("memcheck", "bytecheck", "imecheck", "uicheck")) {
  & cl @cflags "/Fe:$root\tests\$t.exe" @rtObj @feObj (Join-Path $out "$t.obj") @ldlibs
  if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }
}

Write-Host "ビルド完了: shark.exe / sharkvm.exe"
Pop-Location

if ($Task -eq "test") {
  if (-not (Get-Command sh.exe -ErrorAction SilentlyContinue)) {
    Write-Host ""
    Write-Host "tests\run.sh の実行には sh が必要です（Git for Windows に同梱されています）"
    exit 1
  }
  Write-Host ""
  Push-Location $root
  & sh tests/run.sh
  $code = $LASTEXITCODE
  Pop-Location
  exit $code
}
