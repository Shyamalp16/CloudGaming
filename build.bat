@echo off

REM Navigate to the folder containing main.go, create_def.py, etc.
cd /d C:\webrtc\webrtc_dll

echo [Step 1] Building Go DLL...
go build -buildmode=c-shared -o webrtc.dll
IF ERRORLEVEL 1 (
    echo [ERROR] go build failed!
    goto :EOF
)

echo [Step 2] Dumping exports to exports.txt...
dumpbin /exports webrtc.dll > exports.txt
IF ERRORLEVEL 1 (
    echo [ERROR] dumpbin failed!
    goto :EOF
)

echo [Step 3] Running Python script to create webrtc.def via ChatGPT API...
REM Using 'python' synchronously. The batch file will wait until the Python process finishes.
python create_def.py
IF ERRORLEVEL 1 (
    echo [ERROR] Python script failed!
    goto :EOF
)

echo [Step 4] Creating webrtc.lib from webrtc.def...
lib /def:webrtc.def /out:webrtc.lib /machine:x64
IF ERRORLEVEL 1 (
    echo [ERROR] lib.exe failed!
    goto :EOF
)

echo [Step 5] Copying webrtc.dll to VS project Debug folder...
copy /Y webrtc.dll D:\Codes\CloudGaming\VSS\Capture\DisplayCaptureProject\x64\Debug
IF ERRORLEVEL 1 (
    echo [ERROR] Could not copy webrtc.dll!
    goto :EOF
)

REM If you also want to copy the .lib:
REM copy /Y webrtc.lib D:\Codes\CloudGaming\VSS\Capture\DisplayCaptureProject\x64\Debug

echo [SUCCESS] Build + DEF + LIB + copy steps completed successfully.
