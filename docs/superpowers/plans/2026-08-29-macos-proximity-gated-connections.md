# macOS Proximity-Gated Connections Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make an opted-in macOS client run only while its explicitly paired server is within calibrated Bluetooth desk range, present through matching Bonjour metadata, and display-ready.

**Status:** Implemented and verified for issue #12. The checkboxes below retain
the original execution sequence rather than acting as a live task tracker.

**Architecture:** Keep BLE, DNS-SD, UI, and process callbacks behind a deterministic `ProximityConnectionPolicy`. CoreBluetooth and Bonjour adapters emit typed observations; only `MainWindow` applies the policy’s desired child-process state. Launch proximity clients with `--no-restart`, preserve existing TLS trust, and fail closed unless the user activates a session-only BLE override.

**Tech Stack:** C++14, Objective-C++, Qt 5, CoreBluetooth, Bonjour DNS-SD, CMake, GoogleTest.

**Dependency:** Implement issue #13’s `display-ready` publication before the final #12 integration task. The pure filter, policy, persistence, BLE, and Bonjour identity work can land independently.

---

## Files

### Create

- `src/gui/src/ProximitySignalFilter.h`
- `src/gui/src/ProximitySignalFilter.cpp`
- `src/gui/src/ProximityConnectionPolicy.h`
- `src/gui/src/ProximityConnectionPolicy.cpp`
- `src/gui/src/ProximityConfig.h`
- `src/gui/src/ProximityConfig.cpp`
- `src/gui/src/MacProximityController.h`
- `src/gui/src/MacProximityController.mm`
- `src/gui/src/ProximitySettingsDialog.h`
- `src/gui/src/ProximitySettingsDialog.cpp`
- `src/gui/src/ProximitySettingsDialogBase.ui`
- `src/gui/test/ProximitySignalFilterTests.cpp`
- `src/gui/test/ProximityConnectionPolicyTests.cpp`
- `src/gui/test/ProximityConfigTests.cpp`
- `src/gui/test/ZeroconfRecordTests.cpp`

### Modify

- `src/gui/src/ZeroconfRecord.h`
- `src/gui/src/ZeroconfRegister.h`
- `src/gui/src/ZeroconfRegister.cpp`
- `src/gui/src/ZeroconfBrowser.h`
- `src/gui/src/ZeroconfBrowser.cpp`
- `src/gui/src/ZeroconfService.h`
- `src/gui/src/ZeroconfService.cpp`
- `src/gui/src/MainWindow.h`
- `src/gui/src/MainWindow.cpp`
- `src/gui/CMakeLists.txt`
- `src/gui/res/mac/Info.plist`
- `dist/macos/bundle/Barrier.app/Contents/Info.plist.in`
- `doc/newsfragments/12.feature`

## Task 1: Deterministic RSSI calibration and hysteresis

**Files:**
- Create: `src/gui/src/ProximitySignalFilter.h`
- Create: `src/gui/src/ProximitySignalFilter.cpp`
- Create: `src/gui/test/ProximitySignalFilterTests.cpp`
- Modify: `src/gui/CMakeLists.txt`

- [ ] **Step 1: Add failing calibration tests**

```cpp
TEST(ProximitySignalFilterTests, calibrationUsesMedianAndRequiredSampleCount)
{
    barrier::ProximityCalibration calibration;
    EXPECT_FALSE(barrier::calibrateProximityRssi(
        std::vector<int>(19, -50), calibration));

    std::vector<int> samples(20, -50);
    samples.push_back(127); // CoreBluetooth invalid sentinel
    ASSERT_TRUE(barrier::calibrateProximityRssi(samples, calibration));
    EXPECT_EQ(calibration.enterDbm, -56);
    EXPECT_EQ(calibration.exitDbm, -62);
}

TEST(ProximitySignalFilterTests, departureRequiresContinuousTenSecondAbsence)
{
    barrier::ProximitySignalFilter filter({-56, -62});
    filter.addSample(-50, 0);
    filter.addSample(-50, 500);
    filter.addSample(-50, 1000);
    EXPECT_TRUE(filter.isNear(1000));
    EXPECT_TRUE(filter.isNear(10999));
    EXPECT_FALSE(filter.isNear(11000));
}
```

- [ ] **Step 2: Register the new test and production sources**

