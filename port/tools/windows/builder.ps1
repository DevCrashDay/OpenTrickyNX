# SPDX-License-Identifier: GPL-3.0-only
# OpenTrickyNX Builder: a window that sets up the tools, builds "SSX Tricky.nro"
# from your own SSX Tricky (USA) Xbox disc image and puts it on the SD card.
# Started by "OpenTrickyNX Builder.bat" in the top folder.
#
# Without -Task it shows the window. Each button runs this same script again
# with -Task (setup, build, sdcard, nsp) in the background, writing to a log
# file the window shows as it grows. Windows PowerShell 5.1, nothing to install.
param(
    [string]$Task = "",
    [string]$Iso = "",
    [string]$Renderer = "vulkan",
    [string]$Drive = "",
    [string]$Keys = "",
    [string]$Picture = ""
)

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$Work = Join-Path $Root "build\windows"
$Downloads = Join-Path $Work "downloads"
$MsysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "C:\msys64" }
$Bash = Join-Path $MsysRoot "usr\bin\bash.exe"
$Python = Join-Path $MsysRoot "ucrt64\bin\python.exe"
# The Vulkan files: the builder's own download, or one already unpacked by hand
# (NVK_SDK, or a portlibs\switch folder in the top folder).
$NvkDir = Join-Path $Work "nvk-sdk\portlibs\switch"
foreach ($d in @($env:NVK_SDK, (Join-Path $Root "portlibs\switch"))) {
    if ($d -and (Test-Path (Join-Path $d "lib\libvulkan.a"))) { $NvkDir = $d; break }
}
$DockerBin = Join-Path $env:ProgramFiles "Docker\Docker\resources\bin"
$DockerDesktop = Join-Path $env:ProgramFiles "Docker\Docker\Docker Desktop.exe"
$Settings = Join-Path $Work "settings.json"
$Dist = Join-Path $Root "dist\switch"

$MsysUrl = "https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-base-x86_64-latest.sfx.exe"
$DockerUrl = "https://desktop.docker.com/win/main/amd64/Docker%20Desktop%20Installer.exe"
$NvkUrl = "https://github.com/DevCrashDay/OpenTrickyNX/releases/download/nvk-sdk/mesa-switch-nvk-sdk.zip"
$Packages = "mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow mingw-w64-ucrt-x86_64-binutils git"

New-Item -ItemType Directory -Force -Path $Work, $Downloads | Out-Null
if (Test-Path $DockerBin) { $env:PATH = "$DockerBin;$env:PATH" }

function Has-File([string]$path) { $path -and (Test-Path -LiteralPath $path -PathType Leaf) }
function Test-Msys { (Test-Path $Bash) -and (Test-Path $Python) }
function Test-DockerInstalled { [bool](Get-Command docker.exe -ErrorAction SilentlyContinue) }
function Test-Nvk { Test-Path (Join-Path $NvkDir "lib\libvulkan.a") }
function Test-Nro { Test-Path (Join-Path $Dist "opentricky\SSX Tricky.nro") }

# ---------------------------------------------------------------------------
# The background tasks. They print to the log; the last line says how it ended.

function Say([string]$text) { [Console]::Out.WriteLine($text) }
function Fail([string]$text) { Say ""; Say "PROBLEM: $text"; exit 1 }

