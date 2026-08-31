# Dynamic Server Display-Topology Profiles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect the macOS server’s exact active-display topology at runtime, select only an exact saved freeform profile, disable switching for unknown topologies, and disconnect clients when no display remains.

**Status:** Implemented and verified for issue #13. The checkboxes below retain
the original execution sequence rather than acting as a live task tracker.

**Architecture:** CoreGraphics produces a normalized, stable-ID topology snapshot. A pure server-side state machine debounces reconfiguration and distinguishes known, unknown, and unavailable states. `Config` stores multiple immutable topology profiles and atomically swaps the active link map. The GUI authors profiles, parses structured server status, notifies once per unknown topology, and publishes `display-ready` through Bonjour.

**Tech Stack:** C++14, Objective-C++, CoreGraphics/ApplicationServices, OpenSSL EVP SHA-256, Qt 5, Bonjour DNS-SD, CMake, GoogleTest.

**Ordering with issue #12:** Complete Tasks 1–6 first. Issue #12’s resolved TXT registration work must land before Task 7 publishes `display-ready`; #12’s client lifecycle integration then consumes that field.

---

## Files

### Create

- `src/lib/base/Sha256.h`
- `src/lib/base/Sha256.cpp`
- `src/lib/barrier/DisplayTopology.h`
- `src/lib/barrier/DisplayTopology.cpp`
- `src/lib/server/DisplayTopologyStateMachine.h`
- `src/lib/server/DisplayTopologyStateMachine.cpp`
- `src/gui/src/TopologyProfileStore.h`
- `src/gui/src/TopologyProfileStore.cpp`
- `src/gui/src/TopologyStatusParser.h`
- `src/gui/src/TopologyStatusParser.cpp`
- `src/test/unittests/base/Sha256Tests.cpp`
- `src/test/unittests/barrier/DisplayTopologyTests.cpp`
- `src/test/unittests/server/DisplayTopologyStateMachineTests.cpp`
- `src/test/unittests/barrier/TopologyProfileConfigTests.cpp`
- `src/test/unittests/server/ServerTests.cpp`
- `src/gui/test/TopologyProfileStoreTests.cpp`
- `src/gui/test/TopologyStatusParserTests.cpp`

### Modify

- `src/lib/base/CMakeLists.txt`
- `src/lib/barrier/CMakeLists.txt`
- `src/lib/barrier/IPlatformScreen.h`
- `src/lib/barrier/IScreen.h`
- `src/lib/barrier/Screen.h`
- `src/lib/barrier/Screen.cpp`
- `src/lib/platform/OSXScreen.h`
- `src/lib/platform/OSXScreen.mm`
- `src/lib/server/PrimaryClient.h`
- `src/lib/server/PrimaryClient.cpp`
- `src/lib/server/Server.h`
- `src/lib/server/Server.cpp`
- `src/lib/barrier/Config.h`
- `src/lib/barrier/Config.cpp`
- `src/lib/barrier/ServerApp.cpp`
- `src/lib/ipc/IpcMessage.h`
- `src/lib/ipc/IpcMessage.cpp`
- `src/lib/ipc/IpcClientProxy.h`
- `src/lib/ipc/IpcClientProxy.cpp`
- `src/gui/src/Ipc.h`
- `src/gui/src/Ipc.cpp`
- `src/gui/src/IpcClient.h`
- `src/gui/src/IpcClient.cpp`
- `src/gui/src/ServerConfig.h`
- `src/gui/src/ServerConfig.cpp`
- `src/gui/src/FreeformLayoutSettings.h`
- `src/gui/src/FreeformLayoutSettings.cpp`
- `src/gui/src/FreeformServerConfigWidget.h`
- `src/gui/src/FreeformServerConfigWidget.cpp`
- `src/gui/src/ServerConfigDialog.h`
- `src/gui/src/ServerConfigDialog.cpp`
- `src/gui/src/MainWindow.h`
- `src/gui/src/MainWindow.cpp`
- `src/gui/src/ZeroconfRegister.h`
- `src/gui/src/ZeroconfRegister.cpp`
- `src/gui/src/ZeroconfService.h`
- `src/gui/src/ZeroconfService.cpp`
- `src/gui/CMakeLists.txt`
- `src/test/unittests/CMakeLists.txt`
- `doc/newsfragments/13.feature`