Add the two production files to `GUI_COMMON_SOURCE_FILES` / `GUI_COMMON_HEADER_FILES` and the test to `GUI_TEST_SOURCE_FILES` so `guiunittests` exercises the same implementation linked into the app.

- [ ] **Step 3: Run the focused test and confirm RED**

```sh
cmake --build build-arm64 --target guiunittests -j$(nproc)
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='ProximitySignalFilterTests.*'
```

Expected: compilation fails because `ProximitySignalFilter` and `calibrateProximityRssi` do not exist.

- [ ] **Step 4: Implement the value types and filter**

```cpp
namespace barrier {

struct ProximityCalibration {
    int enterDbm;
    int exitDbm;
};

bool calibrateProximityRssi(const std::vector<int>& samples,
                            ProximityCalibration& calibration);

class ProximitySignalFilter {
public:
    explicit ProximitySignalFilter(const ProximityCalibration& calibration);
    void reset();
    void addSample(int rssiDbm, qint64 monotonicMs);
    bool isNear(qint64 monotonicMs) const;

private:
    ProximityCalibration m_calibration;
    double m_filteredDbm;
    bool m_hasFilteredSample;
    int m_entrySamples;
    qint64 m_lastExitQualifyingSampleMs;
    bool m_near;
};

}
```

Implementation rules:

- discard RSSI `127`;
- require at least twenty valid calibration samples;
- sort a copy and use the median;
- set entry to median minus 6 dB and exit to median minus 12 dB;
- use EWMA `filtered = 0.25 * sample + 0.75 * previous`;
- require three consecutive filtered samples at or above entry;
- refresh presence only at or above exit;
- report far after 10,000 ms without a qualifying sample;
- consume an injected monotonic timestamp; never read wall-clock time internally.

- [ ] **Step 5: Run GREEN and commit**

```sh
cmake --build build-arm64 --target guiunittests -j$(nproc)
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='ProximitySignalFilterTests.*'
git add src/gui/src/ProximitySignalFilter.* \
  src/gui/test/ProximitySignalFilterTests.cpp src/gui/CMakeLists.txt
git commit -m "feat: add calibrated proximity signal filter #12"
```

Expected: focused tests pass.

## Task 2: Central connection policy

**Files:**
- Create: `src/gui/src/ProximityConnectionPolicy.h`
- Create: `src/gui/src/ProximityConnectionPolicy.cpp`
- Create: `src/gui/test/ProximityConnectionPolicyTests.cpp`
- Modify: `src/gui/CMakeLists.txt`

- [ ] **Step 1: Add the policy contract and failing truth-table tests**

```cpp
enum class ProximityPolicyState {
    Disabled,
    WaitingForPermission,
    SensorUnavailable,
    WaitingForPeer,
    WaitingForNetwork,
    Starting,
    Connected,
    DepartureGrace,
    ManualOverride
};

struct ProximityInputs {
    bool enabled;
    bool userWantsBarrier;
    bool bluetoothAuthorized;
    bool bluetoothAvailable;
    bool pairedPeerNear;
    bool pairedBonjourPresent;
    bool serverDisplayReady;
    bool manualOverride;
    bool childRunning;
};

struct ProximityDecision {
    ProximityPolicyState state;
    bool shouldRunChild;
};
```

```cpp
TEST(ProximityConnectionPolicyTests, requiresAllAutomaticGates)
{
    ProximityConnectionPolicy policy;
    ProximityInputs inputs{true, true, true, true, true, true, true, false, false};
    EXPECT_TRUE(policy.evaluate(inputs).shouldRunChild);

    inputs.pairedPeerNear = false;
    EXPECT_FALSE(policy.evaluate(inputs).shouldRunChild);
    inputs.pairedPeerNear = true;
    inputs.pairedBonjourPresent = false;
    EXPECT_FALSE(policy.evaluate(inputs).shouldRunChild);
    inputs.pairedBonjourPresent = true;
    inputs.serverDisplayReady = false;
    EXPECT_FALSE(policy.evaluate(inputs).shouldRunChild);
}

TEST(ProximityConnectionPolicyTests, overrideBypassesOnlyBluetooth)
{
    ProximityConnectionPolicy policy;
    ProximityInputs inputs{true, true, false, false, false, true, true, true, false};
    EXPECT_TRUE(policy.evaluate(inputs).shouldRunChild);
    inputs.pairedBonjourPresent = false;
    EXPECT_FALSE(policy.evaluate(inputs).shouldRunChild);
}
```

