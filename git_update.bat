@echo off

echo =========================================
echo Updating all submodules...
echo =========================================

:: 2. Use 'call' so the script keeps running after Git finishes
call git submodule update --remote --merge

echo =========================================
echo Saving changes to main repository...
echo =========================================

:: 3. Stage, commit, and push the updated submodule pointers
call git add .
call git commit -m "Automated update of all submodules"
call git push origin main

echo Done!
pause