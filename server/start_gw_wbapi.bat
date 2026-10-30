@echo off
cd /d C:\Users\Administrator\AppData\Local\Temp\jt_gateway
set JT_VLM_BACKEND=wbapi
F:\Python\pythonw.exe -u gateway.py >> gateway_8000_wbapi.log 2>&1
