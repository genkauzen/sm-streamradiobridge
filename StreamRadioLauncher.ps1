# Stream Radio Core unified launcher for Windows PowerShell 5.1+
# It detects Steam/Scrap Mechanic on any drive, installs the mod, checks the
# native bridge, optionally updates from GitHub, then starts the game and DLL.

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$BridgeDir = Join-Path $Root 'Native\StreamRadioBridge'
$ModSource = Join-Path $Root 'StreamRadio_1.0.5'
$ConfigDir = Join-Path $env:APPDATA 'StreamRadio'
$ConfigPath = Join-Path $ConfigDir 'launcher.json'
$RepoApi = 'https://api.github.com/repos/genkauzen/sm-streamradiobridge/releases/latest'
$RepoManifest = 'https://raw.githubusercontent.com/genkauzen/sm-streamradiobridge/main/manifest.json'

function Get-Config {
    $defaults = [ordered]@{
        AutoInstall = $true
        AutoUpdate = $true
        StartBridge = $true
        KeepLog = $false
        Profile = ''
    }
    try {
        if (Test-Path -LiteralPath $ConfigPath) {
            $saved = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
            foreach ($key in $defaults.Keys) {
                if ($null -ne $saved.$key) { $defaults[$key] = $saved.$key }
            }
        }
    } catch { }
    return [pscustomobject]$defaults
}

function Save-Config([object]$config) {
    if (-not (Test-Path -LiteralPath $ConfigDir)) {
        New-Item -ItemType Directory -Path $ConfigDir -Force | Out-Null
    }
    $config | ConvertTo-Json | Set-Content -LiteralPath $ConfigPath -Encoding UTF8
}

function Get-SteamExe {
    $keys = @(
        'HKCU:\Software\Valve\Steam',
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam'
    )
    foreach ($key in $keys) {
        try {
            $value = (Get-ItemProperty -LiteralPath $key -Name SteamExe -ErrorAction Stop).SteamExe
            if ($value -and (Test-Path -LiteralPath $value)) { return $value }
        } catch { }
    }
    return $null
}

function Get-ScrapProfilePaths {
    $userRoot = Join-Path $env:APPDATA 'Axolot Games\Scrap Mechanic\User'
    if (-not (Test-Path -LiteralPath $userRoot)) { return @() }
    return @(Get-ChildItem -LiteralPath $userRoot -Directory -Filter 'User_*' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -ExpandProperty FullName)
}

function Get-SelectedProfile([object]$config) {
    $paths = Get-ScrapProfilePaths
    if ($config.Profile -and ($paths -contains $config.Profile)) { return $config.Profile }
    return $paths | Select-Object -First 1
}

function Get-LocalVersion {
    $versionFile = Join-Path $Root 'VERSION.txt'
    if (Test-Path -LiteralPath $versionFile) {
        $line = Get-Content -LiteralPath $versionFile | Where-Object { $_ -match '^version\s*=' } | Select-Object -First 1
        if ($line) { return ($line -replace '^version\s*=\s*', '').Trim() }
    }
    return 'unknown'
}

function Get-RequiredChecks([object]$config) {
    $profile = Get-SelectedProfile $config
    $checks = @()
    $checks += [pscustomobject]@{ Name = 'Мод StreamRadio_1.0.5'; Ok = (Test-Path (Join-Path $ModSource 'description.json')); Detail = $ModSource }
    $checks += [pscustomobject]@{ Name = 'StreamRadioBridge.dll'; Ok = (Test-Path (Join-Path $BridgeDir 'StreamRadioBridge.dll')); Detail = $BridgeDir }
    $checks += [pscustomobject]@{ Name = 'StreamRadioBridgeInject.exe'; Ok = (Test-Path (Join-Path $BridgeDir 'StreamRadioBridgeInject.exe')); Detail = $BridgeDir }
    $checks += [pscustomobject]@{ Name = 'yt-dlp.exe'; Ok = (Test-Path (Join-Path $BridgeDir 'yt-dlp.exe')); Detail = $BridgeDir }
    $checks += [pscustomobject]@{ Name = 'ffmpeg.exe'; Ok = (Test-Path (Join-Path $BridgeDir 'ffmpeg.exe')); Detail = $BridgeDir }
    $profileDetail = if ($profile) { $profile } else { 'не найден' }
    $checks += [pscustomobject]@{ Name = 'Профиль Scrap Mechanic'; Ok = [bool]$profile; Detail = $profileDetail }
    return $checks
}

