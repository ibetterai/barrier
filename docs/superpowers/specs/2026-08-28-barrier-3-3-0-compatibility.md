# Barrier 3.3.0 Compatibility Design

## Goal

Release Barrier 3.3.0 with the current 3.2.0 feature set while restoring interoperability with widely deployed Barrier 2.4.0 peers.

## Compatibility contract

Barrier 3.3.0 must negotiate capability per peer connection.

- A 3.3.0 client connected to a 2.4.0 server must announce protocol 1.6 and send only protocol-1.6 messages.
- A 2.4.0 client connected to a 3.3.0 server must remain accepted as protocol 1.6 and must not be required to send display geometry or display names.
- A 3.3.0 client connected to a 3.3.0 server may negotiate protocol 1.7 and use enhanced display geometry and display names.
- A 2.4.0 peer never receives 3.x-only wire messages.
- A 2.4.0 peer remains rectangular because it only exposes `DINF` bounding-box information.

## Feature retention

Barrier 3.3.0 keeps the 3.2.0 behavior set when both peers support it:

- native Apple Silicon macOS build and packaging
- dynamic physical display geometry
- non-rectangular and L-shaped routing
- display names
- target display wake-on-entry
- consistent two-finger scrolling
- Magic Mouse two-finger Spaces swipe forwarding
- existing crash fixes and macOS reliability fixes

Enhanced behavior is optional. Interoperability with 2.4.0 takes priority over sending every 3.x feature to every peer.

## Protocol design

Protocol 1.6 is the Barrier 2.4.0 baseline. Protocol 1.7 introduces 3.x display metadata:

- `DDIS` / `kMsgDDisplayInfo`: per-display rectangles
- `DDNM` / `kMsgDDisplayNames`: per-display names matching the preceding `DDIS` rectangle order

Rules:

1. `DINF` is always sent. It is the compatibility shape and carries the peer's bounding box.
2. `DDIS` is sent only to peers that negotiated display-geometry support.
3. `DDNM` is sent only to peers that negotiated display-name support and only after a matching `DDIS`.
4. Unknown protocol extensions must never be sent to peers that did not negotiate them.
5. Capability predicates should be named by feature, not inferred directly from the current maximum protocol minor.

Required helper shape:

```cpp
namespace barrier {
bool supportsDisplayGeometry(SInt16 minorVersion);
bool supportsDisplayNames(SInt16 minorVersion);
SInt16 negotiatedMinorVersion(SInt16 serverMinor);
}
```

`supportsDisplayGeometry(6)` and `supportsDisplayNames(6)` return false. `supportsDisplayGeometry(7)` and `supportsDisplayNames(7)` return true.

## Mixed-fidelity server layout

A 3.3.0 server can host mixed clients in one layout. Capability is per peer; layout is global.

The server's layout graph contains one screen object per configured screen:

- local 3.3.0 server screen: physical display rectangles are known locally
- 3.3.0 client: physical display rectangles are known from `DDIS`
- 2.4.0 client: no physical display rectangles are known; synthesize one rectangle from `DINF`

Synthetic rectangle rule:

```cpp
if (client did not send DDIS) {
    displays = {{ info.m_x, info.m_y, info.m_w, info.m_h }};
}
```

This produces a mixed-fidelity layout:

| Transition | Source model | Destination model | Expected routing |
|---|---|---|---|
| 3.3.0 server to 3.3.0 client | physical displays | physical displays | full partial-edge routing |
| 3.3.0 client to 3.3.0 server | physical displays | physical displays | full partial-edge routing |
| 3.3.0 server to 2.4.0 client | physical displays | synthetic rectangle | display-aware source edge, rectangular destination |
| 2.4.0 client to 3.3.0 server | synthetic rectangle | physical displays | rectangular source edge, display-aware destination |
| 3.3.0 client to 2.4.0 client | physical displays | synthetic rectangle | display-aware source edge, rectangular destination |
| 2.4.0 client to 3.3.0 client | synthetic rectangle | physical displays | rectangular source edge, display-aware destination |

The weaker side only weakens that side of a transition. One 2.4.0 client must not globally disable enhanced routing between 3.3.0 peers.

## Configuration design

Existing `barrier.conf` files must remain valid.

- Continue writing classic `screens`, `links`, `aliases`, and `options` sections.
- Keep freeform geometry and display metadata out of public release documentation unless represented generically.
- Do not commit machine-specific names, hostnames, local IP addresses, internal role names, or private deployment mapping.
- Public docs may use neutral names such as `Server`, `Client A`, `Client B`, `Old Client`, and `Enhanced Client`.

## Release design

- Version becomes `3.3.0-release`.
- App name remains Barrier for drop-in upgrades.
- Release notes state that 3.3.0 restores interoperability with Barrier 2.4.0 peers by suppressing 3.x-only wire messages unless negotiated.
- Release artifacts and public docs must not include environment-specific deployment details.

## Verification matrix

Run these before release:

1. 3.3.0 client to 2.4.0 server
   - connects
   - no invalid-message disconnect
   - mouse movement works
   - keyboard input works
   - clipboard round-trip works
2. 2.4.0 client to 3.3.0 server
   - connects
   - server accepts protocol 1.6
   - rectangular routing works
   - keyboard and mouse work
3. 3.3.0 client to 3.3.0 server
   - connects with enhanced protocol
   - `DDIS` remains sent
   - `DDNM` remains sent when display names exist
   - L-shaped routing still works
4. Mixed-client 3.3.0 server layout
   - one old client uses synthetic rectangle fallback
   - one enhanced client uses physical display rectangles
   - enhanced server-to-enhanced-client routing remains display-aware
   - old-client transitions remain rectangular on the old side

## Non-goals

- Do not implement new 2.4.0 protocol extensions.
- Do not attempt to infer the physical monitor layout of 2.4.0 peers.
- Do not rename the application.
- Do not add private deployment details to tracked files.
