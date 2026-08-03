# MediaEngine 一期验收脚本
# 用法: powershell -ExecutionPolicy Bypass -File tools\acceptance.ps1
# 输出: tools\acceptance_results.md
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$root = 'E:\新建文件夹\chatgpt'
$exe = Join-Path $root 'build\src\media_player_app.exe'
$samples = Join-Path $root 'samples'
$outDir = Join-Path $root 'tools'
$logDir = Join-Path $env:TEMP 'me_accept'
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

function Run-Case {
    param(
        [string]$Name,
        [string[]]$CaseArgs,
        [int]$Seconds,
        [string]$File
    )
    $log = Join-Path $logDir ($Name + '.log')
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    $argList = @()
    foreach ($a in $CaseArgs) { $argList += $a }
    if ($File) { $argList += (Join-Path $samples $File) }
    $p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru -RedirectStandardError $log -WindowStyle Hidden
    Start-Sleep -Seconds $Seconds
    $alive = -not $p.HasExited
    if ($alive) { Stop-Process -Id $p.Id -Force }
    $txt = ''
    if (Test-Path -LiteralPath $log) { $txt = Get-Content -LiteralPath $log -Raw -Encoding UTF8 }
    return [pscustomobject]@{
        Name = $Name
        Alive = $alive
        PlayStart = ([regex]::Matches($txt, '播放开始')).Count
        OpenFail = [Math]::Max(0, ([regex]::Matches($txt, '打开失败')).Count - ([regex]::Matches($txt, '硬解后端')).Count)
        DecodeFail = ([regex]::Matches($txt, '解码失败')).Count
        ReadFail = ([regex]::Matches($txt, '读包失败')).Count
        AudioResetFail = ([regex]::Matches($txt, '连续写入失败')).Count
        Draw = ([regex]::Matches($txt, 'DRAW_MS')).Count
        Drop = ([regex]::Matches($txt, '\[loop\] DROP')).Count
        Anchor = ([regex]::Matches($txt, '音频起播锚定')).Count
        Freeze = ([regex]::Matches($txt, '冻结主时钟')).Count
        SeekOk = ([regex]::Matches($txt, 'seek 到')).Count
        Resampler = ([regex]::Matches($txt, '重采样开启')).Count
        DecoderSw = ([regex]::Matches($txt, '解码器选择:  sw')).Count
        DecoderHw = ([regex]::Matches($txt, '解码器选择:  d3d11va')).Count
        DiffMean = 0.0
        DiffMax = 0.0
    }
}

function Get-DiffStats {
    param([string]$Name)
    $log = Join-Path $logDir ($Name + '.log')
    if (-not (Test-Path -LiteralPath $log)) { return }
    $vals = @()
    Get-Content -LiteralPath $log -Encoding UTF8 | ForEach-Object {
        if ($_ -match 'diff=\s*(-?[\d.eE+-]+)') { $vals += [double]$Matches[1] }
    }
    if ($vals.Count -eq 0) { return }
    $mean = ($vals | Measure-Object -Average).Average
    $maxAbs = ($vals | ForEach-Object { [math]::Abs($_) } | Measure-Object -Maximum).Maximum
    return [pscustomobject]@{ Mean = $mean; MaxAbs = $maxAbs; N = $vals.Count }
}

$results = @()
function Add-Case {
    param($Name, $Seconds, [string[]]$CaseArgs = @(), [string]$File = '')
    $r = Run-Case -Name $Name -CaseArgs $CaseArgs -Seconds $Seconds -File $File
    $ds = Get-DiffStats -Name $Name
    if ($ds) { $r.DiffMean = $ds.Mean; $r.DiffMax = $ds.MaxAbs }
    $script:results += $r
    $flag = if ($r.Alive -and $r.OpenFail -eq 0 -and $r.DecodeFail -eq 0) { 'PASS' } else { 'FAIL' }
    Write-Output ("{0,-24} {1}  alive={2} play={3} draw={4} drop={5} diff_mean={6:n3} diff_max={7:n3}" -f $Name, $flag, $r.Alive, $r.PlayStart, $r.Draw, $r.Drop, $r.DiffMean, $r.DiffMax)
}

