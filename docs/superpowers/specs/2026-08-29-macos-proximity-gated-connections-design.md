# macOS Proximity-Gated Connections Design

Issue: #12

## Goal

An opted-in macOS Barrier client must start only while its explicitly paired server is physically near, discoverable through Bonjour, and ready to host input. When those conditions are absent, the client waits without opening a TCP connection, logging connection failures, or entering a restart loop.

“Near” means the calibrated Bluetooth signal expected when the two Macs occupy their intended desk positions. It is a presence heuristic, not a measured physical distance or an authentication mechanism.

## Scope

- macOS client and macOS server only.
- Opt-in per client/server pair.
- One explicitly paired server controls each client’s automatic connection.
- The Barrier menu-bar application owns sensing and the child-process lifecycle. The main window may remain hidden.
- Existing Barrier TLS fingerprint verification remains authoritative.
- Existing users, command-line-only users, and non-macOS platforms keep the current connection behavior.

## Current behavior

Three existing behaviors combine to create continuous failed attempts:

1. `ClientApp::nextRestartTimeout()` returns one second.
2. `barrierc` is restartable by default.
3. `MainWindow::barrierFinished()` restarts a child that exits while the GUI still expects Barrier to run.

Barrier already browses Bonjour services, but service removal is not propagated into the client lifecycle: `ZeroconfBrowser` removes records, while `ZeroconfService` and `MainWindow` only add detected servers to the UI.

The feature must therefore control whether `barrierc` exists. Suppressing log messages or changing retry text would leave the unwanted network activity intact.

## Architecture

### Proximity advertiser

The server-side macOS menu-bar application owns a `ProximityAdvertiser` backed by CoreBluetooth peripheral APIs.

It advertises a fixed Barrier proximity service UUID. A read-only GATT characteristic exposes a random 128-bit proximity identifier generated on first enablement and persisted in application settings. The identifier is opaque and contains no hostname, username, IP address, TLS fingerprint, or other personal data.

The advertiser runs only while Barrier is enabled as a server. Quitting the menu-bar application stops advertising. Hiding the main window does not.

### Proximity monitor

The client-side macOS menu-bar application owns a `ProximityMonitor` backed by CoreBluetooth central APIs.

It scans only for the Barrier proximity service UUID and enables duplicate discovery events so that RSSI can be sampled over time. CoreBluetooth callbacks are reduced to typed state updates; they do not start or stop `barrierc` directly.

The monitor persists:

- the server’s opaque proximity identifier;
- the CoreBluetooth peripheral identifier observed during pairing;
- the calibrated entry and exit RSSI thresholds;
- whether proximity gating is enabled for the pair.

The CoreBluetooth peripheral identifier is an OS-local lookup key, not durable security identity. If it changes and the monitor cannot match the paired server, the client fails closed and asks the user to pair again. It never treats a different nearby Barrier advertiser as the paired server.

### Bonjour readiness

The existing Bonjour record remains the network-readiness signal. Browsing must expose both additions and removals to the connection policy.

A proximity-enabled server publishes two TXT values:

- `proximity-id`: the lowercase hexadecimal form of the same opaque 128-bit identifier exposed by the BLE characteristic;
- `display-ready=1`: at least one active server display exists;
- `display-ready=0`: no active server display is available.

The client accepts network readiness only from the Bonjour record whose `proximity-id` matches the explicitly paired BLE server. This prevents a different Barrier server on the LAN from satisfying the network gate. A missing, malformed, or mismatched identifier is not treated as the paired server, and a missing or malformed readiness value is not treated as ready. Issue #13 owns generation of display readiness; issue #12 owns the identity match and consumes readiness as a connection condition.

Bonjour does not prove physical proximity. Bluetooth does not prove TCP reachability. Both are required.

### Connection policy

A single `ProximityConnectionPolicy` consumes sensor state and owns the desired child-process state. No CoreBluetooth, Bonjour, window, or process callback bypasses this policy.

Automatic connection is allowed only when all conditions are true:

```text
feature enabled
AND paired BLE server is near
AND paired Bonjour server is present
AND display-ready = 1
AND user has requested Barrier to run
```

The policy launches `barrierc` with `--no-restart`. The GUI’s existing process-finished path must consult the policy before scheduling another launch. A failed child is not restarted until the complete gate becomes true again.

Desktop mode starts and stops the owned `QProcess`. Service mode sends the equivalent start/stop commands through the existing Barrier daemon IPC path. Both modes expose the same policy states and user-visible status.

## Pairing and calibration

Pairing is initiated explicitly from the client GUI.

1. The client lists currently discovered Barrier proximity advertisers.
2. The user selects the intended server.
3. The client connects through CoreBluetooth and reads the server’s opaque proximity identifier.
4. The client finds the Bonjour record with the same `proximity-id` and binds the record's resolved host plus Barrier's configured data port to the pair. The DNS-SD SRV port belongs to Barrier's auxiliary discovery service and is not the Barrier protocol port.
5. Existing Barrier TLS trust remains separate and is not granted by BLE pairing.
6. With both Macs in their intended desk positions, the client samples RSSI for ten seconds.
7. Calibration succeeds only after at least twenty valid RSSI samples. CoreBluetooth’s invalid RSSI sentinel is discarded.
8. Let `R` be the median valid sample. The entry threshold is `R - 6 dB`; the exit threshold is `R - 12 dB`.
9. The pair and thresholds are persisted only after all steps succeed.

