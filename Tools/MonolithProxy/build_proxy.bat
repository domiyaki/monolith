@echo off
REM Compatibility entrypoint; build.bat owns discovery, output paths and errors.
call "%~dp0build.bat" %*
exit /b %errorlevel%
