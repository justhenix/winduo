WinDuo 0.2.2

- Ease out on lid-open without abruptly hiding the overlay.
- Keep 600 ms close / 400 ms open easing for binary lid input.
- Preserve Preview as the optional manual hinge demo; never run it on launch.

This PC exposes an ACPI lid switch, but no Windows hinge-angle or IMU sensor. Partial lid movement cannot drive this fallback; Windows may turn the panel off at closure.