## Task 1: Canonical topology identity and SHA-256 key

**Files:**
- Create: `src/lib/base/Sha256.h`
- Create: `src/lib/base/Sha256.cpp`
- Create: `src/lib/barrier/DisplayTopology.h`
- Create: `src/lib/barrier/DisplayTopology.cpp`
- Create: `src/test/unittests/base/Sha256Tests.cpp`
- Create: `src/test/unittests/barrier/DisplayTopologyTests.cpp`
- Modify: `src/lib/base/CMakeLists.txt`
- Modify: `src/lib/barrier/CMakeLists.txt`
- Modify: `src/test/unittests/CMakeLists.txt`

- [ ] **Step 1: Add a failing SHA-256 known-vector test**

```cpp
TEST(Sha256Tests, hashesKnownVector)
{
    EXPECT_EQ(barrier::sha256Hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");
}
```

Implement the helper with OpenSSL EVP, return lowercase hexadecimal, and throw on digest initialization/update/finalization failure. Do not add a second cryptographic implementation.

- [ ] **Step 2: Define topology value types and add failing normalization tests**

```cpp
struct DisplayTopologyEntry {
    std::string stableId;
    ScreenRect logicalBounds;
    int rotationDegrees;
    bool primary;
};

struct DisplayTopology {
    static const int kIdentityVersion = 1;
    std::vector<DisplayTopologyEntry> displays;

    bool empty() const;
    DisplayTopology normalized() const;
    std::string canonicalIdentity() const;
    std::string profileKey() const;
};
```

Tests must prove:

- translating all screens by the same offset does not change the key;
- changing relative geometry, logical size, rotation, stable ID, or primary display changes the key;
- input order does not change the key;
- negative relative coordinates survive normalization;
- duplicate stable IDs, two primaries, no primary, zero/negative dimensions, and non-quarter-turn rotations are rejected;
- empty topology is valid as an unavailable observation but has no profile key.

- [ ] **Step 3: Run RED**

```sh
cmake --build build-arm64 --target unittests -j$(nproc)
```

Expected: compilation fails because the SHA and topology types do not exist.

- [ ] **Step 4: Implement canonicalization**

Normalize every rectangle by subtracting the primary display’s origin, sort by `stableId`, and serialize a version-prefixed ASCII representation with explicit field separators and signed decimal coordinates. Hash only the canonical bytes. Never hash display names or transient `CGDirectDisplayID` values.

- [ ] **Step 5: Run focused tests and commit**

```sh
cmake --build build-arm64 --target unittests -j$(nproc)
./build-arm64/bin/unittests \
  --gtest_filter='Sha256Tests.*:DisplayTopologyTests.*'
git add src/lib/base/Sha256.* src/lib/barrier/DisplayTopology.* \
  src/test/unittests/base/Sha256Tests.cpp \
  src/test/unittests/barrier/DisplayTopologyTests.cpp \
  src/lib/base/CMakeLists.txt src/lib/barrier/CMakeLists.txt \
  src/test/unittests/CMakeLists.txt
git commit -m "feat: add stable display topology identity #13"
```

## Task 2: Surface live topology through the screen stack

**Files:**
- Modify: `src/lib/barrier/IPlatformScreen.h`
- Modify: `src/lib/barrier/IScreen.h`
- Modify: `src/lib/barrier/Screen.h`
- Modify: `src/lib/barrier/Screen.cpp`
- Modify: `src/lib/platform/OSXScreen.h`
- Modify: `src/lib/platform/OSXScreen.mm`
- Modify: `src/lib/server/PrimaryClient.h`
- Modify: `src/lib/server/PrimaryClient.cpp`
- Modify: `src/test/unittests/platform/OSXScreenTests.cpp`

- [ ] **Step 1: Run symbol references before changing screen interfaces**

Use LSP/Serena references for `IPlatformScreen::getDisplays`, `IScreen::getDisplays`, `Screen::getDisplays`, and `PrimaryClient::getDisplays`. Update every implementation or test double in this task; do not add a parallel macOS-only side channel.

- [ ] **Step 2: Add the virtual method with a safe non-macOS default**

```cpp
virtual DisplayTopology getDisplayTopology() const;
```

