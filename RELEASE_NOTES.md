WinDuo 0.3.2

- Heavy frosted glass defocus: increased blur sigma to 60 / 110 / 175 px at 1080p.
- Complete bokeh transition: upper screen transitions to 100% blur so phone cameras cannot reconstruct text edges.
- Frosted glass scattering: added translucent shadow lifting and diffuse luminous sheen matching Bendy.
- Hardware card perspective: added anti-aliased rounded top card corners and soft corner vignetting against the black void.
- Responsive lid tilt tracking: mapped camera tilt curve to begin proportional folding immediately upon closing, removing the deadband that delayed tilt until the lid was nearly shut.
- Uplift damping: asymmetric inertia eliminates jitter and flutter when reopening the lid.

WinDuo 0.3.1

- Replace Slight/Normal with a three-step Low / Medium / High blur slider.
- Default to Medium, including upgrades from the old two-level setting.
- Increase Gaussian blur to 24 / 40 / 64 px standard deviation at 1080p, scaled with display resolution.
- Keep more blur near the bottom edge so the effect is clearly visible.
- Apply the selected level to both the desktop and subsequently generated lock wallpaper.
- Verify all three slider positions and progressively stronger blur on GPU and software paths.
