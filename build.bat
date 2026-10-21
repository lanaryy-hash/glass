@echo off
rem Compile Glass : le moteur Spotify (Rust/librespot) puis l'interface (C++).
cd /d "%~dp0"
set CARGO=%USERPROFILE%\.cargo\bin\cargo.exe

echo [1/2] Moteur Spotify (Rust) - la 1re fois ca prend 5 a 10 minutes...
pushd engine
"%CARGO%" build --release || goto :err
popd

echo [2/2] Interface (C++)...
cmake -S . -B build || goto :err
cmake --build build --config Release || goto :err
copy /y engine\target\release\glass-engine.exe build\Release\ >nul || goto :err

echo.
echo OK ! Lance : build\Release\Glass.exe
if not "%1"=="-q" pause
exit /b 0

:err
echo.
echo ECHEC de la compilation (copie le texte ci-dessus a Claude)
if not "%1"=="-q" pause
exit /b 1