The `IPlatformScreen` default converts current display rectangles to a deterministic single virtual-desktop identity suitable for existing non-macOS behavior. `Screen` delegates to the platform screen. `PrimaryClient` exposes the current primary-screen topology directly to `Server`; remote client proxies and the wire protocol do not gain this method.

- [ ] **Step 3: Extract and test the CoreGraphics-record conversion**

Create a file-local pure builder that accepts records containing stable ID, bounds, rotation, and primary flag. Add tests for all four laptop forms:

1. internal display only;
2. internal plus external;
3. external only with lid closed;
4. no active display.

Also test reorder stability and primary-display changes.

- [ ] **Step 4: Build the macOS snapshot**

In `OSXScreen::updateScreenShape()`:

- call `CGGetActiveDisplayList`;
- derive stable IDs with `CGDisplayCreateUUIDFromDisplayID` and canonical lowercase UUID text;
- read logical bounds with `CGDisplayBounds`;
- read rotation with `CGDisplayRotation`, rounded only after validating it is within tolerance of 0/90/180/270;
- set primary from `CGMainDisplayID()`;
- replace `m_displays` and `m_displayTopology` together under the existing platform-screen synchronization rules.

When active display count is zero, explicitly clear `m_displays`, set shape to `0,0 0x0`, clear the topology, and emit `shapeChanged`. Remove the current early return that preserves stale geometry.

- [ ] **Step 5: Preserve reconfiguration event coverage**

Continue handling begin/move/mode/add/remove/enabled/disabled/mirror/shape flags. `OSXScreen` reports every refreshed snapshot; debounce belongs to the server state machine, not the platform callback.

- [ ] **Step 6: Run tests and commit**

```sh
cmake --build build-arm64 --target unittests barrier -j$(nproc)
./build-arm64/bin/unittests \
  --gtest_filter='DisplayTopologyTests.*:OSXScreenTests.*'
git add src/lib/barrier/IPlatformScreen.h src/lib/barrier/IScreen.h \
  src/lib/barrier/Screen.* src/lib/platform/OSXScreen.* \
  src/lib/server/PrimaryClient.* src/test/unittests/platform/OSXScreenTests.cpp
git commit -m "feat: report exact macOS display topology #13"
```

## Task 3: Store multiple topology profiles in Barrier configuration

**Files:**
- Modify: `src/lib/barrier/Config.h`
- Modify: `src/lib/barrier/Config.cpp`
- Create: `src/test/unittests/barrier/TopologyProfileConfigTests.cpp`
- Modify: `src/test/unittests/CMakeLists.txt`

- [ ] **Step 1: Inspect all `Config` construction, copy, equality, read, write, and `generateFreeformLinks` callers with symbol references**

The profile collection and selected runtime links must survive every existing copy path. Update all callers rather than retaining a second flat layout convention.

- [ ] **Step 2: Add failing config-model tests**

```cpp
struct TopologyProfile {
    std::string key;
    DisplayTopology topology;
    std::map<std::string, ScreenPosition> screenPositions;
    std::map<std::string, std::vector<ScreenRect> > displayRects;
};
```

Tests must cover:

- two profiles round-trip through parser and writer;
- exact topology selects the matching profile;
- unknown and empty topology clear the active freeform link map;
- switching A → B → A restores only A’s links;
- malformed key/topology mismatches reject the config;
- duplicate profile keys reject the config;
- existing configs without profile sections remain readable but have no active topology profile;
- legacy links are exposed as an editor seed only and never auto-selected.

- [ ] **Step 3: Extend the configuration grammar**

Add one versioned `topology-profiles` section following existing `section: ...` / `end` parser conventions. Each profile records:

- the 64-character SHA-256 key;
- canonical topology version and entries;
- freeform screen origins;
- saved per-screen display rectangles.

Validate the stored key by recomputing it from the stored topology. Keep existing `screens` and non-freeform options in their current sections.

- [ ] **Step 4: Make profile activation atomic**

```cpp
enum class TopologySelectionResult { Known, Unknown, Unavailable };
TopologySelectionResult Config::selectTopology(const DisplayTopology& topology);
```

Build links into a candidate map, validate all referenced screens and geometry, then swap it into the runtime link map. Unknown or empty topologies swap in an empty link map. Never keep the previous profile’s links while the candidate is built or if the key differs.

- [ ] **Step 5: Run tests and commit**

