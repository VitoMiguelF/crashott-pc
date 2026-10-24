@echo off
rem GPU report for black-screen / flicker reports (issue #3). Runs the installed game
rem (dist\CrashRecomp.exe) with [video] gpu_diagnostics on and each [video] gpu_safe_mode in turn,
rem asks after each run whether the screen flickered, and packs the logs into gpu_report.zip.
rem The overrides go through CRASH_SETTINGS: settings.toml is not changed.
rem Works from the project root or from tools\. A Vulkan validation layer in vulkan_layers\ next to
rem dist\ (VkLayer_khronos_validation.json + .dll) adds a last run with validation.
setlocal EnableExtensions
if exist "%~dp0dist\CrashRecomp.exe" (cd /d "%~dp0") else (cd /d "%~dp0..")
if not exist "dist\CrashRecomp.exe" (
    echo The game is not installed here yet: run CrashSetup.exe first, then this file again.
    echo O jogo ainda nao esta instalado aqui: rode o CrashSetup.exe e depois este arquivo de novo.
    pause
    exit /b 1
)
if not exist gpu_report mkdir gpu_report
> gpu_report\results.txt echo CrashRecomp GPU report, %DATE% %TIME%

echo.
echo CrashRecomp GPU test
echo ====================
echo The game opens several times. Each time: load your save, play 2-3 minutes where the
echo screen used to flicker, then quit (EXIT GAME or close the window) and answer the question.
echo O jogo abre varias vezes. Em cada uma: carregue o save, jogue 2-3 minutos onde a tela
echo piscava, saia do jogo e responda a pergunta.
echo.
pause

call :run 1 off "normal (safe mode off)"
call :run 2 on "safe mode: everything on"
if "%FLICKER%"=="yes" goto validation

echo.
echo Safe mode helped. The next runs turn on one part at a time, to find which one does it.
echo O modo seguro ajudou. Os proximos testes ligam uma parte por vez para achar qual resolve.
set N=3
for %%M in (barriers serial present instances depth memory cache) do call :part %%M
goto validation

:part
if "%STOP%"=="yes" exit /b 0
call :run %N% %1 "safe mode: %1 only"
set /a N+=1
choice /c YN /m "Continue with the next test? / Continuar com o proximo teste?"
if errorlevel 2 set "STOP=yes"
exit /b 0

:validation
if not exist "vulkan_layers\VkLayer_khronos_validation.json" goto pack
echo.
echo Last run: Vulkan validation. The game will be slow; 1-2 minutes are enough.
echo Ultimo teste: validacao do Vulkan. O jogo fica lento; 1-2 minutos bastam.
choice /c YN /m "Run it? / Rodar?"
if errorlevel 2 goto pack
set "VK_ADD_LAYER_PATH=%CD%\vulkan_layers"
set "CRASH_GPU_DEBUG=1"
call :run 99 off "validation"
set "VK_ADD_LAYER_PATH="
set "CRASH_GPU_DEBUG="

:pack
powershell -NoProfile -Command "Compress-Archive -Path 'gpu_report\*' -DestinationPath 'gpu_report.zip' -Force"
echo.
echo Done. Please attach this file to the issue / Pronto. Anexe este arquivo na issue:
echo   %CD%\gpu_report.zip
echo (The logs contain your GPU, driver and Windows user folder path. / Os logs tem a GPU, o
echo driver e o caminho da sua pasta de usuario do Windows.)
pause
exit /b 0

:run
echo.
echo === Test %1: %~3 ===
pause
set "CRASH_SETTINGS=video.gpu_diagnostics=true;video.gpu_safe_mode=%2"
del /q "dist\userdata\crashrecomp_log.txt" 2>nul
start "" /d "dist" /wait "dist\CrashRecomp.exe"
set "CRASH_SETTINGS="
if exist "dist\userdata\crashrecomp_log.txt" copy /y "dist\userdata\crashrecomp_log.txt" "gpu_report\test%1_%2.txt" >nul
choice /c YN /m "Did the screen flicker black (all or part of it)? / A tela piscou de preto (toda ou parte)?"
if errorlevel 2 (set "FLICKER=no") else (set "FLICKER=yes")
>> gpu_report\results.txt echo test %1, %~3 (gpu_safe_mode=%2): flicker %FLICKER%
exit /b 0
