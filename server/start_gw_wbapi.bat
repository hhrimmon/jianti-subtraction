@echo off
rem start_gw_wbapi.bat: start gateway.py in wbapi (local API) mode, logs to gateway_8000_wbapi.log
cd /d "%~dp0"
set JT_VLM_BACKEND=wbapi
pythonw.exe -u gateway.py >> gateway_8000_wbapi.log 2>&1
