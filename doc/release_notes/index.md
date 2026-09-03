Release notes
=============

[comment]: <> (towncrier release notes start)
Barrier `3.4.4` ( `2026-09-03` )
================================

Bug fixes
---------

- Forwarded the Globe-tap key (key code 179) that accompanies every Magic Keyboard Fn tap. Its
  KeyDown was swallowed as unmapped while its KeyUp leaked as KeyID 0, so the tap arrived
  incomplete; it now travels as its own Globe key with matching KeyDown/KeyUp synthesis.

Barrier `3.4.3` ( `2026-09-03` )
================================

Bug fixes
---------

- Synthesized Fn/Globe and right-hand modifiers as FlagsChanged events on the macOS client.
  Globe behavior is driven by SecondaryFn state rather than KeyDown events, so forwarded Fn
  presses had no effect until now.

Barrier `3.4.2` ( `2026-09-03` )
================================

Bug fixes
---------

- Forwarded every macOS server key to the client while the mouse is on the client display. Lone
  Fn/Globe presses, left/right Command, Option, Shift, and Control presses, and brightness keys
  are now delivered instead of being swallowed, and modifiers held across a screen switch are
  replayed to the newly entered screen. Only registered Barrier hotkeys stay on the server.

Barrier `3.4.1` ( `2026-08-31` )
================================

Bug fixes
---------

- Reconciled every saved display-topology profile when configured screens change. Removed screens
  are pruned from compatible profiles, while profiles missing a newly configured screen are
  discarded instead of producing a configuration that the server cannot read.
- Made Stop cancel pending automatic restarts. Configuration-read failures now stop retrying and
  show guidance to repair the screen layout before starting again.

Barrier `3.4.0` ( `2026-08-31` )
================================

Why this release matters
------------------------

**Barrier 3.4.0 makes the pointer follow the physical desk, even when the desk
changes shape.**