# Runs a program, its output (both streams) into the log; returns the exit code.
function Run([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = "Continue"
    & $exe @arguments 2>&1 | ForEach-Object { Say "$_" }
    return $LASTEXITCODE
}

# Runs a bash script in MSYS2's UCRT64 shell, from the top folder. The script
# goes through a file: Windows PowerShell mangles quotes in program arguments.
function Run-Bash([string]$script) {
    $env:MSYSTEM = "UCRT64"
    $env:CHERE_INVOKING = "1"
    $env:MSYS2_PATH_TYPE = "inherit"      # keeps docker.exe reachable
    $env:OT_ROOT = $Root
    $file = Join-Path $Work "step.sh"
    $text = "set -e`ncd `"`$(cygpath -u `"`$OT_ROOT`")`"`n" + $script + "`n"
    [IO.File]::WriteAllText($file, $text.Replace("`r", ""))
    return (Run $Bash @("-l", $file.Replace("\", "/")))
}

# Returns $false when the file isn't there (404); other failures stop the task.
function Download([string]$url, [string]$file, [switch]$Optional) {
    Say "   downloading $url"
    $ProgressPreference = "SilentlyContinue"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    try { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile "$file.part" }
    catch {
        $code = 0
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        if ($Optional -and $code -eq 404) { return $false }
        Fail "the download failed ($($_.Exception.Message)). Check the internet connection and try again."
    }
    Move-Item -Force "$file.part" $file
    return $true
}

function Test-DockerRunning {
    $ErrorActionPreference = "Continue"
    & docker.exe info *> $null
    return ($LASTEXITCODE -eq 0)
}

function Start-Docker {
    if (Test-DockerRunning) { return }
    Say "   starting Docker Desktop (it can take a few minutes)"
    if (Test-Path $DockerDesktop) { Start-Process $DockerDesktop }
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Seconds 5
        if (Test-DockerRunning) { Say "   Docker is running"; return }
    }
    Fail "Docker Desktop did not start. Open it from the Start menu, accept its terms (you can skip signing in), wait until it says it is running, then press the button again."
}

function Task-Setup {
    Say "== [1/4] MSYS2 (Python and tools for translating the game)"
    if (-not (Test-Path $Bash)) {
        $sfx = Join-Path $Downloads "msys2-base.sfx.exe"
        Download $MsysUrl $sfx | Out-Null
        $parent = Split-Path $MsysRoot -Parent
        if ((Split-Path $MsysRoot -Leaf) -ne "msys64") { Fail "MSYS2_ROOT has to end in \msys64 for the automatic install." }
        Say "   unpacking to $MsysRoot"
        if ((Run $sfx @("-y", "-o$parent")) -ne 0 -or -not (Test-Path $Bash)) { Fail "MSYS2 could not be unpacked to $MsysRoot." }
        Say "   first start of MSYS2"
        Run-Bash "true" | Out-Null
    } else {
        Say "   MSYS2 is in $MsysRoot"
    }
    if (-not (Test-Path $Python) -or (Run-Bash "`"`$MINGW_PREFIX/bin/python.exe`" -c 'import capstone, numpy, PIL' && command -v objdump >/dev/null && command -v git >/dev/null") -ne 0) {
        Say "   installing Python, capstone, numpy, pillow, binutils and git"
        # twice: the first run may stop after updating pacman itself
        Run-Bash "pacman -Syu --noconfirm || true" | Out-Null
        Run-Bash "pacman -Syu --noconfirm || true" | Out-Null
        if ((Run-Bash "pacman -S --needed --noconfirm $Packages") -ne 0) { Fail "the MSYS2 packages could not be installed." }
    }
    Say "   MSYS2 is ready"

    Say "== [2/4] Docker Desktop (the Switch compiler runs in it)"
    if (-not (Test-DockerInstalled)) {
        $ErrorActionPreference = "Continue"
        $wsl = $false
        if (Get-Command wsl.exe -ErrorAction SilentlyContinue) { & wsl.exe --status *> $null; $wsl = ($LASTEXITCODE -eq 0) }
        if (-not $wsl) {
            Say "   turning on WSL, which Docker needs (Windows asks for permission)"
            Start-Process wsl.exe -ArgumentList "--install --no-distribution" -Verb RunAs -Wait | Out-Null
        }
        $installer = Join-Path $Downloads "Docker Desktop Installer.exe"
        Download $DockerUrl $installer | Out-Null
        Say "   installing Docker Desktop (Windows asks for permission)"
        $p = Start-Process $installer -ArgumentList "install --quiet --accept-license --backend=wsl-2" -Verb RunAs -Wait -PassThru
        if ($p.ExitCode -ne 0) { Fail "the Docker Desktop installer stopped with code $($p.ExitCode)." }
        Say ""
        Say "RESTART: Docker Desktop is installed. Restart the computer, then open OpenTrickyNX Builder again and press the button once more."
        exit 3
    }
    Start-Docker
    Say "   getting the Switch compiler (about 3 GB the first time)"
    if ((Run "docker.exe" @("pull", "devkitpro/devkita64")) -ne 0) { Fail "the devkitpro/devkita64 image could not be downloaded." }

    Say "== [3/4] Vulkan driver files (Mesa NVK)"
    if (-not (Test-Nvk)) {
        $zip = Join-Path $Downloads "mesa-switch-nvk-sdk.zip"
        if (Download $NvkUrl $zip -Optional) {
            $dir = Join-Path $Work "nvk-sdk"
            if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
            Expand-Archive -Path $zip -DestinationPath $dir
            if (-not (Test-Nvk)) { Fail "the download has no portlibs\switch\lib\libvulkan.a in it." }
        }
    }
    if (Test-Nvk) { Say "   the Vulkan files are in $NvkDir" }
    else { Say "   not published yet: build with OpenGL for now, or unpack the mesa-switch-nvk-sdk download so OpenTrickyNX\portlibs\switch\lib\libvulkan.a exists" }

    Say "== [4/4] done"
    if (Test-Nvk) { Say "OK: everything is installed." }
    else { Say "OK: everything is installed except the Vulkan files: choose OpenGL in step 2 for now." }
}

function Task-Build {
    if (-not (Has-File $Iso)) { Fail "pick your disc image first." }
    if (-not (Test-Msys)) { Fail "press Install first (MSYS2 is missing)." }
    if (-not (Test-DockerInstalled)) { Fail "press Install first (Docker Desktop is missing)." }
    Start-Docker
    # Each translated file takes a couple of GB to compile, and Docker gets half
    # the memory: more at once than that stalls the build.
    $gb = [Math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    $jobs = [Math]::Max(1, [Math]::Min([Environment]::ProcessorCount, [Math]::Floor($gb / 4)))
    $env:SWITCH_JOBS = "$jobs"
    Say "   $gb GB of memory: compiling $jobs files at once"
    if ($Renderer -eq "vulkan") {
        if (-not (Test-Nvk)) { Fail "press Install first (the Vulkan files are missing), or choose OpenGL." }
        $env:VULKAN = "1"
        $env:NVK_SDK = $NvkDir
    } else {
        $env:VULKAN = "0"
    }
    $env:OT_ISO = $Iso
    if ((Run-Bash "./build_switch.sh `"`$(cygpath -u `"`$OT_ISO`")`"") -ne 0) { Fail "the build stopped. The messages above say why." }
    Say "OK: dist\switch\opentricky\SSX Tricky.nro is built."
}

function Task-SdCard {
    if (-not (Test-Nro)) { Fail "build the game first." }
    if (-not $Drive -or -not (Test-Path "$Drive\")) { Fail "pick the SD card's drive." }
    $dest = "$Drive\switch\opentricky"
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Say "== [1/2] the game program"
    $src = Join-Path $Dist "opentricky"
    Copy-Item -Force (Join-Path $src "SSX Tricky.nro") $dest
    if (Test-Path (Join-Path $src "SSX Tricky.sym")) { Copy-Item -Force (Join-Path $src "SSX Tricky.sym") $dest }
    if (-not (Test-Path (Join-Path $dest "SSX Tricky env.txt"))) { Copy-Item (Join-Path $src "SSX Tricky env.txt") $dest }
    Say "   $dest\SSX Tricky.nro"
    Say "== [2/2] the game's files from your disc"
    $game = Join-Path $dest "game"
    if (Test-Path (Join-Path $game "default.xbe")) {
        Say "   already there ($game): kept"
    } else {
        if (-not (Has-File $Iso)) { Fail "pick your disc image first." }
        $tmp = "$game.part"
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        if ((Run $Python @((Join-Path $Root "port\tools\extract_iso.py"), "--all", $Iso, $tmp)) -ne 0) { Fail "the disc image could not be read." }
        Rename-Item $tmp "game"
    }
    Say "OK: the SD card is ready. On the Switch, hold R while starting any game, then pick SSX Tricky (OpenTricky) in the Homebrew Menu."
}

function Task-Nsp {
    if (-not (Has-File $Keys)) { Fail "pick your prod.keys file." }
    if (-not (Has-File $Picture)) { Fail "pick a picture for the icon." }
    if (-not (Test-Msys)) { Fail "press Install first." }
    Start-Docker
    $env:OT_KEYS = $Keys
    $env:OT_PIC = $Picture
    if ((Run-Bash "./build_switch_nsp.sh `"`$(cygpath -u `"`$OT_KEYS`")`" `"`$(cygpath -u `"`$OT_PIC`")`"") -ne 0) { Fail "the icon could not be made. The messages above say why." }
    Say "OK: dist\switch\SSX Tricky.nsp is made. Install it with DBI or Tinfoil; keep it to yourself (it is made with your console's keys)."
}

if ($Task) {
    $ErrorActionPreference = "Stop"
    try {
        switch ($Task) {
            "setup"  { Task-Setup }
            "build"  { Task-Build }
            "sdcard" { Task-SdCard }
            "nsp"    { Task-Nsp }
            default  { Fail "unknown task $Task" }
        }
    } catch {
        Fail $_.Exception.Message
    }
    exit 0
}

# ---------------------------------------------------------------------------
# The window.

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$saved = @{ iso = ""; renderer = "vulkan"; keys = ""; picture = "" }
if (Test-Path $Settings) {
    try { (Get-Content -Raw $Settings | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $saved[$_.Name] = $_.Value } } catch { }
}
function Save-Settings {
    $saved.iso = $isoBox.Text
    $saved.renderer = if ($glRadio.Checked) { "opengl" } else { "vulkan" }
    $saved.keys = $keysBox.Text
    $saved.picture = $picBox.Text
    try { $saved | ConvertTo-Json | Set-Content -Encoding UTF8 $Settings } catch { }
}

$font = New-Object Drawing.Font("Segoe UI", 10)
$bold = New-Object Drawing.Font("Segoe UI", 10, [Drawing.FontStyle]::Bold)
$form = New-Object Windows.Forms.Form
$form.Text = "OpenTrickyNX Builder"
$form.Font = $font
$form.ClientSize = New-Object Drawing.Size(720, 760)
$form.FormBorderStyle = "FixedSingle"
$form.MaximizeBox = $false
$form.StartPosition = "CenterScreen"

function Add-Control($parent, $control, [int]$x, [int]$y, [int]$w, [int]$h) {
    $control.Location = New-Object Drawing.Point($x, $y)
    $control.Size = New-Object Drawing.Size($w, $h)
    $parent.Controls.Add($control)
    return $control
}
function New-Label([string]$text) { $l = New-Object Windows.Forms.Label; $l.Text = $text; return $l }
function New-Button([string]$text) { $b = New-Object Windows.Forms.Button; $b.Text = $text; return $b }
function New-Group($text, [int]$y, [int]$h) {
    $g = New-Object Windows.Forms.GroupBox
    $g.Text = $text
    $g.Font = $bold
    return (Add-Control $form $g 12 $y 696 $h)
}
function Set-Plain($group) { foreach ($c in $group.Controls) { $c.Font = $font } }

# 1. Tools
$g1 = New-Group "1. Install the tools (once)" 8 112
$toolsLabel = Add-Control $g1 (New-Label "") 14 26 460 78
$setupButton = Add-Control $g1 (New-Button "Install / check") 500 36 180 44

# 2. Game
$g2 = New-Group "2. Your game" 128 112
Add-Control $g2 (New-Label "Disc image of your own SSX Tricky (USA) Xbox disc (.iso):") 14 26 600 22 | Out-Null
$isoBox = Add-Control $g2 (New-Object Windows.Forms.TextBox) 14 50 560 26
$isoButton = Add-Control $g2 (New-Button "Browse...") 584 48 96 30
$vkRadio = Add-Control $g2 (New-Object Windows.Forms.RadioButton) 14 80 250 26
$vkRadio.Text = "Vulkan (recommended, faster)"
$glRadio = Add-Control $g2 (New-Object Windows.Forms.RadioButton) 280 80 200 26
$glRadio.Text = "OpenGL"

# 3. Build
$g3 = New-Group "3. Build the game" 248 84
$buildButton = Add-Control $g3 (New-Button "Build SSX Tricky.nro") 14 30 220 40
$buildLabel = Add-Control $g3 (New-Label "The first build takes 20 to 40 minutes.") 250 38 430 24

# 4. SD card
$g4 = New-Group "4. Copy to the SD card" 340 84
Add-Control $g4 (New-Label "SD card drive:") 14 38 110 24 | Out-Null
$driveBox = Add-Control $g4 (New-Object Windows.Forms.ComboBox) 126 35 90 28
$driveBox.DropDownStyle = "DropDownList"
$refreshButton = Add-Control $g4 (New-Button "Refresh") 222 33 84 32
$sdButton = Add-Control $g4 (New-Button "Copy to SD card") 320 30 180 40
$sdLabel = Add-Control $g4 (New-Label "Copies the game and your disc's files.") 510 30 180 44

# Optional: home-menu icon
$g5 = New-Group "Optional: home-menu icon (.nsp, no holding R)" 432 118
Add-Control $g5 (New-Label "prod.keys:") 14 30 80 24 | Out-Null
$keysBox = Add-Control $g5 (New-Object Windows.Forms.TextBox) 96 28 380 26
$keysButton = Add-Control $g5 (New-Button "Browse...") 482 26 90 30
Add-Control $g5 (New-Label "Picture:") 14 66 80 24 | Out-Null
$picBox = Add-Control $g5 (New-Object Windows.Forms.TextBox) 96 64 380 26
$picButton = Add-Control $g5 (New-Button "Browse...") 482 62 90 30
$nspButton = Add-Control $g5 (New-Button "Make .nsp") 584 26 96 66

# Messages
$log = Add-Control $form (New-Object Windows.Forms.TextBox) 12 560 696 160
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = "Vertical"
$log.Font = New-Object Drawing.Font("Consolas", 9)
$status = Add-Control $form (New-Label "Ready.") 12 728 560 24
$folderButton = Add-Control $form (New-Button "Open output folder") 578 724 130 30

foreach ($g in $g1, $g2, $g3, $g4, $g5) { Set-Plain $g }

$isoBox.Text = $saved.iso
$keysBox.Text = $saved.keys
$picBox.Text = $saved.picture
if ($saved.renderer -eq "opengl") { $glRadio.Checked = $true } else { $vkRadio.Checked = $true }

function Mark([bool]$ok) { if ($ok) { "[OK]     " } else { "[needed] " } }
function Update-Tools {
    $toolsLabel.Text = (Mark (Test-Msys)) + "MSYS2 with Python`r`n" +
        (Mark (Test-DockerInstalled)) + "Docker Desktop`r`n" +
        (Mark (Test-Nvk)) + "Vulkan driver files (only for Vulkan)"
}
function Update-Drives {
    $driveBox.Items.Clear()
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if ($d.IsReady -and $d.DriveType -eq "Removable") { [void]$driveBox.Items.Add($d.Name.TrimEnd("\")) }
    }
    if ($driveBox.Items.Count -gt 0) { $driveBox.SelectedIndex = 0 }
}
Update-Tools
Update-Drives

function Pick-File([string]$filter, $box) {
    $d = New-Object Windows.Forms.OpenFileDialog
    $d.Filter = $filter
    if ($box.Text -and (Test-Path $box.Text)) { $d.InitialDirectory = Split-Path $box.Text -Parent }
    if ($d.ShowDialog() -eq "OK") { $box.Text = $d.FileName; Save-Settings }
}
$isoButton.Add_Click({ Pick-File "Xbox disc image (*.iso;*.xiso)|*.iso;*.xiso|All files|*.*" $isoBox })
$keysButton.Add_Click({ Pick-File "Key file (prod.keys)|*.keys|All files|*.*" $keysBox })
$picButton.Add_Click({ Pick-File "Pictures (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg|All files|*.*" $picBox })
$refreshButton.Add_Click({ Update-Drives })
$folderButton.Add_Click({ New-Item -ItemType Directory -Force -Path $Dist | Out-Null; Start-Process explorer.exe $Dist })

# One task at a time, in the background; a timer copies its log into the window.
$script:proc = $null
$script:logFile = ""
$script:logPos = 0
$buttons = @($setupButton, $buildButton, $sdButton, $nspButton)

function Start-Task([string]$name, [string]$title, [string[]]$extra) {
    if ($script:proc -and -not $script:proc.HasExited) { return }
    Save-Settings
    $script:logFile = Join-Path $Work "$name.log"
    Set-Content -Path $script:logFile -Value "" -Encoding ASCII
    $script:logPos = 0
    $log.Text = ""
    # parameter names bare, values quoted (paths may have spaces)
    $quoted = ($extra | ForEach-Object { if ($_ -like "-*") { $_ } else { "`"$_`"" } }) -join " "
    $ps = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    # cmd joins the two output streams into the log
    $line = "/c `"`"$ps`" -NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Task $name $quoted > `"$($script:logFile)`" 2>&1`""
    $script:proc = Start-Process cmd.exe -ArgumentList $line -WindowStyle Hidden -PassThru
    foreach ($b in $buttons) { $b.Enabled = $false }
    $status.Text = "$title ... (you can leave this window open and do something else)"
    $timer.Start()
}

function Read-Log {
    if (-not $script:logFile -or -not (Test-Path $script:logFile)) { return }
    try {
        $fs = [IO.File]::Open($script:logFile, "Open", "Read", "ReadWrite")
        $fs.Seek($script:logPos, "Begin") | Out-Null
        $reader = New-Object IO.StreamReader($fs)
        $new = $reader.ReadToEnd()
        $script:logPos = $fs.Position
        $reader.Close()
        if ($new) {
            $new = ($new -replace "`r(?!`n)", "`r`n") -replace "(?<!`r)`n", "`r`n"
            $log.AppendText($new)
            if ($log.TextLength -gt 200000) { $log.Text = $log.Text.Substring($log.TextLength - 100000) }
            $m = [regex]::Matches($new, "== \[(\d)/(\d)\] ([^\r\n]*)")
            if ($m.Count -gt 0) { $status.Text = "Step $($m[$m.Count-1].Groups[1].Value) of $($m[$m.Count-1].Groups[2].Value): $($m[$m.Count-1].Groups[3].Value)" }
        }
    } catch { }
}

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 500
$timer.Add_Tick({
    Read-Log
    if ($script:proc -and $script:proc.HasExited) {
        $timer.Stop()
        Read-Log
        foreach ($b in $buttons) { $b.Enabled = $true }
        Update-Tools
        $text = $log.Text
        $problem = [regex]::Match($text, "PROBLEM: ([^\r\n]*)")
        $restart = [regex]::Match($text, "RESTART: ([^\r\n]*)")
        $ok = [regex]::Match($text, "OK: ([^\r\n]*)")
        if ($restart.Success) {
            $status.Text = "Restart the computer."
            [void][Windows.Forms.MessageBox]::Show($restart.Groups[1].Value, "OpenTrickyNX Builder", "OK", "Information")
        } elseif ($problem.Success) {
            $status.Text = "Stopped: see the messages."
            [void][Windows.Forms.MessageBox]::Show($problem.Groups[1].Value + "`r`n`r`nThe full messages are in $($script:logFile)", "OpenTrickyNX Builder", "OK", "Warning")
        } elseif ($ok.Success) {
            $status.Text = $ok.Groups[1].Value
        } else {
            $status.Text = "Stopped. See the messages above."
        }
    }
})

$setupButton.Add_Click({ Start-Task "setup" "Installing the tools" @() })
$buildButton.Add_Click({
    if (-not (Has-File $isoBox.Text)) { [void][Windows.Forms.MessageBox]::Show("Pick your disc image first (step 2).", "OpenTrickyNX Builder"); return }
    $r = if ($glRadio.Checked) { "opengl" } else { "vulkan" }
    Start-Task "build" "Building" @("-Iso", $isoBox.Text, "-Renderer", $r)
})
$sdButton.Add_Click({
    if (-not $driveBox.SelectedItem) { [void][Windows.Forms.MessageBox]::Show("Put the SD card in the computer, press Refresh and pick its drive.", "OpenTrickyNX Builder"); return }
    Start-Task "sdcard" "Copying to the SD card" @("-Drive", $driveBox.SelectedItem, "-Iso", $isoBox.Text)
})
$nspButton.Add_Click({ Start-Task "nsp" "Making the home-menu icon" @("-Keys", $keysBox.Text, "-Picture", $picBox.Text) })
$form.Add_FormClosing({
    Save-Settings
    if ($script:proc -and -not $script:proc.HasExited) {
        $a = [Windows.Forms.MessageBox]::Show("A task is still running. Close anyway? It keeps running in the background.", "OpenTrickyNX Builder", "YesNo", "Question")
        if ($a -ne "Yes") { $_.Cancel = $true }
    }
})

if (-not (Test-Path (Join-Path $Root "port\tools\extract_iso.py"))) {
    [void][Windows.Forms.MessageBox]::Show("Start the builder from the OpenTrickyNX folder (unpack the whole download first).", "OpenTrickyNX Builder")
}
[void]$form.ShowDialog()
