@echo off
set IDF_EXPORT=C:\Espressif\frameworks\esp-idf-v5.5\export.bat
set PROJECT_DIR=C:\PROJECTS\airplay-esp32-usb

call "%IDF_EXPORT%"
cd /d "%PROJECT_DIR%"

cmd /k
