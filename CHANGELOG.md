# Changelog

This changelog preserves the public product history of this macOS Apple
Silicon fork. Historical entries were reconstructed from audited source
snapshots and release documentation; see
[source provenance](docs/history/source-provenance.md).

## 3.4.0 — 2026-08-31

### Features

- Added exact macOS display-topology profiles that select the saved freeform
  layout for the current physical display arrangement.
- Added optional proximity-gated client connections using a paired Bluetooth
  identity, Bonjour readiness, and topology readiness.
- Added user-adjustable Connect and Departure signal thresholds from
  `-100 dBm` through `-30 dBm`, with `-75/-90 dBm` defaults and at least
  `15 dB` of hysteresis.
- Added optional, pair-scoped client signal sharing so the server can display
  filtered RSSI and last-seen state while Proximity Settings is open.
- Added rate-limited Bonjour network wake for configured offline clients.

### Bug fixes

- Paused switching when the current display topology is unknown or
  temporarily unavailable, without disrupting brief monitor transitions.
- Restored clients after server display-layout restarts and macOS sleep/wake
  without requiring a manual reload.
- Kept manually configured server addresses authoritative when Auto config is
  disabled, while still using the paired service for eligibility.
- Preserved pairing metadata, selected routable service addresses, and
  rejected auxiliary discovery endpoints as connection targets.
- Normalized multi-display links, persisted accepted topology profiles
  immediately, and suppressed repeated identical service-resolution logs.
- Added checksum-pinned macOS dependencies, complete Mach-O deployment-target
  checks, and release privacy audits.

## 3.3.1 — 2026-08-29

### Bug fixes

- Tolerated small coordinate-rounding gaps in freeform layouts so visually
  adjacent screens retain their intended links.
- Corrected partial-edge coordinate mapping and prevented the cursor from
  bouncing back to the source screen.
- Improved how client display rectangles are represented in the freeform
  layout.

## 3.3.0 — 2026-08-29

### Compatibility

- Restored interoperability with Barrier 2.4.0 and earlier protocol peers.
- Sent enhanced display geometry and display names only to peers that advertise
  support for those extensions.
- Kept a rectangular compatibility fallback for older peers.

## 3.2.0 — 2026-08-29

Status: draft, superseded by later releases.

### Features and fixes

- Added native Apple Silicon packaging for macOS.
- Added physical display geometry, display labels, freeform layouts, L-shaped
  layouts, and routing along real display-rectangle edges.
- Added client display wake-on-entry and layout persistence.
- Improved input in login sessions, perceived scroll direction across hosts,
  and activation of menu-bar windows.
- Added forwarding for Magic Mouse two-finger Spaces gestures.

## 2.4.0 — 2021-11-01

This is the retained upstream compatibility baseline. It added client identity
verification, SHA-256 certificate fingerprints, safer command-line encryption
defaults, randomart fingerprints, additional input support, and a collection
of platform and clipboard fixes. Full upstream notes remain in
[the release notes](doc/release_notes/index.md).

## Earlier upstream releases

Earlier upstream changes remain available in [ChangeLog](ChangeLog),
[the Debian changelog](debian/changelog), and
[the release notes](doc/release_notes/index.md).