```sh
cmake --build build-arm64 --target unittests -j$(nproc)
./build-arm64/bin/unittests \
  --gtest_filter='TopologyProfileConfigTests.*:ConfigTests.*'
git add src/lib/barrier/Config.* \
  src/test/unittests/barrier/TopologyProfileConfigTests.cpp \
  src/test/unittests/CMakeLists.txt
git commit -m "feat: persist exact topology profiles #13"
```

## Task 4: Debounced server topology state machine

**Files:**
- Create: `src/lib/server/DisplayTopologyStateMachine.h`
- Create: `src/lib/server/DisplayTopologyStateMachine.cpp`
- Create: `src/test/unittests/server/DisplayTopologyStateMachineTests.cpp`
- Modify: `src/test/unittests/CMakeLists.txt`

- [ ] **Step 1: Define events, outputs, and failing transition tests**

```cpp
enum class DisplayTopologyState {
    Reconfiguring,
    StableKnown,
    StableUnknown,
    NoDisplayGrace,
    Unavailable
};

struct DisplayTopologyDecision {
    DisplayTopologyState state;
    bool switchingEnabled;
    bool disconnectClients;
    bool displayReady;
    qint64 nextDeadlineMs;
};

class DisplayTopologyStateMachine {
public:
    DisplayTopologyDecision observe(DisplayTopology topology,
                                    bool profileKnown,
                                    qint64 monotonicMs);
    DisplayTopologyDecision onDeadline(qint64 monotonicMs);
};
```

Use a project-native integral timestamp rather than Qt in `src/lib`; the pseudocode names intent only.

Tests must prove:

- non-empty changes remain `Reconfiguring` until 2,000 ms quiet;
- another change resets the quiet deadline;
- known settles to `StableKnown`, switching enabled, ready true;
- unknown settles to `StableUnknown`, sessions retained, switching disabled, ready true;
- zero display immediately disables switching and starts 10,000 ms grace;
- display return during grace cancels disconnect and starts a new quiet period;
- grace expiry enters `Unavailable`, ready false, disconnect exactly once;
- repeated deadlines and repeated identical observations are idempotent.

- [ ] **Step 2: Run RED, implement, run GREEN**

```sh
cmake --build build-arm64 --target unittests -j$(nproc)
./build-arm64/bin/unittests \
  --gtest_filter='DisplayTopologyStateMachineTests.*'
```

- [ ] **Step 3: Commit**

```sh
git add src/lib/server/DisplayTopologyStateMachine.* \
  src/test/unittests/server/DisplayTopologyStateMachineTests.cpp \
  src/test/unittests/CMakeLists.txt
git commit -m "feat: add topology readiness state machine #13"
```

## Task 5: Apply topology decisions inside the server core

**Files:**
- Modify: `src/lib/server/Server.h`
- Modify: `src/lib/server/Server.cpp`
- Modify: `src/lib/barrier/ServerApp.cpp`
- Create: `src/test/unittests/server/ServerTests.cpp`

- [ ] **Step 1: Run references for `Server::setConfig`, `Server::handleShapeChanged`, switching entry points, and connected-client teardown**

Identify every place that assumes the current `Config` always has links. Use one `switchingEnabled()` guard at the deepest shared transition seam rather than sprinkling unknown-topology checks through directional branches.

- [ ] **Step 2: Add server tests with fake time and fake primary topology**

Cover observable behavior:

- known topology loads profile and allows edge transition;
- unknown topology keeps clients connected but edge transitions and return-to-previous-screen are disabled;
- switching to a second known topology uses only its links;
- zero display disables switching immediately;
- clients are disconnected only after grace expiry;
- topology return before expiry keeps sessions;
- config reload reevaluates the current topology and can turn unknown into known without reconnecting.

- [ ] **Step 3: Integrate one topology timer**

On primary `shapeChanged`:

1. read the complete topology from `PrimaryClient`;
2. query whether the loaded config has the exact profile;
3. feed the snapshot and monotonic time into the state machine;
4. cancel/re-arm one event-queue timer for its next deadline;
5. apply the decision.

Ignore remote-client shape events for profile selection; they still update cursor/display metadata as before.

- [ ] **Step 4: Apply decisions in the required order**

For every non-known state, clear active links before logging status or handling pointer movement. For `StableKnown`, select and validate the exact profile before enabling switching. For `Unavailable`, move control back to the primary if possible, then close connected client sessions through the existing orderly disconnect path.

