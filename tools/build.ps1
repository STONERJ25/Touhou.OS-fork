# Thin wrapper so the familiar command still works:  .\tools\build.ps1 [-v] [--clean] [targets...]
python "$PSScriptRoot\build.py" @args
exit $LASTEXITCODE
