pushd %~dp0\..
call tools\build_wasm_win.bat %1
popd