During `Reconfiguring` and `NoDisplayGrace`, retain sessions but disable switching. This prevents the pointer from escaping through stale edges while macOS sends an intermediate geometry sequence.

- [ ] **Step 5: Reevaluate after configuration reload**

After `ServerApp::reloadConfig` calls `Server::setConfig`, `Server` immediately applies the current stable topology against the new profile collection. Saving an unknown topology can therefore produce `StableKnown` without restarting the server or clients.

- [ ] **Step 6: Emit one machine-parseable status line on state/key changes**

Use a stable prefix and tab-separated fields:

```text
BARRIER_TOPOLOGY	state=StableUnknown	key=<64 lowercase hex>	displays=<encoded normalized records>
```

Requirements:

- fields are ASCII and length-bounded;
- display records contain stable ID, x, y, w, h, rotation, primary;
- names are excluded;
- unavailable uses `key=-` and an empty display field;
- identical state/key/topology does not log repeatedly.

- [ ] **Step 7: Run tests and commit**

```sh
cmake --build build-arm64 --target unittests barriers -j$(nproc)
./build-arm64/bin/unittests \
  --gtest_filter='DisplayTopologyStateMachineTests.*:ServerTests.*Topology*'
git add src/lib/server/Server.* src/lib/barrier/ServerApp.cpp \
  src/test/unittests/server/ServerTests.cpp
git commit -m "feat: select topology profiles in server core #13"
```

## Task 6: Scope the freeform editor to the current topology

**Files:**
- Create: `src/gui/src/TopologyProfileStore.h`
- Create: `src/gui/src/TopologyProfileStore.cpp`
- Create: `src/gui/test/TopologyProfileStoreTests.cpp`
- Modify: `src/gui/src/ServerConfig.h`
- Modify: `src/gui/src/ServerConfig.cpp`
- Modify: `src/gui/src/FreeformLayoutSettings.h`
- Modify: `src/gui/src/FreeformLayoutSettings.cpp`
- Modify: `src/gui/src/FreeformServerConfigWidget.h`
- Modify: `src/gui/src/FreeformServerConfigWidget.cpp`
- Modify: `src/gui/src/ServerConfigDialog.h`
- Modify: `src/gui/src/ServerConfigDialog.cpp`
- Modify: `src/gui/CMakeLists.txt`

- [ ] **Step 1: Add failing QSettings profile-store tests**

Use the existing temporary-QSettings convention from `ServerConfigPersistenceTests.cpp`. Cover:

- two exact topology profiles persist independently;
- selecting A → B → A returns A’s geometry;
- saving current unknown topology adds only its key;
- cancellation leaves all profiles unchanged;
- malformed profile data is ignored with an explicit error result;
- legacy flat positions can seed an editor model but are not marked saved.

- [ ] **Step 2: Implement an atomic profile store**

`TopologyProfileStore` owns the QSettings serialization boundary. It reads/writes version, exact topology descriptor, screen positions, and display rectangles. Save through a temporary settings group, sync and validate status, then replace the live group. Do not partially update an existing profile.

- [ ] **Step 3: Replace flat layout access with current-profile access**

`ServerConfig` keeps all profiles and the currently observed topology key. `FreeformLayoutSettings` loads the selected profile if present; otherwise it copies legacy geometry into a transient editor seed. `FreeformServerConfigWidget` receives the current server display rectangles from the topology status, not from stale client metadata.

- [ ] **Step 4: Make unknown topology explicit in the dialog**

When the current key is unknown:

- show `This display arrangement is not configured`;
- render its exact server monitors plus existing client geometries;
- keep Save enabled;
- label Save as creating a profile for the displayed arrangement.

When known, edits update only that key. Do not offer a generic fallback profile or wildcard topology.

- [ ] **Step 5: Serialize every profile into the Barrier config file**

Update `ServerConfig::save()` / stream generation so one save writes all stored profiles using Task 3’s grammar. Preserve unrelated screen options and hotkeys.

- [ ] **Step 6: Run tests and commit**

