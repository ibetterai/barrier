/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <netdb.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#endif

namespace barrier {

inline bool isSupportedAddressInfo(const addrinfo* candidate)
{
    if (candidate == nullptr || candidate->ai_addr == nullptr ||
        candidate->ai_addrlen > sizeof(sockaddr_storage)) {
        return false;
    }
    if (candidate->ai_family == AF_INET) {
        return candidate->ai_addrlen >= sizeof(sockaddr_in) &&
               candidate->ai_addr->sa_family == AF_INET;
    }
    if (candidate->ai_family == AF_INET6) {
        return candidate->ai_addrlen >= sizeof(sockaddr_in6) &&
               candidate->ai_addr->sa_family == AF_INET6;
    }
    return false;
}

inline bool isIpv6LinkLocalAddressInfo(const addrinfo* candidate)
{
    if (!isSupportedAddressInfo(candidate) ||
        candidate->ai_family != AF_INET6) {
        return false;
    }
    const sockaddr_in6* address =
        reinterpret_cast<const sockaddr_in6*>(candidate->ai_addr);
    return address->sin6_addr.s6_addr[0] == 0xfe &&
           (address->sin6_addr.s6_addr[1] & 0xc0) == 0x80;
}

inline bool isIpv4LinkLocalAddressInfo(const addrinfo* candidate)
{
    if (!isSupportedAddressInfo(candidate) ||
        candidate->ai_family != AF_INET) {
        return false;
    }
    const sockaddr_in* address =
        reinterpret_cast<const sockaddr_in*>(candidate->ai_addr);
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(&address->sin_addr);
    return bytes[0] == 169 && bytes[1] == 254;
}

inline bool isUnspecifiedAddressInfo(const addrinfo* candidate)
{
    if (!isSupportedAddressInfo(candidate)) {
        return false;
    }
    if (candidate->ai_family == AF_INET) {
        const sockaddr_in* address =
            reinterpret_cast<const sockaddr_in*>(candidate->ai_addr);
        const unsigned char* bytes =
            reinterpret_cast<const unsigned char*>(&address->sin_addr);
        return bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 &&
               bytes[3] == 0;
    }

    const sockaddr_in6* address =
        reinterpret_cast<const sockaddr_in6*>(candidate->ai_addr);
    for (unsigned int i = 0; i < 16; ++i) {
        if (address->sin6_addr.s6_addr[i] != 0) {
            return false;
        }
    }
    return true;
}

inline bool isUnusableDestinationAddressInfo(const addrinfo* candidate)
{
    if (!isSupportedAddressInfo(candidate)) {
        return true;
    }
    if (candidate->ai_family == AF_INET) {
        const sockaddr_in* address =
            reinterpret_cast<const sockaddr_in*>(candidate->ai_addr);
        const unsigned char* bytes =
            reinterpret_cast<const unsigned char*>(&address->sin_addr);
        return bytes[0] == 0 || bytes[0] >= 224;
    }

    const sockaddr_in6* address =
        reinterpret_cast<const sockaddr_in6*>(candidate->ai_addr);
    if (address->sin6_addr.s6_addr[0] == 0xff) {
        return true;
    }
    for (unsigned int i = 0; i < 16; ++i) {
        if (address->sin6_addr.s6_addr[i] != 0) {
            return false;
        }
    }
    return true;
}

inline const addrinfo* selectPreferredAddressInfo(const addrinfo* addresses)
{
    const addrinfo* linkLocalFallback = nullptr;
    const addrinfo* wildcardFallback = nullptr;
    for (const addrinfo* candidate = addresses; candidate != nullptr;
         candidate = candidate->ai_next) {
        if (!isSupportedAddressInfo(candidate)) {
            continue;
        }
        // nameToAddr serves both outbound connections and server binds. Keep
        // an explicit wildcard usable for binding, but never prefer it over a
        // concrete unicast address returned by the same resolution.
        if (isUnspecifiedAddressInfo(candidate)) {
            if (wildcardFallback == nullptr) {
                wildcardFallback = candidate;
            }
            continue;
        }
        if (isUnusableDestinationAddressInfo(candidate)) {
            continue;
        }
        if (isIpv4LinkLocalAddressInfo(candidate)) {
            if (linkLocalFallback == nullptr) {
                linkLocalFallback = candidate;
            }
            continue;
        }
        if (!isIpv6LinkLocalAddressInfo(candidate)) {
            return candidate;
        }
        const sockaddr_in6* address =
            reinterpret_cast<const sockaddr_in6*>(candidate->ai_addr);
        if (address->sin6_scope_id != 0 &&
            linkLocalFallback == nullptr) {
            linkLocalFallback = candidate;
        }
    }
    return linkLocalFallback != nullptr ? linkLocalFallback :
                                          wildcardFallback;
}

} // namespace barrier