- [ ] **Step 2: Run RED**

```sh
cmake --build build-arm64 --target guiunittests -j$(nproc)
```

Expected: compilation fails on the missing policy types.

- [ ] **Step 3: Implement a pure evaluator**

`evaluate()` must have no timers, process calls, dialogs, DNS operations, or Bluetooth calls. It converts current inputs into one state and one desired process bit. `childRunning` distinguishes `Starting` from `Connected`; grace is represented by the filtered `pairedPeerNear` input from Task 1 rather than another policy timer.

- [ ] **Step 4: Add tests for disabled mode, denied permission, Bluetooth off, waiting for Bonjour, waiting for display readiness, and repeated identical inputs**

Repeated evaluation with identical inputs must produce an identical decision and no side effects.

- [ ] **Step 5: Run GREEN and commit**

```sh
cmake --build build-arm64 --target guiunittests -j$(nproc)
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='ProximityConnectionPolicyTests.*'
git add src/gui/src/ProximityConnectionPolicy.* \
  src/gui/test/ProximityConnectionPolicyTests.cpp src/gui/CMakeLists.txt
git commit -m "feat: add proximity connection policy #12"
```

## Task 3: Pairing persistence with atomic replacement

**Files:**
- Create: `src/gui/src/ProximityConfig.h`
- Create: `src/gui/src/ProximityConfig.cpp`
- Create: `src/gui/test/ProximityConfigTests.cpp`
- Modify: `src/gui/CMakeLists.txt`

- [ ] **Step 1: Add a failing QSettings round-trip test**

```cpp
struct ProximityPairing {
    QString proximityId;
    QUuid peripheralId;
    QString displayName;
    barrier::ProximityCalibration calibration;
};

TEST(ProximityConfigTests, pairingSurvivesRelaunch)
{
    QTemporaryDir directory;
    QSettings settings(directory.filePath("barrier.ini"), QSettings::IniFormat);
    ProximityConfig config(settings);
    ProximityPairing pairing{"00112233445566778899aabbccddeeff",
                             QUuid::createUuid(), "Desk server", {-56, -62}};
    ASSERT_TRUE(config.replacePairing(pairing));

    ProximityConfig reloaded(settings);
    ASSERT_TRUE(reloaded.pairing().has_value());
    EXPECT_EQ(reloaded.pairing()->proximityId, pairing.proximityId);
    EXPECT_EQ(reloaded.pairing()->peripheralId, pairing.peripheralId);
}
```

Use the project’s C++14-compatible optional convention; if `std::optional` is unavailable, expose `bool pairing(ProximityPairing&) const` instead of adding a dependency.

- [ ] **Step 2: Implement a dedicated `proximity` QSettings group**

Persist:

- server advertiser enabled;
- server-generated 128-bit proximity ID;
- client gating enabled;
- paired proximity ID;
- CoreBluetooth peripheral UUID;
- display label;
- entry and exit thresholds.

Validate lowercase 32-character hexadecimal IDs, non-null peripheral UUIDs, and `exitDbm < enterDbm < 0`. Write all pairing fields, call `sync()`, check `QSettings::status()`, then replace in-memory state. A failed write leaves the previous pairing intact.

- [ ] **Step 3: Add tests for malformed IDs, invalid thresholds, cancelled replacement, and server-ID generation-once behavior**

- [ ] **Step 4: Run and commit**

```sh
cmake --build build-arm64 --target guiunittests -j$(nproc)
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='ProximityConfigTests.*'
git add src/gui/src/ProximityConfig.* src/gui/test/ProximityConfigTests.cpp \
  src/gui/CMakeLists.txt
git commit -m "feat: persist macOS proximity pairing #12"
```

## Task 4: Resolved Bonjour identity and removal semantics

**Files:**
- Modify: `src/gui/src/ZeroconfRecord.h`
- Modify: `src/gui/src/ZeroconfRegister.h`
- Modify: `src/gui/src/ZeroconfRegister.cpp`
- Modify: `src/gui/src/ZeroconfBrowser.h`
- Modify: `src/gui/src/ZeroconfBrowser.cpp`
- Modify: `src/gui/src/ZeroconfService.h`
- Modify: `src/gui/src/ZeroconfService.cpp`
- Create: `src/gui/test/ZeroconfRecordTests.cpp`
- Modify: `src/gui/CMakeLists.txt`

