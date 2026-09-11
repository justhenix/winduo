param([string]$Exe = "$PSScriptRoot/../out/WinDuo.exe", [switch]$LiveEffect)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NativeAppTest {
 public delegate bool Callback(IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(Callback c, IntPtr l);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr w, StringBuilder s, int n);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr w,uint m,IntPtr a,IntPtr b);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr w,int id);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr w);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr w,IntPtr dc,uint flags);
 [DllImport("user32.dll")] public static extern bool SetWindowDisplayAffinity(IntPtr w,uint affinity);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr w,out Rect r);
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
}
'@
function Find-AppWindow($processId, $className) {
    $script:foundWindow=[IntPtr]::Zero
    [NativeAppTest]::EnumWindows({param($window,$unused)
        $owner=0
        [NativeAppTest]::GetWindowThreadProcessId($window,[ref]$owner)|Out-Null
        if ($owner -eq $processId) {
            $name=New-Object Text.StringBuilder 256
            [NativeAppTest]::GetClassName($window,$name,256)|Out-Null
            if ($name.ToString() -eq $className) {$script:foundWindow=$window}
        }
        return $true
    },[IntPtr]::Zero)|Out-Null
    return $script:foundWindow
}
function Send-Command($window,$id) {[NativeAppTest]::SendMessage($window,273,$id,0)|Out-Null}
$unit=Start-Process $Exe -ArgumentList '--self-test' -WindowStyle Hidden -PassThru -Wait
Get-Content artifacts/self-test.txt
if($unit.ExitCode -ne 0){throw 'Native unit checks failed.'}
$idle=Start-Process $Exe -ArgumentList '--smoke-test' -WindowStyle Hidden -PassThru -Wait
Copy-Item artifacts/smoke-test.txt artifacts/native-idle-test.txt -Force
if((Get-Content artifacts/smoke-test.txt -Raw) -notmatch 'PASS captures=0 presented=0'){throw 'Idle captured a frame.'}
$app=Start-Process $Exe -ArgumentList '--smoke-test' -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Milliseconds 700
    $second=Start-Process $Exe -WindowStyle Hidden -PassThru -Wait
    Start-Sleep -Milliseconds 200
    $window=Find-AppWindow $app.Id 'WinDuo.Native.Accessory'
    $settings=Find-AppWindow $app.Id 'WinDuo.Native.Settings'
    if(!$window -or !$settings -or ![NativeAppTest]::IsWindowVisible($settings)){throw 'Second launch did not open Settings.'}
    if([NativeAppTest]::GetDlgItem($settings,104) -ne [IntPtr]::Zero){throw 'Unexpected demo surface.'}
    Send-Command $settings 101
    if([NativeAppTest]::SendMessage([NativeAppTest]::GetDlgItem($settings,101),240,0,0) -ne 0){throw 'Enable did not disable.'}
    Send-Command $settings 101
    if([NativeAppTest]::SendMessage([NativeAppTest]::GetDlgItem($settings,101),240,0,0) -ne 1){throw 'Enable did not restore.'}
    $strength=[NativeAppTest]::GetDlgItem($settings,105)
    foreach($level in @(0,1,2,1)) {
        [NativeAppTest]::SendMessage($strength,1029,1,$level)|Out-Null
        [NativeAppTest]::SendMessage($settings,276,5,$strength)|Out-Null
        if([NativeAppTest]::SendMessage($strength,1024,0,0) -ne $level){throw 'Blur slider did not retain its level.'}
    }
    Send-Command $settings 107
    Send-Command $settings 107
    Send-Command $settings 106
    Send-Command $settings 106
    $rect=New-Object NativeAppTest+Rect
    [NativeAppTest]::GetWindowRect($settings,[ref]$rect)|Out-Null
    $image=New-Object Drawing.Bitmap ($rect.Right-$rect.Left),($rect.Bottom-$rect.Top)
    $graphics=[Drawing.Graphics]::FromImage($image);$dc=$graphics.GetHdc()
    try {[NativeAppTest]::PrintWindow($settings,$dc,0)|Out-Null}finally{$graphics.ReleaseHdc($dc)}
    $image.Save((Join-Path (Get-Location) 'artifacts/native-settings.png'));$graphics.Dispose();$image.Dispose()
    [NativeAppTest]::SendMessage($settings,16,0,0)|Out-Null
    if([NativeAppTest]::IsWindowVisible($settings)){throw 'Settings close did not hide it.'}
    $app.WaitForExit()
    Copy-Item artifacts/smoke-test.txt artifacts/native-settings-test.txt -Force
} finally {if(!$app.HasExited){$w=Find-AppWindow $app.Id 'WinDuo.Native.Accessory';if($w){Send-Command $w 103};$app.WaitForExit()}}
if ($LiveEffect) {
$app=Start-Process $Exe -ArgumentList '--smoke-test' -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Milliseconds 700
    $window=Find-AppWindow $app.Id 'WinDuo.Native.Accessory'
    if(!$window){throw 'Test app window unavailable.'}
    Send-Command $window 104
    $app.WaitForExit()
    Copy-Item artifacts/smoke-test.txt artifacts/native-effect-test.txt -Force
    Get-Content artifacts/native-effect-test.txt
    if((Get-Content artifacts/native-effect-test.txt -Raw) -notmatch 'PASS captures=\d+ presented=[1-9]'){throw 'Live effect produced no frames; check panel/fullscreen status.'}
} finally {if(!$app.HasExited){Send-Command $window 103;$app.WaitForExit()}}
}
Write-Output 'PASS Native idle, settings and single-instance checks. Live effect runs only with -LiveEffect.'
