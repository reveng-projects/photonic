# Copyright (c) 2026 Vitaly Chipounov
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

<#
.SYNOPSIS
    Uninstall the previously installed Photonic driver and install the freshly
    built one.

.DESCRIPTION
    Removes any driver package whose original INF is photonic.inf, then adds and
    installs the newly built package. Also imports the test-signing certificate
    and restarts the photonic service.

.PARAMETER Inf
    Path to the built 64-bit photonic.inf to install. May be absolute or
    relative to this script. Defaults to "x64\Debug\photonic\photonic.inf".

.PARAMETER Inf32
    Path to the built 32-bit photonic.inf. May be absolute or relative to this
    script. Defaults to "Debug\photonic\photonic.inf". The script installs this
    package instead of -Inf when running on a 32-bit OS.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File reload.ps1
    powershell -ExecutionPolicy Bypass -File reload.ps1 -Inf Debug\photonic.inf

    Run from an elevated (Administrator) PowerShell.
#>

[CmdletBinding()]
param(
    [string]$Inf = "x64\Debug\photonic\photonic.inf",
    [string]$Inf32 = "Debug\photonic\photonic.inf"
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host "[reload] $msg" }

# --- Require administrator privileges ----------------------------------------
$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must be run as Administrator."
    exit 1
}

