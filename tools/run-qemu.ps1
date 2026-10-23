# Boots build\esp in QEMU + OVMF with a window; serial output goes to this terminal.
# Usage: .\tools\run-qemu.ps1 [--cmdline "text"] [--mem 1G] [--net] [--video 1280x800]
python "$PSScriptRoot\qemu.py" run @args
exit $LASTEXITCODE