```sh
cmake --build build-arm64 --target guiunittests barrier -j$(nproc)
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='TopologyProfileStoreTests.*:ServerConfigPersistenceTests.*'
git add src/gui/src/TopologyProfileStore.* src/gui/src/ServerConfig.* \
  src/gui/src/FreeformLayoutSettings.* \
  src/gui/src/FreeformServerConfigWidget.* \
  src/gui/src/ServerConfigDialog.* \
  src/gui/test/TopologyProfileStoreTests.cpp src/gui/CMakeLists.txt
git commit -m "feat: edit freeform layouts by topology #13"
```

## Task 7: Parse status, notify, and publish display readiness

**Files:**
- Create: `src/gui/src/TopologyStatusParser.h`
- Create: `src/gui/src/TopologyStatusParser.cpp`
- Create: `src/gui/test/TopologyStatusParserTests.cpp`
- Modify: `src/gui/src/MainWindow.h`
- Modify: `src/gui/src/MainWindow.cpp`
- Modify: `src/gui/src/ZeroconfRegister.h`
- Modify: `src/gui/src/ZeroconfRegister.cpp`
- Modify: `src/gui/src/ZeroconfService.h`
- Modify: `src/gui/src/ZeroconfService.cpp`
- Modify: `src/gui/CMakeLists.txt`

- [ ] **Step 1: Add strict parser tests**

Tests accept each valid state and reject:

- missing/duplicate fields;
- invalid state names;
- non-hex or wrong-length keys;
- duplicate display IDs;
- oversized lines;
- malformed integers or geometry;
- a key that does not match the decoded topology.

The parser returns an error without changing the last valid GUI state.

- [ ] **Step 2: Parse topology status before generic log rendering**

In `MainWindow::updateFromLogLine`, parse the stable prefix, update current topology/profile state, and then render the normal log line. Do not infer readiness from localized human log text.

- [ ] **Step 3: Add one-shot unknown notifications and persistent menu status**

On the first `StableUnknown` event for a key in the process session, call `QSystemTrayIcon::showMessage` and expose `Configure this display arrangement` in the tray/menu. Do not repeat for the same key until Barrier restarts. `StableKnown`, `Reconfiguring`, `NoDisplayGrace`, and `Unavailable` each have distinct status text.

- [ ] **Step 4: Publish `display-ready` through Bonjour**

After issue #12’s TXT support is present, register/update the server record with:

- `display-ready=1` for `StableKnown` and `StableUnknown`;
- `display-ready=0` for `Reconfiguring`, `NoDisplayGrace`, and `Unavailable`;
- the server’s persistent `proximity-id` when proximity advertising is enabled.

Update the TXT record in place with `DNSServiceUpdateRecord`; if no registration exists, save the latest values for the next registration. Do not tear down and recreate the service for every display callback.

- [ ] **Step 5: Add UI parser tests and build**

```sh
cmake --build build-arm64 --target guiunittests barrier -j$(nproc)
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='TopologyStatusParserTests.*:ZeroconfRecordTests.*'
```

- [ ] **Step 6: Commit**

```sh
git add src/gui/src/TopologyStatusParser.* \
  src/gui/test/TopologyStatusParserTests.cpp src/gui/src/MainWindow.* \
  src/gui/src/ZeroconfRegister.* src/gui/src/ZeroconfService.* \
  src/gui/CMakeLists.txt
git commit -m "feat: surface topology readiness in GUI #13"
```

## Task 8: Reload saved profiles without restarting sessions

**Files:**
- Modify: `src/gui/src/MainWindow.h`
- Modify: `src/gui/src/MainWindow.cpp`
- Modify: `src/gui/src/Ipc.h`
- Modify: `src/gui/src/Ipc.cpp`
- Modify: `src/gui/src/IpcClient.h`
- Modify: `src/gui/src/IpcClient.cpp`
- Modify: `src/lib/ipc/IpcMessage.h`
- Modify: `src/lib/ipc/IpcMessage.cpp`
- Modify: `src/lib/ipc/IpcClientProxy.h`
- Modify: `src/lib/ipc/IpcClientProxy.cpp`
- Modify: `src/lib/barrier/ServerApp.cpp`
- Modify: relevant IPC unit tests under `src/test/unittests/ipc/`

- [ ] **Step 1: Follow references through both duplicate IPC protocol definitions**

The GUI and `src/lib/ipc` currently duplicate message constants. Add the reload message to both sides in one task and test its wire framing. Do not reuse `kIpcCommand` with an empty or magic shell command.

