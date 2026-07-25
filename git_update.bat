@echo off

echo =========================================
echo Updating all submodules...
echo =========================================

call git submodule update --remote --merge

echo =========================================
echo Saving changes to main repository...
echo =========================================

call git add .
call git commit -m "Automated update of all submodules"
call git push origin main

echo Done!
pause