- [ ] **Step 1: Before changing exported Zeroconf methods, run symbol references**

Use LSP/Serena references for `ZeroconfRegister::registerService`, `ZeroconfBrowser::browseForType`, `ZeroconfService::serverDetected`, and `MainWindow::serverDetected`. Record every caller in the implementation notes and update them in the same task.

- [ ] **Step 2: Extend the record model and add failing TXT tests**

```cpp
struct ZeroconfRecord {
    QString serviceName;
    QString registeredType;
    QString replyDomain;
    QString hostName;
    quint16 port = 0;
    QMap<QString, QByteArray> txt;
};

TEST(ZeroconfRecordTests, pairedServerRequiresMatchingIdentityAndReadiness)
{
    ZeroconfRecord record;
    record.txt["proximity-id"] = "00112233445566778899aabbccddeeff";
    record.txt["display-ready"] = "1";
    EXPECT_TRUE(record.matchesProximityServer(
        "00112233445566778899aabbccddeeff"));
    record.txt["display-ready"] = "0";
    EXPECT_FALSE(record.isDisplayReady());
}
```

- [ ] **Step 3: Add DNS-SD TXT registration**

Change the non-callback overload to:

```cpp
void registerService(const ZeroconfRecord& record,
                     quint16 servicePort,
                     const QMap<QString, QByteArray>& txt);
```

Build the payload with `TXTRecordCreate`, `TXTRecordSetValue`, and `TXTRecordGetBytesPtr`; pass its length and bytes to `DNSServiceRegister`; always call `TXTRecordDeallocate` after registration returns.

Reject keys or values that DNS-SD cannot encode. Do not truncate identity or readiness metadata.

- [ ] **Step 4: Resolve added browse records before publishing them**

Use `DNSServiceResolve` for each add event to populate host, port, and TXT. Keep one resolver ref and `QSocketNotifier` per unresolved service key `(name, type, domain)`. Remove and deallocate it on browse removal or resolution failure.

Emit one synchronized `currentRecordsChanged` list after:

- a record resolves successfully;
- a record is removed;
- a resolved record’s TXT changes.

Do not keep removed records in `MainWindow`’s server list.

- [ ] **Step 5: Change `ZeroconfService` to publish complete state**

Replace add-only callbacks with a signal carrying the complete resolved server list. The client selects the record whose `proximity-id` equals the persisted pairing. Its resolved `hostName` and Barrier's configured data port become the network endpoint; the DNS-SD SRV port belongs to the auxiliary discovery service, and the service display name is presentation only.

- [ ] **Step 6: Run tests and commit**

```sh
cmake --build build-arm64 --target guiunittests barrier -j$(nproc)
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='ZeroconfRecordTests.*'
git add src/gui/src/Zeroconf* src/gui/test/ZeroconfRecordTests.cpp \
  src/gui/CMakeLists.txt
git commit -m "feat: resolve paired Bonjour server metadata #12"
```

## Task 5: CoreBluetooth adapter and macOS permissions

**Files:**
- Create: `src/gui/src/MacProximityController.h`
- Create: `src/gui/src/MacProximityController.mm`
- Modify: `src/gui/CMakeLists.txt`
- Modify: `src/gui/res/mac/Info.plist`
- Modify: `dist/macos/bundle/Barrier.app/Contents/Info.plist.in`

- [ ] **Step 1: Define the Qt-facing adapter contract**

```cpp
class MacProximityController : public QObject {
    Q_OBJECT
public:
    enum class BluetoothState { Unknown, Unauthorized, PoweredOff, PoweredOn, Failed };

    void startAdvertising(const QString& proximityId);
    void stopAdvertising();
    void startScanning();
    void stopScanning();
    void readPairingIdentity(const QUuid& peripheralId);

signals:
    void bluetoothStateChanged(BluetoothState state);
    void peripheralObserved(QUuid peripheralId, QString name, int rssiDbm);
    void pairingIdentityRead(QUuid peripheralId, QString proximityId);
    void operationFailed(QString operation, QString userMessage);
};
```

