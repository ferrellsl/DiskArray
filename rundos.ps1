# Runs one DOS program from build16 in DOSBox-X and waits for it to finish.
# rundos.ps1 <program> <output.txt> [cycles] [cputype] [timeout seconds]
param([string]$Exe, [string]$Out, [string]$Args2 = "", [string]$Cycles = 'max', [string]$Cpu = 'auto', [int]$Timeout = 500)
$dir = Join-Path $PSScriptRoot 'build16'
$conf = Join-Path $dir "$Exe.conf"
@"
[sdl]
output=surface
[dosbox]
machine=svga_s3
memsize=4
[cpu]
core=normal
cputype=$Cpu
cycles=$Cycles
[autoexec]
mount c "$dir"
c:
$Exe $Args2 > $Out
exit
"@ | Set-Content -Encoding ascii $conf
Remove-Item (Join-Path $dir $Out) -ErrorAction SilentlyContinue
$p = Start-Process 'C:\DOSBox-X\dosbox-x.exe' -ArgumentList '-conf', "`"$conf`"", '-nolog', '-fastlaunch' -WindowStyle Minimized -PassThru
if (-not $p.WaitForExit($Timeout * 1000)) { Stop-Process -Id $p.Id -Force; "TIMED OUT after $Timeout s" }
Get-Content (Join-Path $dir $Out) -ErrorAction SilentlyContinue
