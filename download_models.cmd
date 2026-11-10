@echo off
REM Download a whisper.cpp ggml model into D:\AEplugin\models.
REM Usage: download_models.cmd [tiny|base|small|medium|large-v3]
setlocal
set MODEL=%1
if "%MODEL%"=="" set MODEL=small
echo Downloading ggml-%MODEL%.bin ...
curl -L -C - -o "D:\AEplugin\models\ggml-%MODEL%.bin" "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-%MODEL%.bin"
echo Done: D:\AEplugin\models\ggml-%MODEL%.bin
endlocal