[Apple Universal Control](https://support.apple.com/guide/mac-help/mchl412faecf/mac)
is designed for nearby Macs and iPads using the same Apple Account. Barrier
3.4.0 is aimed at changing multi-monitor desks and mixed-platform setups. The
complete 3.4.0 experience brings together these capabilities:

- **Real L-shaped and partial-edge routing.** Modern fork peers exchange the
  physical rectangles of their macOS displays, so the pointer crosses the
  actual shared edge instead of a machine-sized bounding box.
- **A server Mac that can leave and return.** An opt-in macOS client can wait
  for its explicitly paired server, then reconnect when that server is nearby,
  its matching Bonjour service is reachable, and its display topology is ready.
  Connect and Departure thresholds are configurable for each pairing.
- **The right layout for the monitors that are connected now.** Save a profile
  once for each server display combination. After docking, undocking, opening
  or closing the lid, or changing external monitors, Barrier selects the exact
  matching profile automatically. An unknown arrangement pauses switching and
  asks to be saved instead of guessing.
- **Displays that can sleep when they are not being used.** Barrier wakes a
  connected macOS client display when the pointer enters. For an offline Mac,
  it can also request a best-effort Bonjour network wake when **Wake for network
  access** is enabled, without pretending that Bluetooth alone can wake a Mac.
- **A path forward without abandoning Legacy Barrier.** Barrier 3.4.0 restores
  protocol interoperability with Barrier 2.4 and earlier peers, including
  existing Windows and Linux machines. Legacy peers use rectangular fallback;
  modern fork peers keep the enhanced geometry.

These are the user-facing capabilities available in 3.4.0, not a claim that
every item first appeared in this point release. The chronological 3.2, 3.3,
and 3.4 sections below preserve when each feature or fix was delivered.

Features
--------

- Added macOS display-topology profiles that automatically select the exact saved freeform layout
  and pause cross-machine switching for unknown or unavailable arrangements.
- Added optional macOS proximity-gated connections that keep clients idle until their explicitly
  paired server is nearby, discoverable, and topology-ready.
- Added pairing-scoped Connect and Departure signal controls for proximity clients. Values cover
  `-100 dBm` through `-30 dBm`, default to `-75/-90`, require at least `15 dB` of hysteresis, and
  save atomically without re-pairing.
- Added optional client signal sharing so the paired server can show associated clients' filtered
  RSSI and last-seen state while Proximity Settings is open. Sharing is off by default on upgrades
  and uses an opaque pair-scoped routing ID that is not authentication.
- Added rate-limited Bonjour network wake when moving toward a configured offline client. Wake is
  independent of Bluetooth signal scanning and preserves the normal Barrier reconnect path.

Bug fixes
---------

- Fixed proximity pairing when Auto config is disabled, preserved Bonjour metadata through TXT
  replacement, selected routable server addresses, and prevented clients from connecting to the
  auxiliary Bonjour discovery port.
- Fixed client recovery after paired-server restarts caused by display-layout changes and avoided
  false signal-loss warnings from brief closed-lid RSSI dips.
- Fixed multi-display link normalization so saved topology profiles activate and allow switching,
  and persisted accepted display profiles immediately so they survive an app restart.
- Fixed connected clients remaining unreachable until the server was reloaded after macOS sleep
  and wake by keeping Quartz event taps on the Cocoa main run loop and re-enabling disabled taps.
- Fixed proximity gating overriding a manually configured Server IP with the paired Bonjour
  hostname when Auto config is disabled.
- Suppressed repeated identical Zeroconf resolution snapshots and duplicate endpoint log lines
  while preserving real service and topology-readiness updates.
- Fixed Apple Silicon release packaging so Qt and OpenSSL come from checksum-pinned sources built
  for macOS 12, every bundled Mach-O file is audited before signing, and release checks reject
  local home paths and concrete private IPv4 addresses from tracked text sources and final app bundles.
- Fixed event-queue startup readiness so all callers already waiting are released when
  initialization completes.

Barrier `3.3.1` ( `2026-08-29` )
================================

Bug fixes
---------

- Tolerated small coordinate-rounding gaps in freeform layouts so visually adjacent screens keep
  their intended links.
- Corrected partial-edge coordinate mapping so the pointer does not bounce back to the source
  screen.
- Improved freeform display-rectangle handling for portrait displays, multiple displays, and
  multiple simultaneously connected clients.

Barrier `3.3.0` ( `2026-08-29` )
================================

Compatibility
-------------

- Restored interoperability with Barrier 2.4.0 and earlier protocol peers.
- Sent enhanced display geometry and display names only to peers that advertise support, while
  retaining the rectangular fallback for older peers.

Barrier `3.2.0` ( `2026-08-29`, draft and superseded )
======================================================

Features and fixes
------------------

- Added native Apple Silicon packaging for macOS.
- Added physical display geometry, display labels, freeform and L-shaped layouts, and real-edge
  routing.
- Added client display wake-on-entry, layout persistence, reliable login-session input, consistent
  scrolling, menu activation fixes, and Magic Mouse Spaces gesture forwarding.
- Fixed cross-platform HTML clipboard encoding, restored Windows display scaling above 100%, and
  fixed GCC 11.2 builds.
- Improved incompatible-macOS launch guidance, updated Japanese translations, and refreshed
  OS-support and Linux drag-and-drop guidance.

Barrier `2.4.0` ( `2021-11-01` )
================================

Security fixes
--------------

- Barrier now supports client identity verification (fixes CVE-2021-42072, CVE-2021-42073).

  Previously a malicious client could connect to Barrier server without any authentication and
  send application-level messages. This made the attack surface of Barrier significantly larger.
  Additionally, in case the malicious client got possession of a valid screen name by brute forcing
  or other means it could modify the clipboard contents of the server.

  To support seamless upgrades from older versions of Barrier this is currently disabled by default.
  The feature can be enabled in the settings dialog. If enabled, older clients of Barrier will be
  rejected.

- Barrier now uses SHA256 fingerprints for establishing security of encrypted SSL connections.
  After upgrading client to new version the existing server fingerprint will need to be approved
  again. Client and server will show both SHA1 and SHA256 server fingerprints to allow
  interoperability with older versions of Barrier.

All of the above security issues have been reported by Matthias Gerstner who was really helpful
resolving them.

Bug fixes
---------

- Fixed build failure on mips*el and riscv64 architecture.
- Fixed reading of configuration on Windows when the paths contain non-ASCII characters
(https://github.com/debauchee/barrier/issues/976, https://github.com/debauchee/barrier/issues/974,
 https://github.com/debauchee/barrier/issues/444).
- Barrier no longer uses openssl CLI tool for any operations and hooks into the openssl library directly.
- More X11 clipboard MIME types have been mapped to corresponding converters (https://github.com/debauchee/barrier/issues/344).
- Fixed setup of multiple actions associated with a hotkey.
- Fixed setup of hotkeys with special characters such as comma and semicolon
  (https://github.com/debauchee/barrier/issues/778).
- Fixed transfer of non-ASCII characters coming from a Windows server in certain cases
 (https://github.com/debauchee/barrier/issues/527).
- Barrier will now regenerate server certificate if it's invalid instead of failing to launch
 (https://github.com/debauchee/barrier/issues/802)
- Added support for additional keys on Sun Microsystems USB keyboards
 (https://github.com/debauchee/barrier/issues/784).
- Updated Chinese translation.
- Updated Slovak translation.
- Theme icons are now preferred to icons distributed together with Barrier
 (https://github.com/debauchee/barrier/issues/471).
- Fixed incorrect setup of Barrier service path on Windows.

Features
--------

- Added `--drop-target` option that improves drag and drop support on Windows when Barrier is
  being run as a portable app.
- The `--enable-crypto` command line option has been made the default to reduce chances of
  accidental security mishaps when configuring Barrier from command line.
  A new `--disable-crypto` command line option has been added to explicitly disable encryption.
- Added support for randomart images for easier comparison of SSL certificate fingerprints.
  The algorithm is identical to what OpenSSH uses.
- Implemented a configuration option for Server GUI auto-start.
- Made it possible to use keyboard instead of mouse to modify screen layout.
- Added support for keyboard backlight media keys
- Added support for Eisu_toggle and Muhenkan keys
- Added `--profile-dir` option that allows to select custom profile directory.

Barrier `2.3.4` ( `2021-11-01` )
================================

Security fixes
--------------

- Barrier will now correctly close connections when the app-level handshake fails (fixes CVE-2021-42075).

  Previously repeated failing connections would leak file descriptors leading to Barrier being unable
  to receive new connections from clients.

- Barrier will now enforce a maximum length of input messages (fixes CVE-2021-42076).

  Previously it was possible for a malicious client or server to send excessive length messages
  leading to denial of service by resource exhaustion.

- Fixed a bug which caused Barrier to crash when disconnecting a TCP session just after sending
  Hello message (fixes CVE-2021-42074).
  This bug allowed an unauthenticated attacker to crash Barrier with only network access.

All of the above security issues have been reported by Matthias Gerstner who was really helpful
resolving them.

Bug fixes
---------

- Fixed a bug in SSL implementation that caused invalid data occasionally being sent to clients
  under heavy load.
