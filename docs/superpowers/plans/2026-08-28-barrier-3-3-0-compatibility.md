# Barrier 3.3.0 Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Release Barrier 3.3.0 with 3.2.0 features while restoring interoperability with Barrier 2.4.0 peers.

**Architecture:** Capability is negotiated per peer. Protocol 1.6 peers receive only 2.4.0-compatible messages and participate in layout as one synthetic rectangle; protocol 1.7 peers keep display geometry and display names.

**Tech Stack:** C++14, CMake, GoogleTest, Barrier protocol 1.x.

---

## Files

- Modify: `src/lib/barrier/DisplayNames.h`
- Modify: `src/lib/barrier/DisplayNames.cpp`
- Modify: `src/lib/client/Client.h`
- Modify: `src/lib/client/Client.cpp`
- Modify: `src/lib/client/ServerProxy.cpp`
- Modify: `src/lib/server/ClientProxy1_0.cpp`
- Modify: `src/test/unittests/barrier/DisplayNamesTests.cpp`
- Modify: `src/test/unittests/barrier/ClientProxyDisplayNamesTests.cpp`
- Modify: `cmake/Version.cmake`
- Modify: `Build.properties`
- Modify: `README.md`
- Create: `doc/newsfragments/5.compatibility`

## Task 1: Feature-named protocol capability helpers

- [ ] Add `supportsDisplayGeometry(SInt16 minorVersion)` to `src/lib/barrier/DisplayNames.h` beside `supportsDisplayNames`.
- [ ] Implement it in `src/lib/barrier/DisplayNames.cpp` as `return minorVersion >= 7;`.
- [ ] Change `supportsDisplayNames` to also use the explicit feature-minor constant, not `kProtocolMinorVersion` directly.
- [ ] Add unit tests:

```cpp
TEST(DisplayNamesTests, protocolCapabilities_geometryStartsAt17)
{
    EXPECT_FALSE(supportsDisplayGeometry(6));
    EXPECT_TRUE(supportsDisplayGeometry(7));
}

TEST(DisplayNamesTests, protocolCapabilities_namesStartAt17)
{
    EXPECT_FALSE(supportsDisplayNames(6));
    EXPECT_TRUE(supportsDisplayNames(7));
}
```

- [ ] Run: `./build-arm64/bin/unittests --gtest_filter='DisplayNamesTests.protocolCapabilities*'`

## Task 2: Gate client-to-server display geometry

- [ ] Add `bool supportsDisplayGeometry() const;` to `src/lib/client/Client.h` near `supportsDisplayNames()`.
- [ ] Implement it in `src/lib/client/Client.cpp` using the server's negotiated minor version.
- [ ] In `src/lib/client/ServerProxy.cpp::queryInfo()`, compute `const bool sendDisplayGeometry = m_client->supportsDisplayGeometry() && !info.m_displays.empty();`.
- [ ] Send `DDIS` only when `sendDisplayGeometry` is true.
- [ ] Send `DDNM` only when `sendDisplayGeometry` is true, the server supports display names, and the names match the display count.
- [ ] Verify that protocol 1.6 servers receive `DINF` only.

## Task 3: Synthetic rectangle fallback for old clients

- [ ] In `src/lib/server/ClientProxy1_0.cpp::getDisplays()`, return received `m_info.m_displays` when non-empty.
- [ ] When `m_info.m_displays` is empty and `m_info.m_w > 0 && m_info.m_h > 0`, return a single `ScreenRect` from `DINF`.
- [ ] Keep `getDisplayNames()` empty for peers without names.
- [ ] Add unit test proving a client that sent only `DINF` reports one synthetic rectangle.
- [ ] Add unit test proving a client that sent `DDIS` reports the real display rectangles, not the synthetic fallback.

## Task 4: Version and release notes

- [ ] Change version metadata from 3.2.0 to 3.3.0 in `cmake/Version.cmake` and `Build.properties`.
- [ ] Update the README fork section generically. It may mention 3.3.0 compatibility but must not mention local machine names, hostnames, IP addresses, or private deployment mapping.
- [ ] Add `doc/newsfragments/5-compat.bugfix` with a public-safe summary.

## Task 5: Verification

- [ ] Configure with tests if needed:

```sh
cmake -S . -B build-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=macosx \
  -DBARRIER_BUILD_TESTS=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

- [ ] Build: `cmake --build build-arm64 -j$(nproc)`.
- [ ] Run focused tests:

```sh
./build-arm64/bin/unittests --gtest_filter='DisplayNamesTests.*:ClientProxyDisplayNamesTests.*'
```

- [ ] Run client/server smoke test with a 3.x server and 3.x client.
- [ ] Install the built app on the two local compatibility-test machines using the private mapping outside tracked files.
- [ ] Do not write local machine names, hostnames, IP addresses, or private deployment mapping to any committed file.

## Verification results

- Focused protocol compatibility unit tests passed: `DisplayNamesTests.*` and `ClientProxyDisplayNamesTests.*`.
- Release build produced `barrierc` and `barriers` reporting `3.3.0-release` and protocol `1.7`.
- Manual surface testing confirmed both Barrier 2.4.0 and Barrier 3.3.0 peers work with the 3.3.0 build.
- Changed tracked files were scanned for private machine names, hostnames, IP addresses, fleet terms, and local source paths; no matches were found.