function Write-Log([string]$message, [switch]$IsError) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $message
    if ($script:LogBox) {
        $script:LogBox.AppendText($line + [Environment]::NewLine)
        $script:LogBox.SelectionStart = $script:LogBox.TextLength
        $script:LogBox.ScrollToCaret()
    }
    if ($script:StatusLabel) { $script:StatusLabel.Text = $message }
    if ($IsError) { Write-Error $message -ErrorAction Continue } else { Write-Host $line }
}

function Install-Mod([object]$config) {
    $profile = Get-SelectedProfile $config
    if (-not $profile) { throw 'Профиль Scrap Mechanic не найден. Запустите игру один раз и повторите проверку.' }
    if (-not (Test-Path (Join-Path $ModSource 'description.json'))) { throw "Не найден $ModSource" }
    $destination = Join-Path $profile 'Mods\StreamRadio_1.0.5'
    if (Get-Process -Name ScrapMechanic -ErrorAction SilentlyContinue) { throw 'Сначала закройте Scrap Mechanic: файлы мода заняты игрой.' }
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $cache = Join-Path $destination 'Cache'
    if (Test-Path $cache) { Remove-Item -LiteralPath $cache -Recurse -Force }
    $nested = Join-Path $destination 'StreamRadio_1.0.5'
    if (Test-Path $nested) { Remove-Item -LiteralPath $nested -Recurse -Force }
    & robocopy $ModSource $destination /E /COPY:DAT /R:2 /W:1 /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Копирование мода завершилось с кодом robocopy $LASTEXITCODE" }
    $config.Profile = $profile
    Save-Config $config
    Write-Log "Мод установлен: $destination"
}

function Get-RemoteManifest {
    $headers = @{ 'User-Agent' = 'StreamRadioLauncher/1.0' }
    try {
        $release = Invoke-RestMethod -Uri $RepoApi -Headers $headers -TimeoutSec 12
        $asset = @($release.assets | Where-Object { $_.name -match '\.zip$' } | Select-Object -First 1)
        if ($asset) {
            return [pscustomobject]@{ Version = [string]$release.tag_name; Url = [string]$asset.browser_download_url; Sha256 = $null; Source = 'GitHub release' }
        }
    } catch { }
    try {
        $manifest = Invoke-RestMethod -Uri $RepoManifest -Headers $headers -TimeoutSec 12
        return [pscustomobject]@{ Version = [string]$manifest.version; Url = [string]$manifest.downloadUrl; Sha256 = [string]$manifest.sha256; Source = 'GitHub manifest' }
    } catch {
        throw 'GitHub недоступен или репозиторий ещё не опубликован.'
    }
}