Keep CoreBluetooth objects inside an Objective-C++ private implementation. Public headers must not expose Objective-C types.

- [ ] **Step 2: Implement server peripheral behavior**

- create one fixed 128-bit Barrier service UUID and one read-only identity characteristic UUID;
- advertise only the service UUID;
- return the persisted 16-byte identity for read requests;
- reject writes;
- stop advertising when server mode is disabled or the app quits.

- [ ] **Step 3: Implement client central behavior**

- scan only for the fixed service UUID;
- set `CBCentralManagerScanOptionAllowDuplicatesKey` to true;
- emit every valid RSSI sample with the CoreBluetooth peripheral UUID;
- retain discovered peripherals while pairing;
- connect only for an explicit pairing identity read;
- disconnect the GATT session after the identity is read;
- map authorization, powered-off, unsupported, and reset states into `BluetoothState`.

- [ ] **Step 4: Link and declare permissions**

In `src/gui/CMakeLists.txt`, add `CoreBluetooth.framework` only under `APPLE` and compile the `.mm` source only there. Add `NSBluetoothAlwaysUsageDescription` to both app-bundle plist sources with user-facing text explaining that Barrier uses Bluetooth to connect only when the paired server is nearby.

- [ ] **Step 5: Build the packaged target and inspect linkage**

```sh
cmake --build build-arm64 --target barrier -j$(nproc)
otool -L build-arm64/bin/barrier
```

Expected: the app binary links CoreBluetooth and non-Apple configurations never reference the adapter source.

- [ ] **Step 6: Commit**

```sh
git add src/gui/src/MacProximityController.* src/gui/CMakeLists.txt \
  src/gui/res/mac/Info.plist \
  dist/macos/bundle/Barrier.app/Contents/Info.plist.in
git commit -m "feat: add macOS BLE proximity adapter #12"
```

## Task 6: Pairing, calibration, and override UI

**Files:**
- Create: `src/gui/src/ProximitySettingsDialog.h`
- Create: `src/gui/src/ProximitySettingsDialog.cpp`
- Create: `src/gui/src/ProximitySettingsDialogBase.ui`
- Modify: `src/gui/src/MainWindow.h`
- Modify: `src/gui/src/MainWindow.cpp`
- Modify: `src/gui/CMakeLists.txt`

- [ ] **Step 1: Build one role-aware dialog**

Server mode shows:

- `Enable proximity advertising`;
- current opaque pairing availability, without exposing the raw ID;
- `Reset pairing identity`, guarded by confirmation because existing client pairings will stop matching.

Client mode shows:

- `Enable proximity-gated connection`;
- discovered Barrier servers correlated through matching BLE and Bonjour IDs;
- `Pair and calibrate`;
- paired server label and current filtered RSSI;
- `Recalibrate` and `Forget pairing`.

- [ ] **Step 2: Implement transactional pairing**

Collect candidate identity, matching Bonjour record, and ten seconds of valid RSSI samples in temporary dialog state. Call `ProximityConfig::replacePairing()` only after calibration succeeds. Closing or cancelling the dialog leaves the previous pairing and process policy unchanged.

- [ ] **Step 3: Add one tray/menu action for proximity settings and one conditional override action**

`Connect Anyway` appears only in `SensorUnavailable`. It sets an in-memory flag on `MainWindow`; it does not write QSettings. `Resume Proximity` clears it.

- [ ] **Step 4: Verify accessibility and geometry**

Using Qt accessibility inspection or the live macOS surface, confirm:

- every field has a label;
- keyboard focus order reaches Pair, Recalibrate, Forget, Override, and Cancel;
- buttons are at least 32 px high on desktop;
- status text is not clipped at the smallest supported dialog size.

- [ ] **Step 5: Build and commit**

```sh
cmake --build build-arm64 --target barrier guiunittests -j$(nproc)
git add src/gui/src/ProximitySettingsDialog* src/gui/src/MainWindow.* \
  src/gui/CMakeLists.txt
git commit -m "feat: add proximity pairing controls #12"
```

## Task 7: Gate the real client lifecycle

**Files:**
- Modify: `src/gui/src/MainWindow.h`
- Modify: `src/gui/src/MainWindow.cpp`
- Modify: `src/gui/src/ZeroconfService.h`
- Modify: `src/gui/src/ZeroconfService.cpp`
- Create: `doc/newsfragments/12.feature`

