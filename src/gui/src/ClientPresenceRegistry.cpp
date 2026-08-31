/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ClientPresenceRegistry.h"

#include <map>
#include <set>
#include <utility>

namespace barrier {
namespace {

const double kRssiSampleWeight = 0.25;
const int kMinimumBleRssiDbm = -127;
const int kMaximumBleRssiDbm = 20;

bool isStale(std::uint64_t nowMs,
             std::uint64_t lastSeenMs,
             std::uint64_t staleAfterMs)
{
    return nowMs < lastSeenMs || nowMs - lastSeenMs >= staleAfterMs;
}

bool intervalElapsed(std::uint64_t nowMs,
                     std::uint64_t startedMs,
                     std::uint64_t durationMs)
{
    return nowMs >= startedMs && nowMs - startedMs >= durationMs;
}

bool isValidRssi(int rssiDbm)
{
    // CoreBluetooth uses +127 when no RSSI value is available. Keep the
    // accepted range bounded so corrupt scanner input cannot pollute the UI.
    return rssiDbm >= kMinimumBleRssiDbm &&
           rssiDbm <= kMaximumBleRssiDbm;
}

} // namespace

class ClientPresenceRegistry::Impl {
public:
    struct Observation {
        std::string peripheralId;
        double filteredRssiDbm{0.0};
        std::uint64_t lastSeenMs{0};
    };

    explicit Impl(std::uint64_t staleAfterMs) :
        staleAfterMs(staleAfterMs)
    {
    }

    void invalidateRoutingId(const std::string& routingId)
    {
        const auto observation = observations.find(routingId);
        if (observation != observations.end()) {
            const auto peripheral =
                peripheralToRoutingId.find(observation->second.peripheralId);
            if (peripheral != peripheralToRoutingId.end() &&
                peripheral->second == routingId) {
                peripheralToRoutingId.erase(peripheral);
            }
            observations.erase(observation);
        }
        collisions.erase(routingId);
    }