# --- Locate the built driver -------------------------------------------------
# Pick the INF matching the OS bitness. The driver must match the OS, not the
# PowerShell host, so check the OS rather than the current process.
if (-not [Environment]::Is64BitOperatingSystem) {
    Write-Step "32-bit OS detected, using the 32-bit driver package."
    $Inf = $Inf32
}
if (-not [System.IO.Path]::IsPathRooted($Inf)) {
    $Inf = Join-Path $PSScriptRoot $Inf
}
if (-not (Test-Path $Inf)) {
    Write-Error "Driver INF not found: `"$Inf`". Build the driver first, or pass -Inf."
    exit 1
}
$inf = (Resolve-Path $Inf).Path
$cer = Join-Path (Split-Path $inf -Parent) "photonic.cer"
Write-Step "Using driver package: `"$inf`""

# --- Trust the test-signing certificate --------------------------------------
if (Test-Path $cer) {
    Write-Step "Importing test certificate `"$cer`" ..."
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root            | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
}

# --- Stop the running driver service -----------------------------------------
Write-Step "Stopping the photonic service ..."
Stop-Service -Name photonic -Force -ErrorAction SilentlyContinue

# --- Uninstall any previously published photonic.inf packages ----------------
# pnputil prints each driver as a block: the "Published Name" (oemNN.inf) line
# precedes the "Original Name" (photonic.inf) line. Track the last published
# name seen and delete it when the matching original name is found. Matching on
# the value patterns (not the field labels) keeps this locale-independent.
Write-Step "Removing previously installed Photonic driver packages ..."
$published = $null
$found     = $false
foreach ($line in (pnputil /enum-drivers)) {
    if ($line -match '(oem\d+\.inf)') {
        $published = $matches[1]
    }
    if (($line -match 'photonic\.inf') -and $published) {
        Write-Step "  Deleting $published ..."
        pnputil /delete-driver $published /uninstall /force
        $published = $null
        $found     = $true
    }
}
if (-not $found) {
    Write-Step "  No existing Photonic driver package found."
}

# --- Install the updated driver ----------------------------------------------
# pnputil exit codes that are NOT failures:
#   0    - success
#   259  - ERROR_NO_MORE_ITEMS: package added, but no present device needed
#          updating (it was already up-to-date). The re-bind step below forces
#          the device onto the new package, so this is fine.
#   3010 - ERROR_SUCCESS_REBOOT_REQUIRED: installed, reboot needed.
Write-Step "Installing updated driver ..."
pnputil /add-driver "$inf" /install
if ($LASTEXITCODE -notin 0, 259, 3010) {
    Write-Error "Driver installation failed (pnputil exit code $LASTEXITCODE)."
    exit 1
}
if ($LASTEXITCODE -eq 3010) {
    Write-Step "  Driver installed; a reboot is required to complete installation."
}

# --- Re-bind the camera to the new driver (FORCED) ---------------------------
# A plain `pnputil /add-driver /install` does a *ranked* install: PnP keeps the
# driver already bound if it ranks equal-or-better. Our package is only
# TEST-signed, and the driver signature dominates PnP's rank -- so the
# Microsoft-signed inbox "1394 Desktop Camera" driver outranks our package even
# though we have an exact hardware-ID match. That is why pnputil reports the
# device "up-to-date" and the camera stays on the generic driver.
#
# To override rank we force our INF onto every matching device with
# UpdateDriverForPlugAndPlayDevices(INSTALLFLAG_FORCE) -- the same call
# `devcon update <inf> <hwid>` makes. P/Invoke it so no external tool is needed.
Write-Step "Force-binding camera device(s) to the new driver ..."

# The Win32 error must be captured inside the compiled helper, right after the
# P/Invoke returns. Reading Marshal.GetLastWin32Error() from script code is
# unreliable: PowerShell makes native calls of its own between the P/Invoke and
# the read, which clobber the stored error (typically yielding a bogus 0).
if (-not ("PnP.NewDev" -as [type])) {
    Add-Type -Namespace PnP -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
static extern bool UpdateDriverForPlugAndPlayDevices(
    IntPtr hwndParent, string HardwareId, string FullInfPath,
    uint InstallFlags, out bool bRebootRequired);

// Returns 0 on success, otherwise the Win32 error code.
public static uint Update(string hwid, string inf, uint flags, out bool reboot)
{
    if (UpdateDriverForPlugAndPlayDevices(IntPtr.Zero, hwid, inf, flags, out reboot))
    {
        return 0;
    }
    return (uint)Marshal.GetLastWin32Error();
}
'@
}

# Hardware IDs (one per supported camera) shared by all arch sections of
# photonic.inf: the real Vitana/PixeLINK camera and the Linux fake-fw-dev
# simulator. UpdateDriverForPlugAndPlayDevices binds every *present* device
# matching a given hardware ID, so this loop covers any number of cameras of
# each type, present or not.
$hwids = @(
    '1394\Vitana&PixeLINK(tm)_-_Photonic'
    '1394\Linux_Firewire&PixeLINK(tm)_-_Photonic'
)
$INSTALLFLAG_FORCE = 0x1
$ERROR_NO_SUCH_DEVINST = [uint32]0xE000020BL  # no matching device present
$ERROR_NO_MORE_ITEMS   = [uint32]259          # devices already on this driver
$bound = $false

foreach ($hwid in $hwids) {
    $reboot = $false
    $err = [PnP.NewDev]::Update($hwid, $inf, $INSTALLFLAG_FORCE, [ref]$reboot)

    switch ($err) {
        0 {
            Write-Step "  Camera(s) matching `"$hwid`" bound to the new driver."
            $bound = $true
            if ($reboot) { Write-Step "  A reboot is required to complete installation." }
        }
        $ERROR_NO_SUCH_DEVINST {
            Write-Step "  No device matching `"$hwid`" present; skipping."
        }
        $ERROR_NO_MORE_ITEMS {
            # Every matching device is already running this exact driver
            # package, typically because the pnputil install above updated it.
            Write-Step "  Camera(s) matching `"$hwid`" already on the new driver."
            $bound = $true
        }
        default {
            Write-Error ("Forced driver bind failed for `"$hwid`" (error 0x{0:X8})." -f $err)
            exit 1
        }
    }
}

if (-not $bound) {
    Write-Step "  No matching camera present; the new driver will bind on plug-in."
}

# --- Report the driver service state -----------------------------------------
# photonic is a PnP-associated kernel driver: it is demand-started when the
# device binds, so report its state rather than forcing it to start.
$svc = Get-Service -Name photonic -ErrorAction SilentlyContinue
if ($svc) {
    Write-Step "photonic service state: $($svc.Status)"
}
else {
    Write-Step "photonic service not present (no bound device yet)."
}

Write-Step "Done."
