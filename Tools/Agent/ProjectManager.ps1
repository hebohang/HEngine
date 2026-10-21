param([string]$Project = '', [switch]$SmokeTest, [string]$CapturePath = '', [switch]$LayoutTest, [switch]$CreateLaunchTest,
      [ValidateSet('Open','Create')][string]$PreviewPage = 'Open', [string]$CreateLaunchTemplate = 'Blank')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Project.ps1')
$repositoryRoot = Split-Path (Split-Path $PSScriptRoot)
if ($CreateLaunchTest -and (!$SmokeTest -or !(Test-Path "$repositoryRoot/HEngine.distribution.json"))) {
    throw 'CreateLaunchTest requires SmokeTest in a binary SDK validation copy.'
}
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Windows.Forms
[xml]$xaml = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot 'ProjectManager.xaml')
$window = [Windows.Markup.XamlReader]::Load([Xml.XmlNodeReader]::new($xaml))
$workArea = [Windows.SystemParameters]::WorkArea
$window.MinWidth = [Math]::Min($window.MinWidth, $workArea.Width - 24)
$window.MinHeight = [Math]::Min($window.MinHeight, $workArea.Height - 24)
$window.Width = [Math]::Min($window.Width, $workArea.Width - 24)
$window.Height = [Math]::Min($window.Height, $workArea.Height - 24)
$controls = @{}
foreach ($node in $xaml.SelectNodes('//*[@Name]')) { $controls[$node.Name] = $window.FindName($node.Name) }
$controls.Engine.Text = "HEngine $(Get-HEEngineVersion $repositoryRoot) · $repositoryRoot"
$controls.Engine.ToolTip = $repositoryRoot
$iconPath = Join-Path $repositoryRoot 'Engine/Resources/Icons/EngineIcon.ico'
if (Test-Path -LiteralPath $iconPath) {
    $window.Icon = [Windows.Media.Imaging.BitmapFrame]::Create([Uri]::new($iconPath))
    $controls.BrandIcon.Source = $window.Icon
}
$templates = @(Get-HEProjectTemplates)
if ($CreateLaunchTest -and $CreateLaunchTemplate -notin $templates.Id) { throw 'Unknown create-launch validation template.' }
foreach ($template in $templates) {
    $item = [Windows.Controls.ComboBoxItem]::new()
    $item.Tag = $template.Id
    $item.Content = $template.DisplayName
    $item.ToolTip = $template.Description
    $null = $controls.Template.Items.Add($item)
}
$controls.Template.Add_SelectionChanged({
    $controls.TemplateDescription.Text = [string]$controls.Template.SelectedItem.ToolTip
})
$controls.Template.SelectedIndex = 0
$script:launch = $null
$script:launchLog = ''
$script:deferSmokeLaunch = [bool]$SmokeTest
$script:smokeLaunchRequests = @()
$preferencesPath = Join-Path $repositoryRoot 'out/agent/projects/manager.json'
if (Test-Path -LiteralPath $preferencesPath) {
    try { $controls.KeepOpen.IsChecked = (Read-HEWorkspace $preferencesPath).KeepOpen -eq $true }
    catch { $controls.Status.Text = '本机窗口偏好无法读取，已使用默认设置。' }
}
$controls.KeepOpen.Add_Click({
    if (-not $SmokeTest -and -not $LayoutTest) {
        try {
            $null = [IO.Directory]::CreateDirectory((Split-Path $preferencesPath))
            Write-HEWorkspace $preferencesPath ([ordered]@{SchemaVersion=1;KeepOpen=[bool]$controls.KeepOpen.IsChecked})
        } catch { Show-ProjectError $_.Exception.Message }
    }
})
function Set-LaunchBusy([bool]$busy) {
    foreach ($name in @('Open','Recent','Path','Browse','Create','Use','Reset','Forget','Location','LocationBrowse','Folder','Name','Template')) {
        $controls[$name].IsEnabled = -not $busy
    }
    $controls.Cancel.IsEnabled = $busy
}
function Read-LaunchState {
    if ($script:launchLog -and (Test-Path -LiteralPath ($script:launchLog + '.state.json'))) {
        return Read-HEWorkspace ($script:launchLog + '.state.json')
    }
    return $null
}
function Test-EditorReady($state) {
    if (-not $state) { return $false }
    if ($SmokeTest -and $state.State -eq 'validated') { return $true }
    if ($state.State -ne 'ready') { return $false }
    $process = Get-Process -Id $state.ProcessId -ErrorAction SilentlyContinue
    if (-not $process) { return $false }
    try { return $process.StartTime.ToUniversalTime().Ticks -eq $state.ProcessStartTicks -and
        $process.MainWindowHandle -ne [IntPtr]::Zero -and $process.Responding }
    finally { $process.Dispose() }
}
function Complete-ProjectLaunch([int]$exitCode, $state) {
    $ready = $exitCode -eq 0 -and (Test-EditorReady $state)
    if ($script:launch) { $script:launch.Dispose(); $script:launch = $null }
    Set-LaunchBusy $false
    if ($ready) {
        $controls.Status.Text = 'Editor 已加载项目并显示首帧。'
        if (-not $controls.KeepOpen.IsChecked) { $window.Close() }
    } else {
        $detail = if ($state.Message) { $state.Message } else { 'Editor 未确认就绪或已经退出。' }
        Show-ProjectError "打开失败：$detail`n日志：$script:launchLog"
    }
}
$controls.ViewLog.Add_Click({
    if ($script:launchLog -and (Test-Path -LiteralPath $script:launchLog)) {
        try { Start-Process -FilePath 'notepad.exe' -ArgumentList (Quote-ProcessArgument $script:launchLog) | Out-Null }
        catch { Show-ProjectError $_.Exception.Message }
    }
})