    std::uint64_t staleAfterMs;
    std::map<std::string, std::string> routes;
    std::map<std::string, Observation> observations;
    std::map<std::string, std::string> peripheralToRoutingId;
    std::map<std::string, std::uint64_t> collisions;
    std::set<std::string> blockedRoutingIds;
    std::set<std::string> removedRoutingIds;
};

ClientPresenceRegistry::ClientPresenceRegistry(
    std::uint64_t staleAfterMs) :
    m_impl(new Impl(staleAfterMs))
{
}

ClientPresenceRegistry::~ClientPresenceRegistry() = default;
ClientPresenceRegistry::ClientPresenceRegistry(
    ClientPresenceRegistry&&) noexcept = default;
ClientPresenceRegistry& ClientPresenceRegistry::operator=(
    ClientPresenceRegistry&&) noexcept = default;

bool ClientPresenceRegistry::isValidRoutingId(
    const std::string& routingId)
{
    if (routingId.size() != 32) {
        return false;
    }
    for (const char value : routingId) {
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

void ClientPresenceRegistry::replaceRoutes(
    const std::vector<ClientRouteAssociation>& associations)
{
    std::map<std::string, std::string> nextRoutes;
    std::set<std::string> ambiguousRoutingIds;
    for (const auto& association : associations) {
        if (!isValidRoutingId(association.routingId) ||
            association.screenName.empty() ||
            ambiguousRoutingIds.count(association.routingId) != 0) {
            continue;
        }

        const auto existing = nextRoutes.find(association.routingId);
        if (existing == nextRoutes.end()) {
            nextRoutes.emplace(association.routingId,
                               association.screenName);
        }
        else if (existing->second != association.screenName) {
            nextRoutes.erase(existing);
            ambiguousRoutingIds.insert(association.routingId);
        }
    }

    for (const auto& route : m_impl->routes) {
        if (nextRoutes.count(route.first) == 0) {
            m_impl->invalidateRoutingId(route.first);
            m_impl->removedRoutingIds.insert(route.first);
        }
    }
    for (const auto& routingId : ambiguousRoutingIds) {
        m_impl->invalidateRoutingId(routingId);
        m_impl->removedRoutingIds.insert(routingId);
    }
    for (const auto& route : nextRoutes) {
        if (m_impl->removedRoutingIds.erase(route.first) != 0) {
            // Re-association must not resurrect a sample or peripheral link
            // captured while the route was absent or ambiguous.
            m_impl->invalidateRoutingId(route.first);
        }
    }

    m_impl->routes = std::move(nextRoutes);
    m_impl->blockedRoutingIds = std::move(ambiguousRoutingIds);
}

bool ClientPresenceRegistry::observe(const std::string& peripheralId,
                                     const std::string& routingId,
                                     int rssiDbm,
                                     std::uint64_t monotonicMs)
{
    if (peripheralId.empty() || !isValidRoutingId(routingId) ||
        !isValidRssi(rssiDbm) ||
        m_impl->blockedRoutingIds.count(routingId) != 0) {
        return false;
    }

    const auto collision = m_impl->collisions.find(routingId);
    if (collision != m_impl->collisions.end()) {
        if (intervalElapsed(monotonicMs, collision->second,
                            m_impl->staleAfterMs)) {
            m_impl->collisions.erase(collision);
        }
        else {
            return false;
        }
    }

    const auto peripheral =
        m_impl->peripheralToRoutingId.find(peripheralId);
    if (peripheral != m_impl->peripheralToRoutingId.end() &&
        peripheral->second != routingId) {
        const auto oldObservation =
            m_impl->observations.find(peripheral->second);
        if (oldObservation != m_impl->observations.end() &&
            monotonicMs < oldObservation->second.lastSeenMs) {
            return false;
        }
        m_impl->invalidateRoutingId(peripheral->second);
    }

    auto observation = m_impl->observations.find(routingId);
    if (observation != m_impl->observations.end() &&
        observation->second.peripheralId != peripheralId) {
        if (monotonicMs < observation->second.lastSeenMs) {
            return false;
        }
        if (!isStale(monotonicMs, observation->second.lastSeenMs,
                     m_impl->staleAfterMs)) {
            m_impl->invalidateRoutingId(routingId);
            m_impl->collisions.emplace(routingId, monotonicMs);
            return false;
        }
        m_impl->invalidateRoutingId(routingId);
        observation = m_impl->observations.end();
    }

    if (observation != m_impl->observations.end()) {
        if (monotonicMs < observation->second.lastSeenMs) {
            return false;
        }
        observation->second.filteredRssiDbm =
            kRssiSampleWeight * rssiDbm +
            (1.0 - kRssiSampleWeight) *
                observation->second.filteredRssiDbm;
        observation->second.lastSeenMs = monotonicMs;
    }
    else {
        Impl::Observation first;
        first.peripheralId = peripheralId;
        first.filteredRssiDbm = rssiDbm;
        first.lastSeenMs = monotonicMs;
        m_impl->observations.emplace(routingId, std::move(first));
    }
    m_impl->peripheralToRoutingId[peripheralId] = routingId;
    return true;
}

std::vector<ClientPresenceRow> ClientPresenceRegistry::rows(
    std::uint64_t monotonicMs) const
{
    std::vector<ClientPresenceRow> result;
    result.reserve(m_impl->routes.size());
    for (const auto& route : m_impl->routes) {
        ClientPresenceRow row;
        row.routingId = route.first;
        row.screenName = route.second;

        const auto observation = m_impl->observations.find(route.first);
        if (observation != m_impl->observations.end() &&
            m_impl->collisions.count(route.first) == 0) {
            row.hasFilteredRssi = true;
            row.filteredRssiDbm = observation->second.filteredRssiDbm;
            row.hasLastSeen = true;
            row.lastSeenMs = observation->second.lastSeenMs;
            row.state = isStale(monotonicMs,
                                observation->second.lastSeenMs,
                                m_impl->staleAfterMs)
                ? ClientPresenceState::Stale
                : ClientPresenceState::Available;
        }
        result.push_back(std::move(row));
    }
    return result;
}

void ClientPresenceRegistry::clearObservations()
{
    m_impl->observations.clear();
    m_impl->peripheralToRoutingId.clear();
    m_impl->collisions.clear();
}

} // namespace barrier