- [ ] **Step 2: Add an explicit reload message**

Define a fixed four-byte `IRLD` message with no payload. `IpcClient::sendReload()` writes it. The daemon proxy accepts it only from an authenticated local GUI connection and raises `forServerApp().reloadConfig()` for the owned server process; it must not execute a command string.

- [ ] **Step 3: Keep the desktop path simple**

After a successful profile save:

- desktop mode sends SIGHUP to the `barriers` process already owned by `MainWindow`;
- service mode sends `IRLD`;
- failure to deliver reload leaves the saved profile intact and shows an actionable error;
- no path restarts the child as a substitute for reload.

- [ ] **Step 4: Verify live activation**

Add integration coverage or a focused harness proving:

1. server reports `StableUnknown`;
2. GUI saves that exact key;
3. reload event reaches `ServerApp::reloadConfig`;
4. the same connected server reports `StableKnown`;
5. client sessions remain open throughout.

- [ ] **Step 5: Run tests and commit**

```sh
cmake --build build-arm64 --target unittests guiunittests barrier -j$(nproc)
./build-arm64/bin/unittests --gtest_filter='Ipc*Reload*:*ServerApp*Reload*'
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='*Reload*:*Topology*'
git add src/gui/src/MainWindow.* src/gui/src/Ipc* src/lib/ipc/Ipc* \
  src/lib/barrier/ServerApp.cpp src/test/unittests/ipc
git commit -m "feat: reload topology profiles without restart #13"
```

## Task 9: News fragment and end-to-end macOS verification

**Files:**
- Create: `doc/newsfragments/13.feature`

- [ ] **Step 1: Add the news fragment**

```text
Added macOS display-topology profiles that automatically select the exact saved freeform layout and pause switching for unknown or unavailable arrangements.
```

- [ ] **Step 2: Build the full arm64 product**

```sh
cmake -S . -B build-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=macosx \
  -DBARRIER_BUILD_TESTS=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-arm64 -j$(nproc)
```

- [ ] **Step 3: Run the complete changed-contract suites**

```sh
./build-arm64/bin/unittests \
  --gtest_filter='Sha256Tests.*:DisplayTopologyTests.*:DisplayTopologyStateMachineTests.*:TopologyProfileConfigTests.*:ServerTests.*Topology*:*Ipc*Reload*'
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='Topology*:*ServerConfigPersistenceTests.*:*Zeroconf*'
```

- [ ] **Step 4: Exercise all four laptop states at the actual Barrier surface**

With at least one client connected, sequentially verify:

1. lid open, internal only;
2. lid open, internal plus external;
3. lid closed, external only;
4. lid closed, no external display.

For each non-empty state:

- capture the reported profile key and normalized topology;
- verify a known key applies its exact freeform links;
- verify an unknown key keeps the client session but prevents every edge transition and shows one notification;
- save the unknown layout and verify it applies live without reconnecting;
- switch back to a previous arrangement and verify automatic reselection.

For zero display:

- confirm switching disables immediately;
- restore a display within ten seconds and confirm the session survives;
- repeat and leave it absent past ten seconds; confirm the client disconnects and Bonjour advertises `display-ready=0`.

- [ ] **Step 5: Stress transient and sequential changes**

Plug/unplug two external monitors sequentially and change primary display. Confirm:

- state remains `Reconfiguring` until two seconds after the last callback;
- no previous profile is active during the sequence;
- the final exact topology selects once;
- identical callbacks do not duplicate notifications or status churn.

- [ ] **Step 6: Verify computed UI geometry**

On the current-topology editor and tray menu, inspect actual widget geometry/accessibility:

- no clipped topology/status text;
- buttons at least 32 px high;
- server monitor rectangles do not overlap unless the CoreGraphics topology itself overlaps;
- shared column edges align within 4 px;
- keyboard focus reaches Configure, Save, Cancel, and profile controls.

This proves measurable correctness only; human review remains the aesthetic check.

- [ ] **Step 7: Run hygiene checks**

```sh
git diff --check origin/main...HEAD
```

Confirm tracked files contain no machine-specific monitor UUIDs, hostnames, private paths, or captured logs.

- [ ] **Step 8: Commit the fragment and any test-only corrections**

```sh
git add doc/newsfragments/13.feature
git commit -m "docs: note dynamic display profiles #13"
```
