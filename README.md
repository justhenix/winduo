# WinDuo

A tiny native Windows tray app that softly blurs your laptop display on lid close. Inspired by [Lid Plane](https://github.com/jh3y/lid-plane). C++/Win32, MIT licensed, no .NET required.

## Install

Download your executable from [Releases](https://github.com/justhenix/winduo/releases), keep it in a permanent folder, and run it. Requires Windows 10 2004+ or Windows 11. Start with Windows defaults on. Quit any older WinDuo first.

## Controls

- Double-click the tray icon or launch again to open Settings.
- Choose Slight or Normal; lock-screen blur is opt-in.
- Preview is hidden by default. Enable **Show preview menu** to run a one-shot effect.
- Esc pauses an active effect. Quit exits; disable startup before uninstalling.

Only the internal panel is affected. Idle stops capture. Fullscreen apps pause the effect. No uploads or analytics.

**Windows limits:** most laptops report only open/closed, not hinge angle; lid sleep cannot reliably be delayed. Lock blur uses a locally saved wallpaper, not a secure-desktop overlay. Win+L needs a cached frame and may lock before it updates. The previous image is backed up/restored; Spotlight mode may need reselecting manually.

## Build

Install Visual Studio C++ Build Tools and the Windows SDK, then run:

```powershell
./scripts/publish.ps1 -Runtime win-x64
./native/test.ps1
```

Output: `artifacts/WinDuo-win-x64.exe`. ARM64: use `-Runtime win-arm64` with ARM64 build tools. Default tests never animate your desktop; `-LivePreview` explicitly enables that test. Builds are unsigned.
