# Puts the TouhouOS toolchain on PATH for the CURRENT PowerShell session only.
# Dot-source it:   . .\tools\dev-shell.ps1
# (tools/build.py and tools/qemu.py find the tools themselves; this is just for manual use.)

$ToolPaths = @(
    'C:\Program Files\LLVM\bin',
    'C:\Program Files\qemu',
    "$env:LOCALAPPDATA\bin\NASM"
)

foreach ($p in $ToolPaths) {
    if ((Test-Path $p) -and ($env:Path -notlike "*$p*")) {
        $env:Path = "$p;$env:Path"
    }
}

Write-Output 'TouhouOS dev environment:'
foreach ($tool in 'clang', 'lld-link', 'ld.lld', 'llvm-objdump', 'llvm-symbolizer', 'nasm', 'qemu-system-x86_64', 'python') {
    $cmd = Get-Command $tool -ErrorAction SilentlyContinue
    if ($cmd) {
        Write-Output "  $tool -> $($cmd.Source)"
    } else {
        Write-Output "  $tool -> NOT FOUND"
    }
}