function Open-RecentProject($item) {
    if (-not $item -or -not $controls.Open.IsEnabled) { return }
    Select-Project ([string]$item.Tag)
    Open-Project
}
function Register-RecentActivation($item) {
    $item.Add_MouseDoubleClick({
        param($sender, $eventArgs)
        if ($eventArgs.ChangedButton -ne [Windows.Input.MouseButton]::Left) { return }
        $eventArgs.Handled = $true
        Open-RecentProject $sender
    })
}
function New-RecentItem([string]$name, [string]$path) {
    $item = [Windows.Controls.ListBoxItem]::new()
    $item.Tag = $path; $item.ToolTip = $path
    $panel = [Windows.Controls.StackPanel]::new()
    $title = [Windows.Controls.TextBlock]::new()
    $title.Text = $name; $title.FontWeight = 'SemiBold'; $title.TextTrimming = 'CharacterEllipsis'
    $location = [Windows.Controls.TextBlock]::new()
    $location.Text = $path; $location.FontSize = 11; $location.Foreground = '#91A2BA'
    $location.TextTrimming = 'CharacterEllipsis'; $location.Margin = '0,5,0,0'
    $null = $panel.Children.Add($title); $null = $panel.Children.Add($location)
    $item.Content = $panel
    Register-RecentActivation $item
    return $item
}
function Refresh-Projects {
    $controls.Recent.Items.Clear()
    foreach ($entry in @(Get-HERecentProjects $repositoryRoot)) {
        $item = New-RecentItem $entry.Name $entry.Path
        $null = $controls.Recent.Items.Add($item)
    }
    try {
        $selection = Find-HEProjectSelection -RepositoryRoot $repositoryRoot -StartDirectory $repositoryRoot
        $info = Resolve-HEProject -ProjectPath $selection.Path -RepositoryRoot $repositoryRoot
        $controls.Default.Text = "当前默认：$($info.DisplayName)`n$($info.Root)"
        if (-not (@(Get-HERecentProjects $repositoryRoot) | Where-Object { $_.Path -ieq $info.Root })) {
            $item = New-RecentItem "$($info.DisplayName) · 默认" $info.Root
            $controls.Recent.Items.Insert(0, $item)
        }
    } catch {
        if ((Test-Path -LiteralPath (Join-Path $repositoryRoot 'HEngine.distribution.json')) -and
            !(Test-Path -LiteralPath (Join-Path $repositoryRoot 'HEngine.workspace.local.json'))) {
            $controls.Default.Text = '尚未设置默认项目。可以创建空白工程，或导入已有工程。'
        } else { $controls.Default.Text = "未找到可用默认项目。请选择或创建项目。`n$($_.Exception.Message)" }
    }
}
function Show-ProjectError($errorValue) {
    $controls.Status.Text = [string]$errorValue
    if ($script:launchLog) { $controls.LogPanel.IsExpanded = $true }
}
function Select-Project([string]$path) {
    $controls.Path.Text = $path
    try {
        $info = Resolve-HEProject $path $repositoryRoot
        $controls.Pages.SelectedIndex = 0
        $controls.Details.Text = "$($info.DisplayName)  ·  Engine $($info.EngineVersion)`n$($info.Root)"
        $controls.Details.ToolTip = $info.Root
        $controls.Status.Text = if (Test-Path -LiteralPath (Join-Path $repositoryRoot 'HEngine.distribution.json')) {
            '项目已就绪。将使用此版本的 Editor 打开；修改 C# 脚本需要安装对应的 SDK。'
        } else { '项目已就绪。打开将先编译此项目的 Editor；首次构建需要较长时间。' }
    } catch { $controls.Details.Text = '项目暂不可用'; Show-ProjectError $_.Exception.Message }
}
function Quote-ProcessArgument([string]$value) {
    return '"' + [regex]::Replace([regex]::Replace($value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
$controls.Browse.Add_Click({
    $dialog = [Microsoft.Win32.OpenFileDialog]::new()
    $dialog.Filter = 'HEngine 项目 (Project.heproject)|Project.heproject'
    if ($dialog.ShowDialog($window)) { Select-Project $dialog.FileName }
})
$controls.Recent.Add_SelectionChanged({
    if ($controls.Recent.SelectedItem) { Select-Project ([string]$controls.Recent.SelectedItem.Tag) }
})
$controls.Recent.Add_KeyDown({
    param($sender, $eventArgs)
    if ($eventArgs.Key -eq [Windows.Input.Key]::Return -and
        $eventArgs.KeyboardDevice.Modifiers -eq [Windows.Input.ModifierKeys]::None) {
        $eventArgs.Handled = $true
        Open-RecentProject $controls.Recent.SelectedItem
    }
})
$controls.Use.Add_Click({
    try {
        $info = Set-HEWorkspaceProject $repositoryRoot $controls.Path.Text
        Add-HERecentProject $repositoryRoot $info
        Refresh-Projects
        $controls.Status.Text = '已记住此引擎工作区的默认项目。下次 hectl 和 Agent 自动使用它。'
    } catch { Show-ProjectError $_.Exception.Message }
})
$controls.Reset.Add_Click({
    try {
        Write-HEWorkspace (Join-Path $repositoryRoot 'HEngine.workspace.local.json') ([ordered]@{SchemaVersion=1;DefaultProject=''})
        Refresh-Projects
        $controls.Status.Text = '已恢复团队工作区的默认项目。'
    } catch { Show-ProjectError $_.Exception.Message }
})
$controls.Forget.Add_Click({
    try {
        if ($controls.Recent.SelectedItem) {
            $path = [string]$controls.Recent.SelectedItem.Tag
            $entries = @(Get-HERecentProjects $repositoryRoot | Where-Object { $_.Path -ine $path })
            Write-HEWorkspace (Join-Path $repositoryRoot 'out/agent/projects/recent.json') ([ordered]@{SchemaVersion=1;Projects=$entries})
            Refresh-Projects
            $controls.Status.Text = '已从最近列表移除；项目文件和默认绑定保留。'
        }
    } catch { Show-ProjectError $_.Exception.Message }
})
$controls.LocationBrowse.Add_Click({
    $dialog = [Windows.Forms.FolderBrowserDialog]::new()
    $dialog.Description = '选择父目录；新项目会创建在其中的新文件夹中'
    try { if ($dialog.ShowDialog() -eq 'OK') { $controls.Location.Text = $dialog.SelectedPath } }
    finally { $dialog.Dispose() }
})
$controls.Create.Add_Click({
    try {
        $folder = $controls.Folder.Text.Trim()
        if (-not $folder -or $folder -match '[<>:"/\\|?*\x00-\x1f]' -or $folder.EndsWith('.') -or $folder.EndsWith(' ') -or
            $folder -match '(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$') { throw '请填写有效的新文件夹名称。' }
        $template = [string]$controls.Template.SelectedItem.Tag
        $info = New-HEProject $repositoryRoot (Join-Path $controls.Location.Text $folder) $controls.Name.Text -Template $template
        Add-HERecentProject $repositoryRoot $info
        Refresh-Projects
        Select-Project $info.Root
        Open-Project
    } catch { Show-ProjectError $_.Exception.Message }
})
function Open-Project {
    try {
        if (-not $controls.Open.IsEnabled) { return }
        if ($script:launch -and -not $script:launch.HasExited) { return }
        if ([string]::IsNullOrWhiteSpace($controls.Path.Text)) { throw '请先选择项目。' }
        $info = Resolve-HEProject $controls.Path.Text $repositoryRoot
        if ($SmokeTest) {
            $script:smokeLaunchRequests += $info.Root
            if ($script:deferSmokeLaunch) { return }
        }
        Add-HERecentProject $repositoryRoot $info
        $logRoot = Join-Path $repositoryRoot 'out/agent/logs'
        $null = [IO.Directory]::CreateDirectory($logRoot)
        $script:launchLog = Join-Path $logRoot ('project-open-' + [guid]::NewGuid().ToString('N') + '.log')
        $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'ProjectLaunch.ps1'),'-Project',$info.Root,'-Log',$script:launchLog)
        if ($SmokeTest) { $arguments += @('-Check','-UseWarp') }
        $start = [Diagnostics.ProcessStartInfo]::new('powershell.exe', (($arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join ' '))
        $start.UseShellExecute = $false; $start.CreateNoWindow = $true
        $start.WorkingDirectory = $repositoryRoot
        $script:launch = [Diagnostics.Process]::Start($start)
        $null = $script:launch.Handle
        Set-LaunchBusy $true
        $controls.ViewLog.IsEnabled = $true
        $controls.Log.Clear()
        $controls.ViewLog.ToolTip = $script:launchLog
        $controls.Status.Text = "正在准备 $($info.DisplayName)，完成后将自动打开编辑器…"
    } catch { Show-ProjectError $_.Exception.Message }
}
$controls.Open.Add_Click({ Open-Project })
$controls.Cancel.Add_Click({
    try {
    if ($script:launch -and -not $script:launch.HasExited) {
        $state = Read-LaunchState
        if ($state.State -eq 'ready') { Complete-ProjectLaunch 0 $state; return }
        # Only this window's explicitly started build tree; never discover/stop
        # pre-existing Editor or Game instances by executable name.
        & taskkill.exe /PID $script:launch.Id /T /F | Out-Null
        $script:launch.WaitForExit(5000) | Out-Null
        if ($script:launch.HasExited) {
            $script:launch.Dispose(); $script:launch = $null
            Set-LaunchBusy $false
            $controls.Status.Text = "构建已取消。项目文件保留，可再次打开重试。日志：$script:launchLog"
        }
    }
    } catch { Show-ProjectError $_.Exception.Message }
})
$timer = [Windows.Threading.DispatcherTimer]::new()
$timer.Interval = [TimeSpan]::FromSeconds(1)
$timer.Add_Tick({
    try {
    if ($script:launch) {
        if (Test-Path -LiteralPath $script:launchLog) {
            $controls.Log.Text = (Get-Content -LiteralPath $script:launchLog -Encoding UTF8 -Tail 40 -ErrorAction SilentlyContinue) -join "`n"
            $controls.Log.ScrollToEnd()
        }
        $state = Read-LaunchState
        if ($state.State -eq 'starting') { $controls.Status.Text = '正在打开编辑器，等待项目加载…' }
        if ($state.State -eq 'ready' -and (Test-EditorReady $state)) {
            Complete-ProjectLaunch 0 $state
        } elseif ($script:launch.HasExited) {
            Complete-ProjectLaunch $script:launch.ExitCode $state
        }
    }
    } catch { Show-ProjectError $_.Exception.Message }
})
$window.Add_Closing({
    param($sender, $eventArgs)
    if ($script:launch -and -not $script:launch.HasExited) {
        if ((Read-LaunchState).State -eq 'ready') { return }
        $eventArgs.Cancel = $true
        $controls.Status.Text = '项目正在构建。可以点击“取消构建”，或等待完成后关闭此窗口。'
    }
})
$controls.Location.Text = [Environment]::GetFolderPath('MyDocuments')
Refresh-Projects
try {
    $initial = Resolve-HEProject -ProjectPath $Project -RepositoryRoot $repositoryRoot
    Select-Project $initial.Root
} catch {
    if (!$Project -and (Test-Path -LiteralPath (Join-Path $repositoryRoot 'HEngine.distribution.json'))) {
        $controls.Status.Text = '欢迎使用 HEngine。请选择已有的 Project.heproject，或创建一个新项目。'
    } else { Show-ProjectError $_.Exception.Message }
}
function Save-ManagerCapture {
    if (!$CapturePath) { return }
    $window.UpdateLayout()
    $bitmap = [Windows.Media.Imaging.RenderTargetBitmap]::new([int]$window.ActualWidth,[int]$window.ActualHeight,96,96,[Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($window)
    $encoder = [Windows.Media.Imaging.PngBitmapEncoder]::new()
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $stream = [IO.File]::Create([IO.Path]::GetFullPath($CapturePath))
    try { $encoder.Save($stream) } finally { $stream.Dispose() }
}
if ($LayoutTest) {
    try {
        $window.Show()
        foreach ($page in @(0,1)) {
            $controls.Pages.SelectedIndex = $page
            $window.UpdateLayout()
            $form = if ($page -eq 0) { $controls.OpenForm } else { $controls.CreateForm }
            if ($form.ScrollableHeight -gt 0.5 -or $form.ScrollableWidth -gt 0.5) {
                throw "Default page $page overflows at $($window.ActualWidth)x$($window.ActualHeight): $($form.ScrollableHeight) DIP"
            }
        }
        if (!$window.Icon) { throw 'Project Manager window icon is missing.' }
        $controls.Pages.SelectedIndex = if ($PreviewPage -eq 'Create') { 1 } else { 0 }
        Save-ManagerCapture
        Write-Output "static-check PASS: both default pages fit without scrolling; icon loaded; window $($window.ActualWidth)x$($window.ActualHeight)."
    } finally { $window.Close() }
} elseif ($SmokeTest) {
    $recentBefore = @(Get-HERecentProjects $repositoryRoot)
    try {
    $window.Show(); $window.UpdateLayout()
    $controls.Location.Text = Join-Path $repositoryRoot 'out/agent'
    $null = [IO.Directory]::CreateDirectory($controls.Location.Text)
    foreach ($definition in $templates) {
        $controls.Folder.Text = 'ui-project-test-' + [guid]::NewGuid().ToString('N')
        $controls.Template.SelectedItem = @($controls.Template.Items | Where-Object { $_.Tag -eq $definition.Id })[0]
        if ($controls.TemplateDescription.Text -ne $definition.Description) { throw "Template description did not update: $($definition.Id)" }
        $testCreationLaunch = $CreateLaunchTest -and $definition.Id -eq $CreateLaunchTemplate
        if ($testCreationLaunch) { $script:deferSmokeLaunch = $false; $controls.KeepOpen.IsChecked = $true }
        $controls.Create.RaiseEvent([Windows.RoutedEventArgs]::new([Windows.Controls.Button]::ClickEvent))
        $expected = Join-Path $controls.Location.Text $controls.Folder.Text
        if ($controls.Path.Text -ne $expected -or -not (Test-Path -LiteralPath (Join-Path $expected 'Project.heproject'))) {
            throw "Create interaction failed for $($definition.Id): $($controls.Status.Text)"
        }
        if ($script:smokeLaunchRequests[-1] -ne $expected) { throw 'Created project was not passed to automatic launch.' }
        if ($testCreationLaunch) {
            if (!$script:launch) { throw "Create did not launch Editor: $($controls.Status.Text)" }
            $deadline = [DateTime]::UtcNow.AddMinutes(5)
            while (!$script:launch.WaitForExit(1000)) {
                if ([DateTime]::UtcNow -gt $deadline) {
                    & taskkill.exe /PID $script:launch.Id /T /F | Out-Null
                    throw 'Created-project Editor startup timed out.'
                }
            }
            $creationState = Read-LaunchState
            if ($script:launch.ExitCode -ne 0 -or $creationState.State -ne 'validated') {
                throw "Created-project Editor did not become ready: $script:launchLog"
            }
            Complete-ProjectLaunch 0 $creationState
            if (!$window.IsVisible) { throw 'Keep-open setting was ignored after creation.' }
            $script:deferSmokeLaunch = $true
        }
        $sourceScene = Join-Path $repositoryRoot "Engine/Templates/$($definition.Id)/Assets/Scenes/Main.he"
        $createdScene = Join-Path $expected 'Assets/Scenes/Main.he'
        if ((Get-FileHash -LiteralPath $createdScene).Hash -ne (Get-FileHash -LiteralPath $sourceScene).Hash) {
            throw "Template selection created the wrong scene: $($definition.Id)"
        }
        if ($definition.Id -eq 'SharedArena') {
            $createdProfiles = Get-Content -Raw -LiteralPath (Join-Path $expected 'Config/BuildProfiles.json') | ConvertFrom-Json
            if (@($createdProfiles.profiles).Count -ne 4 -or !(Test-Path -LiteralPath (Join-Path $expected 'Scripts/CSharp/Rules/GameSession.cs'))) {
                throw 'SharedArena template selection did not create the shared four-platform game.'
            }
        }
        if ($definition.Id -in @('FirstPerson','TopDown') -and
            -not (Test-Path -LiteralPath (Join-Path $expected "Scripts/CSharp/$($definition.Id)Controller.cs"))) {
            throw "Template selection lost its controller: $($definition.Id)"
        }
    }
    $controls.Create.RaiseEvent([Windows.RoutedEventArgs]::new([Windows.Controls.Button]::ClickEvent))
    if ($controls.Status.Text -notlike '*already exists*') { throw 'Duplicate creation did not report an error.' }
    if ($script:smokeLaunchRequests.Count -ne $templates.Count) { throw 'Failed creation attempted to open a project.' }
    Select-Project $expected
    $window.UpdateLayout()
    # Cancellation must affect only the process tree owned by this window.
    $sleepStart = [Diagnostics.ProcessStartInfo]::new('powershell.exe', '-NoProfile -Command "Start-Sleep -Seconds 30"')
    $sleepStart.UseShellExecute = $false; $sleepStart.CreateNoWindow = $true
    $sentinel = [Diagnostics.Process]::Start($sleepStart)
    try {
        $script:launch = [Diagnostics.Process]::Start($sleepStart)
        $controls.Cancel.RaiseEvent([Windows.RoutedEventArgs]::new([Windows.Controls.Button]::ClickEvent))
        if ($script:launch -or $sentinel.HasExited) { throw 'Cancellation did not isolate its owned process.' }
    }
    finally {
        if (-not $sentinel.HasExited) { $sentinel.Kill(); $sentinel.WaitForExit(5000) | Out-Null }
        $sentinel.Dispose()
    }
    Select-Project $expected
    Save-ManagerCapture
    $script:deferSmokeLaunch = $false
    # Exercise recent-item activation through the shared process-launch path,
    # validation project, using a bounded check instead of a user-owned Editor.
    if ($Project) {
        $target = Resolve-HEProject $Project $repositoryRoot
        Add-HERecentProject $repositoryRoot $target
        Refresh-Projects
        $item = @($controls.Recent.Items | Where-Object { $_.Tag -eq $target.Root })[0]
        $controls.Recent.SelectedItem = $item
        # A manually edited path must not override the item being activated.
        $controls.Path.Text = $expected
        $emptyClick = [Windows.Input.MouseButtonEventArgs]::new([Windows.Input.Mouse]::PrimaryDevice, 0, [Windows.Input.MouseButton]::Left)
        $emptyClick.RoutedEvent = [Windows.Controls.Control]::MouseDoubleClickEvent
        $controls.Recent.RaiseEvent($emptyClick)
        $rightClick = [Windows.Input.MouseButtonEventArgs]::new([Windows.Input.Mouse]::PrimaryDevice, 0, [Windows.Input.MouseButton]::Right)
        $rightClick.RoutedEvent = [Windows.Controls.Control]::MouseDoubleClickEvent
        $item.RaiseEvent($rightClick)
        if ($script:launch) { throw 'Empty-space/right-button activation opened a project.' }
        $doubleClick = [Windows.Input.MouseButtonEventArgs]::new([Windows.Input.Mouse]::PrimaryDevice, 0, [Windows.Input.MouseButton]::Left)
        $doubleClick.RoutedEvent = [Windows.Controls.Control]::MouseDoubleClickEvent
        $item.RaiseEvent($doubleClick)
        if (-not $script:launch) { throw "Open interaction failed: $($controls.Status.Text)" }
        if ($controls.Path.Text -ne $target.Root) { throw 'Recent activation selected the wrong project.' }
        $launchId = $script:launch.Id
        $enter = [Windows.Input.KeyEventArgs]::new([Windows.Input.Keyboard]::PrimaryDevice,
            [Windows.PresentationSource]::FromVisual($window), 0, [Windows.Input.Key]::Return)
        $enter.RoutedEvent = [Windows.Input.Keyboard]::KeyDownEvent
        $controls.Recent.RaiseEvent($enter)
        $doubleClick.Handled = $false
        $item.RaiseEvent($doubleClick)
        if ($script:launch.Id -ne $launchId -or -not $enter.Handled) { throw 'Repeated activation launched another build.' }
        $deadline = [DateTime]::UtcNow.AddMinutes(10)
        while (-not $script:launch.WaitForExit(1000)) {
            if ([DateTime]::UtcNow -gt $deadline) {
                & taskkill.exe /PID $script:launch.Id /T /F | Out-Null
                throw "Open validation timed out: $script:launchLog"
            }
        }
        if ($script:launch.ExitCode -ne 0) { throw "Open validation failed: $script:launchLog" }
        $state = Read-LaunchState
        if ($state.State -ne 'validated') { throw 'Editor exited without confirming its first frame.' }
        # A zero exit code alone must not close the window. Keep-open and the
        # default auto-close exercise the same completion handler as the timer.
        $controls.KeepOpen.IsChecked = $false
        Complete-ProjectLaunch 0 ([pscustomobject]@{State='failed';Message='Simulated startup failure'})
        if (-not $window.IsVisible -or -not $controls.Open.IsEnabled) { throw 'Failed launch closed the manager or prevented retry.' }
        $controls.KeepOpen.IsChecked = $true
        Complete-ProjectLaunch 0 $state
        if (-not $window.IsVisible) { throw 'Keep-open preference was ignored.' }
        $controls.KeepOpen.IsChecked = $false
        Complete-ProjectLaunch 0 $state
        if ($window.IsVisible) { throw 'Ready Editor did not close the manager.' }
    }
    if ($CreateLaunchTest -and !$Project) {
        $controls.KeepOpen.IsChecked = $false
        Complete-ProjectLaunch 0 $creationState
        if ($window.IsVisible) { throw 'Manager did not close after created-project readiness.' }
        Write-Output 'test-layer PASS: create button -> real Editor readiness -> keep-open/auto-close; templates and owned cancellation.'
    }
    elseif ($Project) { Write-Output 'Project manager interactions, ready handshake, failure recovery, keep-open and auto-close: PASS' }
    else { Write-Output 'Project manager templates, creation/auto-launch routing, duplicate rejection and owned cancellation: PASS; actual Editor launch not requested.' }
    }
    finally {
        if ($script:launch) {
            if (!$script:launch.HasExited) {
                & taskkill.exe /PID $script:launch.Id /T /F | Out-Null
                $null = $script:launch.WaitForExit(5000)
            }
            $script:launch.Dispose(); $script:launch = $null
        }
        Write-HEWorkspace (Join-Path $repositoryRoot 'out/agent/projects/recent.json') ([ordered]@{SchemaVersion=1;Projects=$recentBefore})
        $window.Close()
    }
} else {
    $timer.Start()
    try { $null = $window.ShowDialog() } finally { $timer.Stop() }
}

