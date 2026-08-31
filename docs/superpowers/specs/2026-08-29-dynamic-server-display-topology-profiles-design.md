# Dynamic Server Display-Topology Profiles Design

Issue: #13

## Goal

A macOS Barrier server must react safely when a MacBook’s active displays change at runtime. Every exact server display topology may have its own saved freeform layout. Known topologies apply automatically without restarting connected clients. Unknown topologies keep sessions alive but expose no cross-screen transitions until the user saves a layout.

A persistently closed-lid MacBook with no active external display is not a usable Barrier server. It becomes unavailable and releases clients rather than retaining stale geometry.

## Relationship to proximity gating

This feature and issue #12 are separate state machines.

- Display-topology management owns active server displays and valid cross-screen links.
- Proximity management owns whether a macOS client process should run.
- Their only shared contract is server display readiness: at least one active server display must exist before an automatic client connection is allowed.

An unknown but non-empty topology is display-ready. Clients may connect so that the freeform editor has current client metadata, but switching remains disabled until the topology profile is saved.

## Required server states

The design covers all four MacBook display states:

1. Lid closed with no active external display.
2. Lid closed with one or more active external displays.
3. Lid open with only the built-in display active.
4. Lid open with the built-in display and one or more external displays active.

It also covers displays being removed or added while Barrier is running, a laptop moving outside proximity after a display transition, and multiple external displays arriving sequentially.

## Current behavior

`OSXScreen` already registers `CGDisplayRegisterReconfigurationCallback()`. On relevant CoreGraphics flags it rebuilds a display snapshot from `CGGetActiveDisplayList()` and emits `IScreen::shapeChanged()`.

The current path is insufficient for dynamic profiles:

- `OSXScreen::updateScreenShape()` returns when the active display count is zero, leaving the previous geometry in memory.
- `Server::handleShapeChanged()` adjusts cursor coordinates but does not select a freeform profile or regenerate links from the new server topology.
- The GUI stores one set of freeform positions and display rectangles.
- The GUI serializes one static `links` section for the server process.
- CoreGraphics emits multiple callbacks during one physical reconfiguration, so acting on each callback can expose intermediate geometry.

The solution must replace stale links at their source. Merely repainting the freeform canvas would leave runtime routing incorrect.

## Architectural ownership

### Server core owns runtime selection

The `barriers` process owns topology settling, profile selection, link activation, pointer safety, and zero-display handling. This keeps routing decisions beside `Server`, which owns the active input screen and link map.

The Qt GUI owns profile authoring, persistence UI, macOS notifications, and menu-bar presentation. It does not independently decide which runtime links are active.

This boundary avoids two competing topology state machines. It also lets the server remain safe if the GUI is temporarily unavailable: unknown topology still means no switching, and zero displays still release clients.

### Status bridge

The server emits a stable, structured topology-status record through the existing daemon-to-GUI log/IPC stream. The record contains:

- state: `reconfiguring`, `known`, `unknown`, or `unavailable`;
- topology identifier when non-empty;
- canonical topology description for GUI matching and display;
- active display count;
- whether switching is enabled.

The GUI parser accepts only the exact emitted format. Malformed or partial records do not mutate GUI state. Runtime safety never depends on the GUI parsing this record; it is a presentation and Bonjour-publication bridge only.

The GUI maps the state to Bonjour `display-ready`:

- `known` or `unknown`: `1`;
- `reconfiguring` or `unavailable`: `0`.

## Display topology snapshot

The macOS platform snapshot must capture each active display atomically as one record:

- stable CoreGraphics display UUID;
- current `CGDirectDisplayID` for runtime access only;
- logical bounds;
- rotation;
- product name for presentation;
- whether the display is the current primary display.

`CGDirectDisplayID` is not persisted because it may change. The stable UUID is the profile identity.

`getDisplays()`, display names, and topology identity must read the same ordered snapshot. They must not query CoreGraphics independently and risk mismatched counts or ordering.

A zero-display query is a valid snapshot. It clears the current active snapshot rather than preserving previous geometry.

## Exact normalized topology identity

A topology is identified by a versioned canonical representation.

1. Record the stable UUID of the primary display.
2. Subtract the primary display origin from every display’s origin.
3. For each display, record stable UUID, normalized `x`, normalized `y`, logical width, logical height, and rotation.
4. Sort display records by stable UUID so callback enumeration order does not affect identity.
5. Serialize with an explicit format version.
6. Hash the canonical representation with SHA-256 to produce the topology identifier stored as lowercase hexadecimal.

The profile stores both the identifier and canonical representation. On load, Barrier recomputes and verifies the identifier before accepting the profile.

