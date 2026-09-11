WinDuo 0.2.1

- Fix Slight/Normal dropdown resetting when opened.
- Preserve the known lid state after Preview, pause and display changes, so the next real close is not ignored.
- Remove the extra 120 ms delay before responding to Windows lid-close signals.
- Show the last Windows lid state in Settings; check registration failures.
- Keep stale-frame cancellation, automatic timeout and no automatic previews.

Most Windows laptops report only open/closed, not hinge angle. Partial lid movement cannot drive the effect without a live sensor, and Windows may turn the panel off immediately at closure.
