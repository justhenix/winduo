WinDuo 0.3.0

- Ctrl+Alt+Space or a tray click toggles a reversible virtual hinge.
- Removed the old demo menu, checkbox and timed sequence.
- ACPI close/open uses 500/400 ms easing with an optional 1.5-second awake request.
- Direct2D Gaussian blur runs on D3D11, with a software fallback.
- Dark native Settings includes strength, startup and opt-in lock wallpaper recovery.
- Experimental webcam input is off by default; front camera auto-selection or explicit camera selection.
- Camera calibration, median spike rejection, 250 ms filtering, and idle/fullscreen/lid shutdown keep sampling bounded and local.
- Added CMake x64 Release output at out/WinDuo.exe and native regression checks.

The webcam signal is a lighting-based approximation, not measured hinge angle. Sampling stops after three seconds at idle; re-enable or calibrate to restart. Awake requests cannot guarantee overriding Windows lid sleep. Camera hardware behavior and lock wallpaper personalization still need testing on the target device.
