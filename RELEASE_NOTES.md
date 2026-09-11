WinDuo 0.2.0: native C++/Win32 edition.

- Native standalone executable under 1 MB; no .NET dependency.
- Tray-only launch, compact Settings, startup preference and single-instance activation.
- Preview hidden by default; default tests no longer animate the desktop.
- Debounced lid-close input; initial and invalid notifications ignored.
- Stale effects cancelled on open, lock, unlock and suspend; missed open events cannot leave automatic blur stuck.
- Internal-panel-only, click-through Gaussian blur with DXGI/GDI capture.
- Opt-in lock wallpaper backup/restore and migration from v0.1 preferences.
- C# application and .NET build dependencies removed from the current source tree.

Windows 10 2004+ or Windows 11. Unsigned. Binary lid state cannot measure hinge motion or reliably delay sleep. Win+L needs an existing cached frame; Spotlight mode is not restored. ARM64 needs hardware validation.