The following create a distinct topology:

- a display is added or removed;
- the primary display changes;
- relative arrangement changes;
- logical resolution changes;
- rotation changes.

A translation applied equally to all global CoreGraphics coordinates does not create a new topology.

## Profile model

A topology profile is keyed only by the server topology identifier. It does not include the set of currently connected clients.

Each profile stores:

- the canonical server topology representation;
- the freeform origin of every configured Barrier screen;
- the latest saved display rectangles for configured client screens;
- profile creation and last-update schema versions.

The local server origin is always normalized to `(0, 0)`. Current live server display rectangles come from the settled platform snapshot; they are not replaced by stale saved rectangles.

When a known profile activates, the server combines:

- current live server display rectangles;
- current connected-client display rectangles received through `DDIS` when available;
- saved client display rectangles as the fallback for configured but currently absent clients;
- the selected profile’s screen origins.

It derives a complete candidate link map off to the side, validates all referenced screens and non-empty edge intervals, then swaps the map atomically. Existing links remain inaccessible during reconfiguration, so no caller sees a partially rebuilt layout.

Only connected screens participate in live switching. Client arrival or departure does not create another server topology profile.

## Configuration persistence

The Barrier configuration format gains a topology-profile section that can represent multiple profiles. Existing configurations without that section remain readable, but their classic links are not silently promoted to an exact topology profile. On the first non-empty topology, Barrier enters `StableUnknown`, offers the legacy geometry as an editable starting point in the freeform editor, and requires an explicit save before switching is enabled.

The serialized model contains versioned topology identity, per-screen origins, and saved client display rectangles. It does not persist transient `CGDirectDisplayID` values.

The GUI writes all profiles, not only the currently active profile. Saving a freeform layout updates only the matching topology entry and preserves every other profile.

After a save, the GUI invokes the server’s existing configuration-reload path:

- desktop mode signals the owned `barriers` process to reload;
- service mode uses an explicit IPC reload command rather than restarting the service.

`ServerApp::reloadConfig()` validates the complete replacement configuration before calling `Server::setConfig()`. If parsing or validation fails, the previous configuration and runtime profile remain active and the GUI reports the save failure.

Older Barrier binaries are not required to read the new topology-profile section. New binaries must continue reading existing classic `screens`, `links`, `aliases`, and `options` sections.

## Runtime state machine

### `StableKnown`

At least one active display exists and an exact profile matches. The selected link map is active, switching is enabled, and `display-ready=1` is published.

### `StableUnknown`

At least one active display exists but no profile matches. Client sessions remain connected, all cross-screen transitions are disabled, and `display-ready=1` is published. The GUI presents `Layout required`.

### `Reconfiguring`

A relevant CoreGraphics callback has arrived. Cross-screen transitions are disabled immediately, and a two-second one-shot settle timer starts. Every additional callback replaces that timer.

If input is active on a client, Barrier returns control to the current server primary display before swapping or clearing links. No new transition away from the server is allowed during this state.

The state publishes `display-ready=0`, preventing a new proximity-gated client from starting during an intermediate topology. Existing connected sessions remain alive.

### `NoDisplayGrace`

A settled snapshot contains zero active displays. All links remain disabled and a ten-second grace timer begins. This absorbs transient zero-display states while macOS changes clamshell or display modes.

If a non-empty topology returns during the grace period, the timer is cancelled and the normal known/unknown selection path resumes.

### `Unavailable`

Zero active displays persisted through the grace period. The server disconnects clients so their local input is restored, publishes `display-ready=0`, and retains no active routing geometry. A later non-empty topology can return directly to `StableKnown` or `StableUnknown` without restarting the server process.

All transitions and timer cancellation are idempotent.

## Pointer and input safety

Display reconfiguration must never leave control trapped on a client or route through stale edges.

- The first topology-change event disables new cross-screen transitions.
- If a server display remains available, active remote input is returned to the current primary server display at a clamped safe coordinate.
- A known replacement profile is built and validated before becoming visible.
- An unknown topology exposes an empty cross-screen link map.
- A persistent zero-display state disconnects clients, causing each client to restore local input behavior.

The previous profile is never used temporarily for a different topology.

## Unknown-topology UX

The GUI posts one macOS notification per unknown topology identifier and marks the menu-bar state `Layout required`.

Clicking the notification or menu item opens the freeform editor scoped to the current topology. The editor shows:

- the current live server display geometry;
- all configured client screens;
- current client display metadata when connected;
- saved client metadata when a client is absent.

Saving creates or replaces only the current topology profile, reloads configuration, and activates the profile immediately if the topology is still current. No client or server restart is required.

