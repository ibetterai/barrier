# Barrier macOS Apple Silicon fork

This repository is the iBetterAI-maintained macOS Apple Silicon fork of
[upstream Barrier](https://github.com/debauchee/barrier).

It keeps upstream Barrier as the base project and adds macOS-specific builds,
display-routing behavior, and compatibility fixes for this fork. For general
Barrier usage, supported platforms, FAQ, source history, and upstream project
details, see the [upstream Barrier repository](https://github.com/debauchee/barrier).

## Download

- Latest Apple Silicon macOS build:
  [Barrier 3.3.0](https://github.com/ibetterai/barrier/releases/tag/v3.3.0)
- Current DMG:
  [Barrier-3.3.0-release-arm64.dmg](https://github.com/ibetterai/barrier/releases/download/v3.3.0/Barrier-3.3.0-release-arm64.dmg)

This fork currently publishes Apple Silicon macOS packages only.

## What this fork adds

- Native Apple Silicon macOS build and release packaging.
- Dynamic macOS physical-display geometry support.
- Non-rectangular and L-shaped monitor routing.
- Freeform server configuration that follows the live macOS Displays layout
  when displays move and the configuration is re-saved.
- Cross-machine edge routing based on real display rectangles, while preserving
  normal local macOS transitions between displays on the same host.
- macOS display labels in the freeform canvas, using OS-provided display names
  when available.
- Client display wake-on-entry when the target macOS display system is asleep.
- Consistent perceived direction for Magic Mouse and trackpad two-finger
  vertical and horizontal scrolling across hosts.
- macOS menu bar fixes so Barrier windows such as settings and logs reliably
  come to the foreground.
- Magic Mouse two-finger left/right Spaces swipe forwarding. When the pointer is
  on a macOS client, the server detects the swipe intent and the client injects
  the native `Control` + left/right Space-switching shortcut.
- Barrier 2.4.0 interoperability restored in Barrier 3.3.0. Protocol 1.6 peers
  receive only 2.4.0-compatible messages; 3.x peers keep enhanced display
  geometry and display names.

## Compatibility

Barrier 3.3.0 is the recommended release from this fork.

- Barrier 3.3.0 works with Barrier 2.4.0 peers by negotiating old-peer protocol
  behavior and suppressing 3.x-only display metadata unless the peer supports it.
- Barrier 3.x peers keep enhanced multi-display and L-shaped routing when both
  sides support protocol 1.7.
- Barrier 2.4.0 peers use rectangular-screen fallback behavior.
- Barrier 3.2.0 is superseded and hidden because it may not interoperate with
  Barrier 2.4.0 peers.

## Known limitations

- Apple Silicon macOS is the only packaged target published by this fork.
- A true headless/background macOS service is not implemented. Use the
  macOS app/session path.
- Magic Mouse Spaces swipe forwarding is macOS-specific and maps to the target
  Mac's Space-switching shortcut path, not a general-purpose gesture relay.
- If the target Mac's Mission Control keyboard shortcut for moving left/right a
  Space is disabled or remapped away from `Control` + left/right arrow, Spaces
  switching may not occur until that macOS setting is restored.

## Contact and support for this fork

Use the GitHub issue tracker for project support.

## Upstream Barrier

Use upstream Barrier for:

- Windows, Linux, Intel macOS, and BSD releases.
- General documentation and FAQ.
- Upstream issues, contribution guidance, and source history.

Upstream repository: <https://github.com/debauchee/barrier>
