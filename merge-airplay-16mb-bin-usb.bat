@echo off
setlocal

:: ============================================================
:: AirPlay ESP32 - Merge Build into Single .bin
:: Activates ESP-IDF 5.5 and merges build output into one file
:: that can be flashed at offset 0x0 (like the official releases)
:: ============================================================

set IDF_EXPORT=C:\Espressif\frameworks\esp-idf-v5.5\export.bat
set PROJECT_DIR=C:\PROJECTS\airplay-esp32-usb
set CHIP=esp32s3
set MERGED_BIN=merged-airplay2-usb.bin

:: Flash layout - must match what "idf.py build" / "idf.py flash" prints.
:: If you change partitions.csv or sdkconfig, re-check these offsets
:: (run "idf.py build" and look at the printed esptool.py command).
set BOOTLOADER_OFFSET=0x0
set PARTTABLE_OFFSET=0x8000
set OTADATA_OFFSET=0x19000
set APP_OFFSET=0x20000
set STORAGE_OFFSET=0x620000

title AirPlay ESP32 - Merge to Single BIN

echo ============================================================
echo  AirPlay ESP32 - Merge Build into Single .bin
echo ============================================================
echo  Project : %PROJECT_DIR%
echo  Chip    : %CHIP%
echo  Output  : %MERGED_BIN%
echo ============================================================
echo.

echo Activating ESP-IDF environment...
call "%IDF_EXPORT%" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Failed to activate ESP-IDF environment.
    pause
    exit /b 1
)

where idf.py >nul 2>&1
if errorlevel 1 (
    echo [ERROR] idf.py not found on PATH after activation.
    pause
    exit /b 1
)

cd /d "%PROJECT_DIR%"
if errorlevel 1 (
    echo [ERROR] Could not find project folder: %PROJECT_DIR%
    pause
    exit /b 1
)

if not exist "build\bootloader\bootloader.bin" (
    echo [ERROR] build\bootloader\bootloader.bin not found.
    echo Run a build first: idf.py build
    pause
    exit /b 1
)
if not exist "build\partition_table\partition-table.bin" (
    echo [ERROR] build\partition_table\partition-table.bin not found.
    pause
    exit /b 1
)
if not exist "build\airplay2-receiver.bin" (
    echo [ERROR] build\airplay2-receiver.bin not found.
    pause
    exit /b 1
)

echo Merging build output into %MERGED_BIN% ...
echo.

if exist "build\ota_data_initial.bin" (
    if exist "build\storage.bin" (
        python -m esptool --chip %CHIP% merge_bin -o %MERGED_BIN% ^
            --flash_mode dio --flash_freq 80m --flash_size 16MB ^
            %BOOTLOADER_OFFSET% build\bootloader\bootloader.bin ^
            %PARTTABLE_OFFSET% build\partition_table\partition-table.bin ^
            %OTADATA_OFFSET% build\ota_data_initial.bin ^
            %APP_OFFSET% build\airplay2-receiver.bin ^
            %STORAGE_OFFSET% build\storage.bin
    ) else (
        python -m esptool --chip %CHIP% merge_bin -o %MERGED_BIN% ^
            --flash_mode dio --flash_freq 80m --flash_size 16MB ^
            %BOOTLOADER_OFFSET% build\bootloader\bootloader.bin ^
            %PARTTABLE_OFFSET% build\partition_table\partition-table.bin ^
            %OTADATA_OFFSET% build\ota_data_initial.bin ^
            %APP_OFFSET% build\airplay2-receiver.bin
    )
) else (
    python -m esptool --chip %CHIP% merge_bin -o %MERGED_BIN% ^
        --flash_mode dio --flash_freq 80m --flash_size 16MB ^
        %BOOTLOADER_OFFSET% build\bootloader\bootloader.bin ^
        %PARTTABLE_OFFSET% build\partition_table\partition-table.bin ^
        %APP_OFFSET% build\airplay2-receiver.bin
)

if errorlevel 1 (
    echo.
    echo [ERROR] Merge failed. Check the offsets at the top of this script
    echo match what "idf.py build" printed for your project.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo  Done! Created: %PROJECT_DIR%\%MERGED_BIN%
echo ============================================================
echo.
echo To flash it to a board, run:
echo   python -m esptool --chip %CHIP% -p COMx -b 460800 write_flash 0x0 %MERGED_BIN%
echo.

pause
endlocal
exit /b 0
