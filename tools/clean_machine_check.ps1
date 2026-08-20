# clean_machine_check.ps1 - run inside a bare Windows container.
#
# **The half of the installer verification that is about the machine.** The
# packaging job installs the MSI on the runner that just built it, which covers
# everything about the installer and nothing about where it lands. This runs
# where a player's computer is: no Visual Studio, no CMake, no SDL, and no
# Visual C++ redistributable.
#
# It checks that first. A box with a compiler on it proves nothing, so if this
# turns out not to be a clean machine it fails rather than reporting a pass that
# means something weaker than it sounds.

$ErrorActionPreference = "Stop"

if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    throw "cl.exe is present - this is not a machine that has never had a toolchain"
}
if (Get-Command cmake -ErrorAction SilentlyContinue) {
    throw "cmake is present - this is not a machine that has never had a toolchain"
}
if (Test-Path "C:\Windows\System32\VCRUNTIME140.dll") {
    throw "the Visual C++ redistributable is present - this is not a clean machine"
}
Write-Host "clean: no compiler, no cmake, no redistributable"

$msi = Get-ChildItem "C:\dist\*.msi" | Select-Object -First 1
if (-not $msi) { throw "no MSI was mounted at C:\dist" }
Write-Host "installing $($msi.Name)"

$p = Start-Process msiexec.exe -Wait -PassThru `
     -ArgumentList "/i", "`"$($msi.FullName)`"", "/qn", "/norestart"
if ($p.ExitCode -ne 0) { throw "msiexec install failed with $($p.ExitCode)" }

# Found rather than assumed, the same as the check on the runner.
$exe = Get-ChildItem "C:\Program Files" -Recurse -Filter gearstick_cli.exe `
       -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $exe) { throw "the installer put nothing anywhere findable" }
$root = $exe.Directory.FullName
Write-Host "installed into $root"

if (-not (Test-Path (Join-Path $root "assets"))) {
    throw "the installer left the assets behind"
}

Set-Location $root
& .\gearstick_cli.exe selftest --verify
if ($LASTEXITCODE -ne 0) {
    throw "the golden replay did not re-race on a machine with no toolchain"
}

Write-Host "the installed game ran on a machine that has never had a toolchain"