- [ ] **Step 1: Add one reconciliation method**

```cpp
void MainWindow::reconcileProximityClient()
{
    const ProximityDecision decision = m_proximityPolicy.evaluate(currentProximityInputs());
    updateProximityStatus(decision.state);

    if (decision.shouldRunChild && !isBarrierProcessRunning()) {
        startBarrier();
    }
    else if (!decision.shouldRunChild && isBarrierProcessRunning()) {
        stopBarrier();
    }
}
```

Every BLE, Bonjour, display-readiness, user-intent, child-started, and child-finished callback updates stored observations and calls this method. No callback directly starts a second child.

- [ ] **Step 2: Make proximity client arguments non-restartable**

In `MainWindow::clientArgs`, append `--no-restart` only when proximity gating or its session override is active. Use the resolved host from the matching Bonjour record together with Barrier's configured data port; never use the auxiliary DNS-SD SRV port or fall back to the manual hostname while proximity mode is enabled.

- [ ] **Step 3: Remove the GUI restart loop from the gated path**

Change `MainWindow::barrierFinished` so the proximity path updates `childRunning=false` and reconciles policy. Keep the existing one-second restart behavior unchanged for non-proximity users.

- [ ] **Step 4: Wire display readiness from issue #13**

Only a matching `proximity-id` record with `display-ready=1` satisfies the gate. `display-ready=0`, missing TXT, record removal, or identity mismatch enters the waiting/departure path without opening TCP.

- [ ] **Step 5: Add user-visible status mapping**

Map all `ProximityPolicyState` values to menu-bar/main-window text. Sensor failures are paused states, not connection-error messages. Do not display raw CoreBluetooth error objects.

- [ ] **Step 6: Add the news fragment**

`doc/newsfragments/12.feature`:

```text
Added optional macOS proximity-gated connections that keep clients idle until their explicitly paired server is nearby and discoverable.
```

- [ ] **Step 7: Run focused and full changed-contract tests**

```sh
cmake --build build-arm64 --target guiunittests barrier -j$(nproc)
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='Proximity*:*Zeroconf*'
```

- [ ] **Step 8: Commit**

```sh
git add src/gui/src/MainWindow.* src/gui/src/ZeroconfService.* \
  doc/newsfragments/12.feature
git commit -m "feat: gate macOS client lifecycle by proximity #12"
```

## Task 8: macOS end-to-end verification

**Files:** No production changes expected.

- [ ] **Step 1: Build and package an arm64 app**

```sh
cmake -S . -B build-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=macosx \
  -DBARRIER_BUILD_TESTS=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-arm64 -j$(nproc)
```

- [ ] **Step 2: Run the complete relevant automated suite**

```sh
QT_QPA_PLATFORM=offscreen ./build-arm64/bin/guiunittests \
  --gtest_filter='Proximity*:*Zeroconf*:*TrayIcon*'
./build-arm64/bin/unittests --gtest_filter='DisplayTopology*'
```

- [ ] **Step 3: Exercise the real menu-bar surface**

Verify with two macOS machines:

1. main window hidden while the menu icon remains;
2. paired server absent: no `barrierc` process and no TCP attempts;
3. BLE near but Bonjour absent: `Waiting for network` and no TCP attempt;
4. Bonjour present but RSSI below threshold: `Waiting for nearby server`;
5. both gates stable: exactly one `barrierc --no-restart` process;
6. brief obstruction: no disconnect;
7. ten-second sustained departure: process stops and stays stopped;
8. Bluetooth disabled: paused status and session-only override;
9. another nearby Barrier server: ignored because its `proximity-id` differs;
10. application quit: BLE scan/advertisement ends.

- [ ] **Step 4: Inspect process and network evidence**

Use the actual process list and Barrier logs to prove the waiting states create no client TCP attempts. A UI status alone is insufficient.

- [ ] **Step 5: Run diff and public-data checks**

```sh
git diff --check origin/main...HEAD
```

Confirm no local machine names, IP addresses, Bluetooth identifiers, calibration samples, or private deployment paths entered tracked files.

- [ ] **Step 6: Commit any test-only correction with `#12`; otherwise leave the verified implementation commits unchanged**