A later recalibration replaces the thresholds atomically. Cancelled or failed calibration leaves the previous pair unchanged.

## RSSI filtering and hysteresis

The monitor maintains an exponentially weighted moving average with alpha `0.25` over valid samples from the paired peripheral.

- **Enter:** the filtered RSSI must meet or exceed the entry threshold for three consecutive samples.
- **Stay near:** any valid sample above the exit threshold refreshes presence.
- **Leave:** no qualifying sample may arrive for ten continuous seconds.

The ten-second departure grace absorbs advertisement loss, radio scheduling gaps, and brief obstruction. Entry and exit thresholds are deliberately different so that a signal near the boundary does not repeatedly start and stop Barrier.

Calibration communicates “desk sensitivity,” not meters. The UI must not claim a precise physical distance.

## State model

| State | Meaning | Child process |
|---|---|---|
| `Disabled` | Proximity mode is not enabled | Existing Barrier behavior |
| `WaitingForPermission` | Bluetooth authorization is unresolved | Stopped |
| `SensorUnavailable` | Permission denied, Bluetooth off, or sensing failed | Stopped |
| `WaitingForPeer` | Paired BLE server is not stably near | Stopped |
| `WaitingForNetwork` | BLE is near but Bonjour/readiness is absent | Stopped |
| `Starting` | Full gate became true and launch is in progress | Starting |
| `Connected` | Barrier client is connected | Running |
| `DepartureGrace` | A required signal was lost transiently | Running until grace expires |
| `ManualOverride` | User explicitly bypassed BLE for this GUI session | Governed by Bonjour and display readiness |

Every transition is idempotent. Repeated sensor callbacks cannot launch a second child or schedule duplicate stop operations.

## Manual override

When Bluetooth is unavailable, the status reads `Paused: proximity unavailable` and explains the specific authorization or radio state without displaying a connection error.

`Connect Anyway` creates a session-scoped override that bypasses only the BLE condition. Bonjour presence, `display-ready=1`, and the user’s run intent remain mandatory, so the override does not reintroduce blind TCP retries.

The override is cleared when the menu-bar application quits, the user disables Barrier, or the user selects `Resume Proximity`. It is never persisted across launches.

## User-visible behavior

The menu-bar status distinguishes:

- waiting for nearby server;
- nearby, waiting for network;
- nearby, waiting for an active server display;
- connecting;
- connected;
- departure grace;
- proximity unavailable;
- manual override.

Sensor transitions do not open the main window. Permission prompts, explicit pairing, recalibration, and manual override are the only proximity actions that require foreground interaction.

## Failure handling

- **Bluetooth denied or off:** fail closed; do not launch; expose manual override.
- **Paired peripheral identifier no longer matches:** fail closed and require re-pairing.
- **Bonjour disappears:** enter departure grace, then stop the client.
- **Server reports no active display:** enter departure grace, then stop the client.
- **Child exits while the complete gate remains true:** one policy-controlled restart may occur after the normal launch delay.
- **Child exits while any gate is false:** remain stopped with a waiting status.
- **Malformed BLE or Bonjour metadata:** ignore it and remain stopped.
- **Menu-bar application quits:** stop scanning; normal application shutdown owns child cleanup.

No failure silently falls back to the current one-second retry loop.

## Security and privacy

BLE presence is advisory only. It must not:

- authenticate the Barrier network peer;
- approve or replace a TLS fingerprint;
- disclose hostnames, IP addresses, usernames, or TLS material in advertisements;
- connect to an unpaired advertiser merely because its RSSI is stronger.

A malicious nearby advertiser can affect availability but cannot gain Barrier trust. Existing encrypted and authenticated network connection behavior remains unchanged.

## Compatibility

- No Barrier wire-protocol message changes.
- No change to default `barrierc` behavior outside proximity mode.
- No new requirement for older peers unless the user enables proximity mode.
- A proximity-enabled client pairs only with a server build that advertises the BLE service and publishes explicit display readiness.

## Verification

### Deterministic tests

- Policy truth table covers every combination of enablement, BLE presence, Bonjour presence, display readiness, user intent, and override.
- RSSI filtering rejects invalid samples and applies entry/exit hysteresis.
- Departure grace is cancelled when all signals recover before expiry.
- Repeated callbacks never create duplicate starts, stops, or timers.
- Process exit restarts only when the full gate remains true.
- Manual override bypasses BLE only and never persists.
- Bonjour removal removes the server from current discovery state.

### macOS integration scenarios

1. Paired server absent: no `barrierc` process and no TCP attempts.
2. Server physically near but off the LAN: wait for network without a TCP attempt.
3. Server on the LAN but outside desk threshold: wait for proximity.
4. Both signals become stable: one client launch and successful connection.
5. Brief RSSI loss below threshold: connection remains.
6. Sustained physical departure: client stops after grace and remains idle.
7. Main window hidden: sensing and automatic lifecycle continue.
8. Menu-bar application quit: sensing stops.
9. Bluetooth denied: paused status and explicit session override.
10. Unknown nearby Barrier advertiser: ignored.

Surface verification must observe the menu-bar status, actual `barrierc` process state, and network-attempt logs together.

## Non-goals

- Estimating physical distance in meters.
- Supporting Windows or Linux proximity sensing.
- Allowing the server to gate accepted clients through mutual BLE sensing.
- Replacing Barrier TLS authentication or trust prompts.
- Connecting to the strongest arbitrary Barrier server.
- Running sensing after the Barrier menu-bar application has quit.