The notification is not repeated for every CoreGraphics callback. It may appear again only after the topology was configured and its profile was later removed or became invalid.

## Scenario behavior

### Lid closed, no external display

A transient zero-display snapshot enters `NoDisplayGrace`. If no display returns within ten seconds, the server enters `Unavailable`, disconnects clients, and publishes `display-ready=0`. A nearby client waits without starting or retrying.

### Lid closed, external displays active

The external-only topology settles. A matching profile applies automatically. An unknown external-only topology keeps sessions connected but disables switching and requests freeform configuration.

### Lid open, built-in display only

The built-in-only topology selects its exact profile. Removing external monitors while the lid remains open therefore transitions safely to this profile rather than to unavailable state.

### Lid open with external displays

The combined built-in-plus-external topology selects its own exact profile. Closing the lid removes the built-in display and selects a different external-only profile.

### Displays removed before the laptop moves away

Display topology and proximity remain independent. Display removal may select another profile or make the server unavailable. Later proximity loss may stop the client as well. Repeated stop or disconnect requests are harmless.

### Displays added after a closed-lid laptop arrives

With no active display, clients wait. The first external display creates an external-only topology. Later displays create new exact topologies after each settles.

### Displays added after an open-lid laptop arrives

The built-in-only profile remains active until reconfiguration begins. Each stable combined topology selects its exact profile. Rapid callbacks are collapsed by the settle timer.

### External displays connected sequentially

If additions occur within the two-second quiet period, only the final topology is exposed. If one arrangement remains stable longer than the quiet period, it is a legitimate topology and selects its own profile or requests configuration before the next monitor arrives.

## Failure handling

- **CoreGraphics query fails:** remain in `Reconfiguring`, keep switching disabled, retry snapshot capture with bounded delay, and report the platform error internally.
- **Zero displays:** treat as valid state and apply the grace policy; never retain stale geometry.
- **Profile hash mismatch or malformed profile:** reject that profile and treat the topology as unknown.
- **Candidate link validation fails:** keep the empty safe map and report `Layout required`; do not partially apply links.
- **Configuration reload fails:** retain the previous complete configuration and show a non-destructive GUI error.
- **GUI absent:** core safety behavior continues; notification and Bonjour status wait until the GUI/status bridge returns.
- **Duplicate callbacks:** reset the settle timer without duplicating notifications or map swaps.

## Compatibility

- Existing Barrier network protocol behavior remains unchanged.
- Existing `DINF`, `DDIS`, and `DDNM` behavior remains unchanged.
- Existing configuration files remain readable.
- Dynamic profiles are local server configuration, not peer-negotiated protocol data.
- Non-macOS platforms may continue reporting one bounding-box topology and do not need to implement CoreGraphics identity.

## Verification

### Deterministic tests

- Canonical signatures ignore enumeration order and global translation.
- Signatures change for display membership, primary display, relative position, logical size, and rotation.
- Zero-display snapshots clear previous geometry.
- Reconfiguration callbacks debounce into one settled transition.
- Known profiles build and atomically activate the expected partial-edge links.
- Unknown profiles expose no cross-screen links while preserving connected sessions.
- Zero-display grace cancels when a display returns and disconnects only after sustained absence.
- Malformed profiles and invalid link maps fail closed.
- Profile persistence round-trips multiple topologies without overwriting unrelated entries.
- Configuration reload failure preserves the last valid runtime configuration.
- One unknown topology produces one notification state transition.

### macOS surface scenarios

1. Open lid, built-in only: save and automatically reselect its profile.
2. Add one external display: switching pauses; known combined profile activates or notification opens the correct editor.
3. Add a second monitor quickly: intermediate callbacks do not expose stale links.
4. Add a second monitor after the first topology settled: each stable topology selects independently.
5. Remove externals with lid open: built-in-only profile returns automatically.
6. Close lid with an external active: external-only profile activates.
7. Remove the last external while lid remains closed: clients disconnect after grace and menu status becomes unavailable.
8. Restore an external display: server becomes ready and selects the exact profile without process restart.
9. Enter an unknown topology while a client is connected: session remains, pointer returns safely, and switching is disabled.
10. Save the current unknown layout: links activate live without reconnecting peers.

Surface proof must include current macOS display geometry, menu-bar state, server topology-status output, connected process state, and actual pointer transitions at every enabled edge.

## Non-goals

- Creating a virtual display for a headless or closed-lid MacBook.
- Guessing or fuzzily adapting a layout that has no exact profile.
- Keying profiles by connected-client combinations.
- Managing dynamic client-side monitor profiles in this issue.
- Changing the Barrier peer wire protocol.
- Keeping stale routing active during display reconfiguration.
