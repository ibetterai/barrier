/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace barrier {

enum class ClientPresenceState {
    Unavailable,
    Available,
    Stale,
};

struct ClientRouteAssociation {
    std::string routingId;
    std::string screenName;
};

struct ClientPresenceRow {
    std::string routingId;
    std::string screenName;
    ClientPresenceState state{ClientPresenceState::Unavailable};
    bool hasFilteredRssi{false};
    double filteredRssiDbm{0.0};
    bool hasLastSeen{false};
    std::uint64_t lastSeenMs{0};
};

class ClientPresenceRegistry {
public:
    // This view model intentionally reports signal presence only. It does not
    // reuse the client connection gate's configurable enter/exit thresholds.
    explicit ClientPresenceRegistry(std::uint64_t staleAfterMs = 5000);
    ~ClientPresenceRegistry();

    ClientPresenceRegistry(ClientPresenceRegistry&&) noexcept;
    ClientPresenceRegistry& operator=(ClientPresenceRegistry&&) noexcept;
    ClientPresenceRegistry(const ClientPresenceRegistry&) = delete;
    ClientPresenceRegistry& operator=(const ClientPresenceRegistry&) = delete;

    static bool isValidRoutingId(const std::string& routingId);

    void replaceRoutes(const std::vector<ClientRouteAssociation>& routes);
    // Returns false for malformed, ambiguous, colliding, or non-monotonic
    // observations. Valid unassociated observations are retained for
    // BLE-first discovery but never create a named row by themselves.
    bool observe(const std::string& peripheralId,
                 const std::string& routingId,
                 int rssiDbm,
                 std::uint64_t monotonicMs);
    std::vector<ClientPresenceRow> rows(std::uint64_t monotonicMs) const;
    void clearObservations();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace barrier
