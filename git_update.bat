@echo off
setlocal

cd /d "%~dp0"

:menu
echo.
echo =========================================
echo  AhuraRTOS submodule update
echo =========================================
echo.
echo   1. Update submodules only
echo   2. Update submodules, commit and push
echo   Q. Quit
echo.
set "choice="
set /p choice=Select [1/2/Q]:

if /i "%choice%"=="1" goto update_only
if /i "%choice%"=="2" goto update_commit
if /i "%choice%"=="q" goto quit

echo Not a valid choice.
goto menu

:update_only
call :update
if errorlevel 1 goto failed
echo.
echo =========================================
echo  Working tree
echo =========================================
call git status --short
echo.
echo Submodules updated. Nothing committed.
goto done

:update_commit
call :update
if errorlevel 1 goto failed

echo.
echo =========================================
echo  Staging changes...
echo =========================================
call git add -A
if errorlevel 1 goto failed

call git diff --cached --quiet
if not errorlevel 1 (
    echo Nothing to commit - the submodules were already up to date.
    goto done
)

call git status --short
echo.

call git commit -m "Automated update of all submodules"
if errorlevel 1 goto failed

echo.
echo =========================================
echo  Pushing to origin/main...
echo =========================================
call git push origin main
if errorlevel 1 goto failed

echo.
echo Submodules updated, committed and pushed.
goto done

:update
echo.
echo =========================================
echo  Updating all submodules...
echo =========================================
call git submodule update --init --remote --merge
exit /b %errorlevel%

:failed
echo.
echo A git command failed. Nothing further was attempted.
goto end

:quit
echo Cancelled.
goto end

:done
echo.
echo Done!

:end
endlocal
pause
