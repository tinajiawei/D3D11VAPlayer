# MediaEngine 壁纸 watchdog（docs/11 二期 M7 稳定性）：
# 循环启动壁纸进程；进程异常退出（崩溃）后 5 秒自动重启，实现"崩溃后恢复壁纸"。
# 用法:
#   powershell -ExecutionPolicy Bypass -File tools\start_wallpaper_watchdog.ps1
#   powershell -ExecutionPolicy Bypass -File tools\start_wallpaper_watchdog.ps1 -Web -Url "https://example.com"
param(
    [string]$Root = 'E:\新建文件夹\chatgpt',
    [string]$Media = 'samples\test_h264.mp4',
    [switch]$Web = $false,
    [string]$Url = 'about:blank'
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Root 'build\src\media_player_app.exe'
if (-not (Test-Path -LiteralPath $exe)) { Write-Error "找不到 $exe"; exit 1 }

$count = 0
while ($true) {
    $count++
    Write-Host ("[watchdog] 第 {0} 次启动壁纸进程 ..." -f $count)
    if ($Web) {
        $argList = @('--web-wallpaper', $Url, '--wallpaper-keep')
    } else {
        $argList = @('--wallpaper', '--wallpaper-keep', $Media)
    }
    $p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru -WindowStyle Hidden
    $p.WaitForExit()
    Write-Host ("[watchdog] 进程退出 code={0}，5 秒后重启（Ctrl+C 停止）" -f $p.ExitCode)
    Start-Sleep -Seconds 5
}