function Update-FromGitHub([object]$config, [switch]$Silent) {
    $remote = Get-RemoteManifest
    $local = Get-LocalVersion
    $remoteVersion = ([string]$remote.Version) -replace '^v', ''
    if ($remoteVersion -and $remoteVersion -eq $local) {
        if (-not $Silent) { Write-Log "Установлена актуальная версия $local" }
        return $false
    }
    if (-not $remote.Url) { throw 'В манифесте GitHub нет ссылки на ZIP-пакет.' }
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('StreamRadioUpdate_' + [guid]::NewGuid().ToString('N'))
    $zip = Join-Path $tempRoot 'package.zip'
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
    try {
        Write-Log "Загрузка обновления $remoteVersion..."
        Invoke-WebRequest -Uri $remote.Url -OutFile $zip -Headers @{ 'User-Agent' = 'StreamRadioLauncher/1.0' } -UseBasicParsing -TimeoutSec 120
        if ($remote.Sha256) {
            $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
            if ($hash -ne $remote.Sha256.ToUpperInvariant()) { throw 'SHA-256 ZIP-пакета не совпадает с манифестом.' }
        }
        $extract = Join-Path $tempRoot 'extract'
        Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
        $candidate = Get-ChildItem -LiteralPath $extract -Directory -Recurse -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName 'StreamRadio_1.0.5\description.json') } | Select-Object -First 1
        if (-not $candidate) {
            $candidate = [pscustomobject]@{ FullName = $extract }
        }
        $payload = if (Test-Path (Join-Path $candidate.FullName 'StreamRadio_1.0.5')) { $candidate.FullName } else { $extract }
        foreach ($name in @('StreamRadio_1.0.5','Native','Install_StreamRadio.bat','Start_StreamRadioBridge.bat','VERSION.txt','manifest.json')) {
            $from = Join-Path $payload $name
            if (Test-Path $from) { Copy-Item -LiteralPath $from -Destination $Root -Recurse -Force }
        }
        Write-Log "Обновление установлено: $remoteVersion"
        return $true
    } finally {
        if (Test-Path $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

function Start-GameAndBridge {
    $dll = Join-Path $BridgeDir 'StreamRadioBridge.dll'
    $injector = Join-Path $BridgeDir 'StreamRadioBridgeInject.exe'
    if (-not (Test-Path $dll) -or -not (Test-Path $injector)) { throw 'Не найден DLL или инжектор bridge.' }
    $game = Get-Process -Name ScrapMechanic -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $game) {
        $steam = Get-SteamExe
        if ($steam) {
            Start-Process -FilePath $steam -ArgumentList '-applaunch 387990'
            Write-Log "Scrap Mechanic запускается через Steam: $steam"
        } else {
            Start-Process 'steam://rungameid/387990'
            Write-Log 'Steam.exe не найден в реестре; запущен Steam URI.'
        }
        Write-Log 'Жду процесс ScrapMechanic.exe (до 120 секунд)...'
        for ($second = 1; $second -le 120; $second++) {
            Start-Sleep -Seconds 1
            $game = Get-Process -Name ScrapMechanic -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($game) { break }
            if (($second % 10) -eq 0) { Write-Log "Игра ещё запускается... $second/120 сек." }
        }
    } else {
        Write-Log "Scrap Mechanic уже запущен (PID $($game.Id))."
    }
    if (-not $game) {
        throw 'ScrapMechanic.exe не появился за 120 секунд. Запустите игру через Steam и повторите START.'
    }
    $gamePath = $null
    try { $gamePath = $game.Path } catch { }
    if ($gamePath) { Write-Log "Игра найдена: PID $($game.Id), $gamePath" }
    Write-Log 'Запускаю инжектор и жду его результат...'
    $tempBase = Join-Path ([IO.Path]::GetTempPath()) ('StreamRadioInjector_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tempBase -Force | Out-Null
    $stdout = Join-Path $tempBase 'stdout.log'
    $stderr = Join-Path $tempBase 'stderr.log'
    try {
        $injectProcess = Start-Process -FilePath $injector -ArgumentList @($dll) -WorkingDirectory $BridgeDir `
            -PassThru -Wait -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        foreach ($line in @(Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue)) {
            if ($line.Trim()) { Write-Log "Инжектор: $line" }
        }
        foreach ($line in @(Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue)) {
            if ($line.Trim()) { Write-Log "Инжектор ERROR: $line" -IsError }
        }
        switch ($injectProcess.ExitCode) {
            0 { Write-Log 'Bridge успешно подключён к игре.' }
            2 { throw 'Инжектор не нашёл ScrapMechanic.exe. Игра могла закрыться во время запуска.' }
            3 { throw 'Инжекция DLL не удалась. Запустите лаунчер от имени администратора и проверьте антивирус.' }
            4 { throw 'В игре уже загружен другой StreamRadioBridge. Полностью закройте Scrap Mechanic и запустите START заново.' }
            default { throw "Инжектор завершился с кодом $($injectProcess.ExitCode)." }
        }
    } finally {
        if (Test-Path -LiteralPath $tempBase) { Remove-Item -LiteralPath $tempBase -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# Load persisted settings before the optional headless check and GUI.
$config = Get-Config

# Headless validation hook used by packaging/diagnostic scripts.
if ($env:STREAMRADIO_CHECKONLY -eq '1') {
    $failed = $false
    foreach ($item in (Get-RequiredChecks $config)) {
        $mark = '[!!]'
        if ($item.Ok) { $mark = '[OK]' } else { $failed = $true }
        Write-Log ("{0} {1} — {2}" -f $mark, $item.Name, $item.Detail)
    }
    if ($failed) { exit 1 } else { exit 0 }
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Stream Radio Core — Launcher'
$form.StartPosition = 'CenterScreen'
$form.Size = New-Object System.Drawing.Size(800, 570)
$form.MinimumSize = New-Object System.Drawing.Size(720, 500)
$form.BackColor = [System.Drawing.Color]::FromArgb(24, 31, 38)
$form.ForeColor = [System.Drawing.Color]::White

$title = New-Object System.Windows.Forms.Label
$title.Text = 'STREAM RADIO CORE  •  1.0.5'
$title.Font = New-Object System.Drawing.Font('Segoe UI', 16, [System.Drawing.FontStyle]::Bold)
$title.Location = New-Object System.Drawing.Point(22, 18)
$title.AutoSize = $true
$form.Controls.Add($title)

$script:StatusLabel = New-Object System.Windows.Forms.Label
$script:StatusLabel.Text = 'Готов к проверке'
$script:StatusLabel.Location = New-Object System.Drawing.Point(25, 55)
$script:StatusLabel.AutoSize = $true
$script:StatusLabel.ForeColor = [System.Drawing.Color]::LightSkyBlue
$form.Controls.Add($script:StatusLabel)

$buttons = @{}
function Add-ActionButton([string]$name, [string]$text, [int]$x, [int]$width) {
    $button = New-Object System.Windows.Forms.Button
    $button.Name = $name; $button.Text = $text
    $button.Location = New-Object System.Drawing.Point($x, 83)
    $button.Size = New-Object System.Drawing.Size($width, 34)
    $button.FlatStyle = 'Flat'; $button.FlatAppearance.BorderColor = [System.Drawing.Color]::SteelBlue
    $form.Controls.Add($button); $buttons[$name] = $button; return $button
}
$checkButton = Add-ActionButton 'Check' 'Проверить' 22 112
$installButton = Add-ActionButton 'Install' 'Установить / обновить' 142 166
$updateButton = Add-ActionButton 'Update' 'Проверить GitHub' 316 142
$startButton = Add-ActionButton 'Start' 'START' 468 100
$settingsButton = Add-ActionButton 'Settings' 'Настройки' 578 100
$exitButton = Add-ActionButton 'Exit' 'Выход' 688 72

$profileLabel = New-Object System.Windows.Forms.Label
$profileLabel.Text = 'Профиль:'; $profileLabel.Location = New-Object System.Drawing.Point(25, 132); $profileLabel.AutoSize = $true
$form.Controls.Add($profileLabel)
$profileBox = New-Object System.Windows.Forms.ComboBox
$profileBox.Location = New-Object System.Drawing.Point(92, 128); $profileBox.Size = New-Object System.Drawing.Size(520, 25)
$profileBox.DropDownStyle = 'DropDownList'
foreach ($path in (Get-ScrapProfilePaths)) { [void]$profileBox.Items.Add($path) }
$selected = Get-SelectedProfile $config
if ($selected -and $profileBox.Items.Contains($selected)) { $profileBox.SelectedItem = $selected }
$form.Controls.Add($profileBox)

$script:LogBox = New-Object System.Windows.Forms.RichTextBox
$script:LogBox.Location = New-Object System.Drawing.Point(22, 166)
$script:LogBox.Size = New-Object System.Drawing.Size(740, 270)
$script:LogBox.ReadOnly = $true; $script:LogBox.BackColor = [System.Drawing.Color]::FromArgb(12, 17, 22)
$script:LogBox.ForeColor = [System.Drawing.Color]::Gainsboro; $script:LogBox.Font = New-Object System.Drawing.Font('Consolas', 9)
$form.Controls.Add($script:LogBox)

$settings = New-Object System.Windows.Forms.GroupBox
$settings.Text = 'Настройки запуска'; $settings.Location = New-Object System.Drawing.Point(22, 448); $settings.Size = New-Object System.Drawing.Size(740, 62)
$settings.ForeColor = [System.Drawing.Color]::White
$form.Controls.Add($settings)
$autoInstall = New-Object System.Windows.Forms.CheckBox; $autoInstall.Text = 'Устанавливать мод при START'; $autoInstall.Location = New-Object System.Drawing.Point(14, 25); $autoInstall.AutoSize = $true; $autoInstall.Checked = [bool]$config.AutoInstall; $settings.Controls.Add($autoInstall)
$autoUpdate = New-Object System.Windows.Forms.CheckBox; $autoUpdate.Text = 'Автообновление GitHub'; $autoUpdate.Location = New-Object System.Drawing.Point(245, 25); $autoUpdate.AutoSize = $true; $autoUpdate.Checked = [bool]$config.AutoUpdate; $settings.Controls.Add($autoUpdate)
$startBridge = New-Object System.Windows.Forms.CheckBox; $startBridge.Text = 'Запускать bridge'; $startBridge.Location = New-Object System.Drawing.Point(445, 25); $startBridge.AutoSize = $true; $startBridge.Checked = [bool]$config.StartBridge; $settings.Controls.Add($startBridge)

function Read-UiConfig {
    $config.AutoInstall = $autoInstall.Checked; $config.AutoUpdate = $autoUpdate.Checked; $config.StartBridge = $startBridge.Checked
    if ($profileBox.SelectedItem) { $config.Profile = [string]$profileBox.SelectedItem }
    Save-Config $config
}

function Refresh-Checks {
    Read-UiConfig
    $ok = $true
    foreach ($item in (Get-RequiredChecks $config)) {
        $mark = if ($item.Ok) { '[OK]' } else { '[!!]'; $ok = $false }
        Write-Log "$mark $($item.Name) — $($item.Detail)"
    }
    if ($ok) { Write-Log "Локальная версия: $(Get-LocalVersion)" } else { Write-Log 'Есть проблемы. Нажмите «Установить / обновить» или START.' }
    return $ok
}

$checkButton.Add_Click({ try { [void](Refresh-Checks) } catch { Write-Log $_.Exception.Message -IsError } })
$installButton.Add_Click({ try { Read-UiConfig; Install-Mod $config; [void](Refresh-Checks) } catch { Write-Log $_.Exception.Message -IsError } })
$updateButton.Add_Click({ try { Read-UiConfig; [void](Update-FromGitHub $config) } catch { Write-Log $_.Exception.Message -IsError } })
$settingsButton.Add_Click({ $settings.Visible = -not $settings.Visible })
$exitButton.Add_Click({ $form.Close() })
$startButton.Add_Click({
    try {
        Read-UiConfig
        if ($config.AutoUpdate) { try { [void](Update-FromGitHub $config -Silent) } catch { Write-Log "Автообновление пропущено: $($_.Exception.Message)" } }
        if ($config.AutoInstall) { Install-Mod $config }
        [void](Refresh-Checks)
        if ($config.StartBridge) { Start-GameAndBridge } else { Write-Log 'Запуск bridge отключён в настройках.' }
    } catch { Write-Log $_.Exception.Message -IsError }
})
$form.Add_Shown({ Write-Log "Локальная версия: $(Get-LocalVersion)"; [void](Refresh-Checks) })
$form.Add_FormClosing({ Read-UiConfig })
[void]$form.ShowDialog()