Write-Output '===== 一、格式验收（每格式 7s 存活） ====='
Add-Case 'F1_h264' 7 -File 'test_h264.mp4'
Add-Case 'F2_hevc' 7 -File 'test_hevc.mp4'
Add-Case 'F3_av1' 7 -File 'test_av1.mp4'
Add-Case 'F4_long1080p' 7 -File 'test_long_1080p.mp4'
Add-Case 'F5_rot90' 7 -File 'test_rot90.mp4'
Add-Case 'F6_rot270' 7 -File 'test_rot270.mp4'
Add-Case 'F7_video_only' 7 -File 'test_video_only.mp4'
Add-Case 'F8_wav' 7 -File 'test_wav.wav'
Add-Case 'F9_mp3' 7 -File 'test_mp3.mp3'
Add-Case 'F10_png' 7 -File 'test.png'
Add-Case 'F11_gif' 7 -File 'test.gif'
Add-Case 'F12_webp' 7 -File 'test.webp'
Add-Case 'F13_markers' 7 -File 'test_markers.mp4'

Write-Output '===== 二、音视频同步分场景（debug 采集 diff） ====='
Add-Case 'S1_normal' 12 @('--debug') 'test_long_1080p.mp4'
Add-Case 'S2_seek_rapid' 12 @('--seek','20','--debug') 'test_long_1080p.mp4'
Add-Case 'S3_eof_seek' 14 @('--eof-seek','10','--debug') 'test_h264.mp4'
Add-Case 'S4_speed_2x' 14 @('--speed','2','--debug') 'test_long_1080p.mp4'
Add-Case 'S5_speed_05x' 14 @('--speed','0.5','--debug') 'test_long_1080p.mp4'
Add-Case 'S6_pause_resume' 12 @('--pause-test','--debug') 'test_long_1080p.mp4'
Add-Case 'S7_reopen' 12 @('--reopen','4','--debug') 'test_long_1080p.mp4'
Add-Case 'S8_video_only_sync' 8 @('--debug') 'test_video_only.mp4'
Add-Case 'S9_device_switch' 10 @('--device','2','--debug') 'test_long_1080p.mp4'
Add-Case 'S10_device_fail_fallback' 10 @('--device','0','--debug') 'test_long_1080p.mp4'

Write-Output '===== 三、软硬解对比（解码器选择/绘制） ====='
Add-Case 'H1_h264_sw' 8 @('--debug') 'test_h264.mp4'
Add-Case 'H2_h264_hw' 8 @('--hw','--debug') 'test_h264.mp4'
Add-Case 'H3_hevc_sw' 8 @('--debug') 'test_hevc.mp4'
Add-Case 'H4_hevc_hw' 8 @('--hw','--debug') 'test_hevc.mp4'
Add-Case 'H5_av1_sw' 8 @('--debug') 'test_av1.mp4'
Add-Case 'H6_av1_hw' 8 @('--hw','--debug') 'test_av1.mp4'

$md = @(
    '# MediaEngine 一期验收结果',
    '',
    '| 场景 | 存活 | 播放开始 | 打开失败 | 解码失败 | 绘制帧 | 丢帧 | 冻结 | 锚定 | diff均值 | diff最大 |',
    '| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |'
)
foreach ($r in $results) {
    $md += ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9:n3} | {10:n3} |' -f $r.Name, $r.Alive, $r.PlayStart, $r.OpenFail, $r.DecodeFail, $r.Draw, $r.Drop, $r.Freeze, $r.Anchor, $r.DiffMean, $r.DiffMax)
}
[System.IO.File]::WriteAllLines((Join-Path $outDir 'acceptance_results.md'), $md, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ''
Write-Output ("结果表已写入: " + (Join-Path $outDir 'acceptance_results.md'))
