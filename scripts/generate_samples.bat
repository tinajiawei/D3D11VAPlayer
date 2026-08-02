@echo off
rem 生成验收样例（需要 ffmpeg 在 PATH 中；也可把 FFMPEG 改成你本机 ffmpeg.exe 的完整路径）
set FFMPEG=ffmpeg
if not exist samples mkdir samples

echo == h264 1080p ==
%FFMPEG% -y -v error -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=5 -c:v libx264 -pix_fmt yuv420p -an samples\test_long_1080p.mp4
%FFMPEG% -y -v error -f lavfi -i testsrc2=size=1280x720:rate=30:duration=5 -c:v libx264 -pix_fmt yuv420p -an samples\test_h264.mp4
%FFMPEG% -y -v error -f lavfi -i testsrc2=size=1280x720:rate=30:duration=5 -c:v libx264 -pix_fmt yuv420p -an samples\test_video_only.mp4

echo == hevc / av1 ==
%FFMPEG% -y -v error -f lavfi -i testsrc2=size=1280x720:rate=30:duration=5 -c:v libx265 -pix_fmt yuv420p -an samples\test_hevc.mp4
%FFMPEG% -y -v error -f lavfi -i testsrc2=size=1280x720:rate=30:duration=5 -c:v libaom-av1 -cpu-used 8 -b:v 1M -an samples\test_av1.mp4

echo == 旋转（竖屏） ==
%FFMPEG% -y -v error -f lavfi -i testsrc2=size=720x1280:rate=30:duration=5 -c:v libx264 -pix_fmt yuv420p -an -metadata:s:v rotate=90 samples\test_rot90.mp4
%FFMPEG% -y -v error -f lavfi -i testsrc2=size=720x1280:rate=30:duration=5 -c:v libx264 -pix_fmt yuv420p -an -metadata:s:v rotate=270 samples\test_rot270.mp4

echo == 音频 ==
%FFMPEG% -y -v error -f lavfi -i "sine=frequency=440:duration=5" -c:a pcm_s16le samples\test_wav.wav
%FFMPEG% -y -v error -f lavfi -i "sine=frequency=440:duration=5" -c:a libmp3lame -b:a 128k samples\test_mp3.mp3

echo == 图片 / GIF / WebP ==
%FFMPEG% -y -v error -f lavfi -i "color=c=red:size=640x360:duration=1" -frames:v 1 samples\test.png
%FFMPEG% -y -v error -f lavfi -i "testsrc2=size=320x240:rate=10:duration=2" -vf "fps=10,scale=320:240" samples\test.gif
%FFMPEG% -y -v error -f lavfi -i "color=c=blue:size=640x360:duration=1" -frames:v 1 -c:v libwebp samples\test.webp

echo == 完成：samples 目录已生成 ==