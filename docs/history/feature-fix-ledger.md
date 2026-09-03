# Feature and fix ledger

This ledger gives reconstructed fork-specific history stable public IDs. The
IDs describe product behavior; they are not links to legacy tracker objects.

| ID | Date | Type | Summary | Status | First version |
|---|---|---|---|---|---|
| H-001 | 2026-08-29 | Feature | Native Apple Silicon macOS packaging | Shipped | 3.2.0 |
| H-002 | 2026-08-29 | Feature | Physical display geometry and freeform L-shaped layouts | Shipped | 3.2.0 |
| H-003 | 2026-08-29 | Feature | Routing along real display-rectangle edges | Shipped | 3.2.0 |
| H-004 | 2026-08-29 | Feature | OS-provided labels for physical displays | Shipped | 3.2.0 |
| H-005 | 2026-08-29 | Feature | Client display wake when the pointer enters | Shipped | 3.2.0 |
| H-006 | 2026-08-29 | Fix | Persistent layouts and reliable login-session input | Shipped | 3.2.0 |
| H-007 | 2026-08-29 | Fix | Consistent perceived trackpad and mouse scrolling | Shipped | 3.2.0 |
| H-008 | 2026-08-29 | Fix | Reliable activation of menu-bar windows | Shipped | 3.2.0 |
| H-009 | 2026-08-29 | Feature | Magic Mouse Spaces gesture forwarding | Shipped | 3.2.0 |
| H-010 | 2026-08-29 | Compatibility | Protocol interoperability with 2.4.0 and earlier peers | Shipped | 3.3.0 |
| H-011 | 2026-08-29 | Compatibility | Capability-gated enhanced display metadata | Shipped | 3.3.0 |
| H-012 | 2026-08-29 | Compatibility | Rectangular fallback for older peers | Shipped | 3.3.0 |
| H-013 | 2026-08-29 | Fix | Freeform coordinate-rounding gap tolerance | Shipped | 3.3.1 |
| H-014 | 2026-08-29 | Fix | Partial-edge coordinate mapping without cursor bounce | Shipped | 3.3.1 |
| H-015 | 2026-08-29 | Fix | Freeform display-rectangle handling for portrait and multi-display clients, including multiple simultaneous connections | Shipped | 3.3.1 |
| H-016 | 2026-08-31 | Feature | Exact display-topology profiles | Shipped | 3.4.0 |
| H-017 | 2026-08-31 | Safety | Switching pause for unknown display topologies | Shipped | 3.4.0 |
| H-018 | 2026-08-31 | Feature | Paired Bluetooth and Bonjour proximity gating | Shipped | 3.4.0 |
| H-019 | 2026-08-31 | Feature | User-adjustable signal thresholds with hysteresis | Shipped | 3.4.0 |
| H-020 | 2026-08-31 | Feature | Optional pair-scoped client signal sharing | Shipped | 3.4.0 |
| H-021 | 2026-08-31 | Feature | Rate-limited network wake for offline clients | Shipped | 3.4.0 |
| H-022 | 2026-08-31 | Fix | Automatic client reconnection after topology-driven server reloads | Shipped | 3.4.0 |
| H-023 | 2026-08-31 | Fix | Pointer transfer after macOS sleep/wake through main-run-loop event capture and disabled-tap recovery | Shipped | 3.4.0 |
| H-024 | 2026-08-31 | Fix | Manual server address precedence when Auto config is off | Shipped | 3.4.0 |
| H-025 | 2026-08-31 | Fix | Deduplicated service-resolution snapshots and endpoint logs | Shipped | 3.4.0 |
| H-026 | 2026-08-31 | Release | Pinned dependencies, deployment checks, and privacy audits | Shipped | 3.4.0 |
| H-027 | 2026-08-31 | Fix | Preserved paired discovery metadata, selected routable server endpoints, and rejected auxiliary discovery endpoints | Shipped | 3.4.0 |
| H-028 | 2026-08-31 | Fix | Event-queue readiness wakes every caller already waiting at startup | Shipped | 3.4.0 |
| H-029 | 2026-08-31 | Fix | Reconciled saved topology profiles after configured screens change | Shipped | 3.4.1 |
| H-030 | 2026-08-31 | Fix | Explicit Stop cancels queued restarts and configuration failures stop retrying | Shipped | 3.4.1 |
| H-031 | 2026-09-03 | Fix | Forwarded Fn/Globe, left/right modifiers, and brightness keys to the client with held-modifier replay on screen switch | Shipped | 3.4.2 |
| H-032 | 2026-09-03 | Fix | Synthesized Fn/Globe and right-hand modifiers as FlagsChanged events on the macOS client | Shipped | 3.4.3 |
| H-033 | 2026-09-03 | Fix | Forwarded the Globe-tap key (key code 179) as its own Globe key with matching client synthesis | Shipped | 3.4.4 |
| H-034 | 2026-09-03 | Fix | Forwarded Spotlight, Dictation, and Do Not Disturb keys (codes 177, 176, 178) as their own keys | Shipped | 3.4.5 |
| H-035 | 2026-09-03 | Fix | Replayed SecondaryFn flag on synthesized function-row keys | Shipped | 3.4.6 |
| H-036 | 2026-09-03 | Fix | Normalized bundled runpaths before the release audit (unblocks public dmg) | Shipped | 3.4.7 |

Upstream history before this fork is retained in the source-tree changelogs and
release notes.
