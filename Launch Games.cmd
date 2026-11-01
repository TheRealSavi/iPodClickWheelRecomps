@echo off
cd /d "%~dp0"
where uv >nul 2>nul
if errorlevel 1 (
    echo UV is required. Install it from https://docs.astral.sh/uv/getting-started/installation/
    pause
    exit /b 1
)
uv run --python 3.12 python common/tools/launcher.py --detach
if errorlevel 1 pause
