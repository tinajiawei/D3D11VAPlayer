# 软解 vs D3D11VA vs AMF 性能对比（docs/11 二期 M7）：
# 同一文件分别用三个后端播放 N 秒，采集进程 CPU 时间与绘制帧数。
# 用法: powershell -ExecutionPolicy Bypass -File tools\perf_compare.ps1 -Media samples\test_h264.mp4
param(
    [string]$Root = 'E:\新建文件夹\chatgpt',
    [string]$Media = 'samples\test_h264.mp4',
    [int]$Seconds = 6
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Root 'build\src\media_player_app.exe'
if (-not (Test-Path -LiteralPath $exe)) { Write-Error "找不到 $exe"; exit 1 }

$results = @()
foreach ($backend in @('sw', 'd3d11va', 'amf')) {
    $log = Join-Path $env:TEMP ("me_perf_{0}.log" -f $backend)
    Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
    $env:ME_HW_BACKEND = $backend
    $p = Start-Process -FilePath $exe -ArgumentList @('--debug', '--hw', (Join-Path $Root $Media)) `
        -PassThru -RedirectStandardError $log -WindowStyle Hidden
    Start-Sleep -Seconds $Seconds
    $p.Refresh()
    $cpu = [Math]::Round($p.TotalProcessorTime.TotalSeconds, 2)
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:ME_HW_BACKEND -ErrorAction SilentlyContinue
    $txt = ''
    if (Test-Path -LiteralPath $log) { $txt = Get-Content -LiteralPath $log -Raw -Encoding UTF8 }
    $draw = ([regex]::Matches($txt, 'DRAW_MS')).Count
    $decoder = ''
    if ($txt -match '解码器选择:\s+(\S+)') { $decoder = $Matches[1] }
    $results += [pscustomobject]@{
        Backend = $backend
        Decoder = $decoder
        CpuSeconds = $cpu
        Frames = $draw
        Fps = [Math]::Round($draw / $Seconds, 1)
    }
}
$results | Format-Table -AutoSize