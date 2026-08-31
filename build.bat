@echo off
setlocal
where ufbt >nul 2>nul
if not errorlevel 1 (
  ufbt
  exit /b %errorlevel%
)

py -c "import ufbt" >nul 2>nul
if errorlevel 1 (
  echo uFBT was not found.
  echo Install it with: py -m pip install --user ufbt
  exit /b 1
)

py -m ufbt
