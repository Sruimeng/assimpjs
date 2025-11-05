pushd %~dp0\..

call tools\build_wasm_win_release.bat all || goto :error
echo Build and Distribution Succeeded.

popd
exit /b 0

:error
echo Distribution Failed with Error %errorlevel%.
popd
exit /b 1
