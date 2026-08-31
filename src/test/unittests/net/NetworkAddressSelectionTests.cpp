/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "arch/NetworkAddressSelection.h"

#include <gtest/gtest.h>

namespace {

struct AddressInfoFixture {
    addrinfo info{};
    sockaddr_storage address{};

    explicit AddressInfoFixture(int family)
    {
        info.ai_family = family;
        info.ai_addr = reinterpret_cast<sockaddr*>(&address);
        if (family == AF_INET) {
            info.ai_addrlen = sizeof(sockaddr_in);
            reinterpret_cast<sockaddr_in*>(&address)->sin_family = AF_INET;
        }
        else if (family == AF_INET6) {
            info.ai_addrlen = sizeof(sockaddr_in6);
            reinterpret_cast<sockaddr_in6*>(&address)->sin6_family = AF_INET6;
        }
    }

    sockaddr_in6& ipv6()
    {
        return *reinterpret_cast<sockaddr_in6*>(&address);
    }

    sockaddr_in& ipv4()
    {
        return *reinterpret_cast<sockaddr_in*>(&address);
    }
};

void makeLinkLocal(AddressInfoFixture& fixture, unsigned int scopeId,
                   unsigned char secondByte = 0x80)
{
    fixture.ipv6().sin6_addr.s6_addr[0] = 0xfe;
    fixture.ipv6().sin6_addr.s6_addr[1] = secondByte;
    fixture.ipv6().sin6_scope_id = scopeId;
}

void makeIpv4(AddressInfoFixture& fixture, unsigned char first,
              unsigned char second, unsigned char third,
              unsigned char fourth)
{
    unsigned char* bytes = reinterpret_cast<unsigned char*>(
        &fixture.ipv4().sin_addr);
    bytes[0] = first;
    bytes[1] = second;
    bytes[2] = third;
    bytes[3] = fourth;
}

} // namespace

TEST(NetworkAddressSelectionTests,
     PrefersIpv4OverUnscopedAndScopedLinkLocalIpv6)
{
    AddressInfoFixture unscoped(AF_INET6);
    AddressInfoFixture scoped(AF_INET6);
    AddressInfoFixture ipv4(AF_INET);
    makeLinkLocal(unscoped, 0);
    makeLinkLocal(scoped, 17);
    makeIpv4(ipv4, 192, 168, 1, 19);
    unscoped.info.ai_next = &scoped.info;
    scoped.info.ai_next = &ipv4.info;

    EXPECT_EQ(&ipv4.info,
              barrier::selectPreferredAddressInfo(&unscoped.info));
}

TEST(NetworkAddressSelectionTests, PrefersUlaOverScopedLinkLocalIpv6)
{
    AddressInfoFixture scoped(AF_INET6);
    AddressInfoFixture ula(AF_INET6);
    makeLinkLocal(scoped, 16);
    ula.ipv6().sin6_addr.s6_addr[0] = 0xfd;
    scoped.info.ai_next = &ula.info;

    EXPECT_EQ(&ula.info,
              barrier::selectPreferredAddressInfo(&scoped.info));
}

TEST(NetworkAddressSelectionTests, RetainsScopedLinkLocalAsLastResort)
{
    AddressInfoFixture first(AF_INET6);
    AddressInfoFixture second(AF_INET6);
    makeLinkLocal(first, 16);
    makeLinkLocal(second, 17);
    first.info.ai_next = &second.info;

    EXPECT_EQ(&first.info,
              barrier::selectPreferredAddressInfo(&first.info));
}

TEST(NetworkAddressSelectionTests, RejectsUnscopedLinkLocalOnlyList)
{
    AddressInfoFixture unscoped(AF_INET6);
    makeLinkLocal(unscoped, 0);

    EXPECT_EQ(nullptr,
              barrier::selectPreferredAddressInfo(&unscoped.info));
}

TEST(NetworkAddressSelectionTests, PreservesResolverOrderWithinPriority)
{
    AddressInfoFixture first(AF_INET);
    AddressInfoFixture second(AF_INET6);
    makeIpv4(first, 192, 168, 1, 19);
    second.ipv6().sin6_addr.s6_addr[0] = 0xfd;
    first.info.ai_next = &second.info;

    EXPECT_EQ(&first.info,
              barrier::selectPreferredAddressInfo(&first.info));
}

TEST(NetworkAddressSelectionTests, KeepsGlobalIpv6BeforeLaterIpv4)
{
    AddressInfoFixture globalIpv6(AF_INET6);
    AddressInfoFixture ipv4(AF_INET);
    globalIpv6.ipv6().sin6_addr.s6_addr[0] = 0x20;
    globalIpv6.ipv6().sin6_addr.s6_addr[1] = 0x01;
    globalIpv6.info.ai_next = &ipv4.info;

    EXPECT_EQ(&globalIpv6.info,
              barrier::selectPreferredAddressInfo(&globalIpv6.info));
}

TEST(NetworkAddressSelectionTests, ClassifiesFullFe80PrefixAsLinkLocal)
{
    AddressInfoFixture scopedUpperEdge(AF_INET6);
    AddressInfoFixture ipv4(AF_INET);
    makeLinkLocal(scopedUpperEdge, 16, 0xbf);
    makeIpv4(ipv4, 192, 168, 1, 19);
    scopedUpperEdge.info.ai_next = &ipv4.info;

    EXPECT_EQ(&ipv4.info,
              barrier::selectPreferredAddressInfo(&scopedUpperEdge.info));

    AddressInfoFixture unscopedUpperEdge(AF_INET6);
    makeLinkLocal(unscopedUpperEdge, 0, 0xbf);
    EXPECT_EQ(nullptr,
              barrier::selectPreferredAddressInfo(&unscopedUpperEdge.info));
}

TEST(NetworkAddressSelectionTests, PrefersLanIpv4OverIpv4LinkLocal)
{
    AddressInfoFixture linkLocal(AF_INET);
    AddressInfoFixture lan(AF_INET);
    makeIpv4(linkLocal, 169, 254, 10, 20);
    makeIpv4(lan, 192, 168, 1, 219);
    linkLocal.info.ai_next = &lan.info;

    EXPECT_EQ(&lan.info,
              barrier::selectPreferredAddressInfo(&linkLocal.info));
}

TEST(NetworkAddressSelectionTests, RetainsIpv4LinkLocalAsLastResort)
{
    AddressInfoFixture linkLocal(AF_INET);
    makeIpv4(linkLocal, 169, 254, 10, 20);

    EXPECT_EQ(&linkLocal.info,
              barrier::selectPreferredAddressInfo(&linkLocal.info));
}

TEST(NetworkAddressSelectionTests,
     PrefersUnicastButRetainsIpv4WildcardForServerBinding)
{
    AddressInfoFixture ipv4Unspecified(AF_INET);
    AddressInfoFixture ipv4Multicast(AF_INET);
    AddressInfoFixture ipv6Unspecified(AF_INET6);
    AddressInfoFixture ipv6Multicast(AF_INET6);
    AddressInfoFixture usable(AF_INET);
    makeIpv4(ipv4Unspecified, 0, 0, 0, 0);
    makeIpv4(ipv4Multicast, 224, 0, 0, 1);
    ipv6Multicast.ipv6().sin6_addr.s6_addr[0] = 0xff;
    makeIpv4(usable, 172, 22, 19, 19);
    ipv4Unspecified.info.ai_next = &ipv4Multicast.info;
    ipv4Multicast.info.ai_next = &ipv6Unspecified.info;
    ipv6Unspecified.info.ai_next = &ipv6Multicast.info;
    ipv6Multicast.info.ai_next = &usable.info;

    EXPECT_EQ(&usable.info,
              barrier::selectPreferredAddressInfo(&ipv4Unspecified.info));
    usable.info.ai_next = nullptr;
    ipv6Multicast.info.ai_next = nullptr;
    EXPECT_EQ(&ipv4Unspecified.info,
              barrier::selectPreferredAddressInfo(&ipv4Unspecified.info));
}

TEST(NetworkAddressSelectionTests, RetainsIpv6WildcardForServerBinding)
{
    AddressInfoFixture ipv6Unspecified(AF_INET6);

    EXPECT_EQ(&ipv6Unspecified.info,
              barrier::selectPreferredAddressInfo(&ipv6Unspecified.info));
}

TEST(NetworkAddressSelectionTests, PrefersIpv4LinkLocalOverEarlierWildcard)
{
    AddressInfoFixture wildcard(AF_INET);
    AddressInfoFixture linkLocal(AF_INET);
    makeIpv4(linkLocal, 169, 254, 10, 20);
    wildcard.info.ai_next = &linkLocal.info;

    EXPECT_EQ(&linkLocal.info,
              barrier::selectPreferredAddressInfo(&wildcard.info));
}

TEST(NetworkAddressSelectionTests,
     PrefersScopedIpv6LinkLocalOverEarlierWildcard)
{
    AddressInfoFixture wildcard(AF_INET6);
    AddressInfoFixture linkLocal(AF_INET6);
    makeLinkLocal(linkLocal, 17);
    wildcard.info.ai_next = &linkLocal.info;

    EXPECT_EQ(&linkLocal.info,
              barrier::selectPreferredAddressInfo(&wildcard.info));
}

TEST(NetworkAddressSelectionTests, SkipsMalformedAndUnsupportedCandidates)
{
    AddressInfoFixture malformed(AF_INET6);
    AddressInfoFixture unsupported(AF_INET);
    AddressInfoFixture usable(AF_INET);
    malformed.info.ai_addrlen = sizeof(sockaddr_in);
    unsupported.info.ai_family = AF_UNSPEC;
    makeIpv4(usable, 172, 22, 19, 19);
    malformed.info.ai_next = &unsupported.info;
    unsupported.info.ai_next = &usable.info;

    EXPECT_EQ(&usable.info,
              barrier::selectPreferredAddressInfo(&malformed.info));
}

TEST(NetworkAddressSelectionTests, SkipsNullMismatchedAndOversizeCandidates)
{
    AddressInfoFixture nullAddress(AF_INET);
    AddressInfoFixture mismatched(AF_INET6);
    AddressInfoFixture oversize(AF_INET);
    AddressInfoFixture usable(AF_INET);
    nullAddress.info.ai_addr = nullptr;
    reinterpret_cast<sockaddr_in6*>(&mismatched.address)->sin6_family = AF_INET;
    oversize.info.ai_addrlen = sizeof(sockaddr_storage) + 1;
    makeIpv4(usable, 172, 22, 19, 19);
    nullAddress.info.ai_next = &mismatched.info;
    mismatched.info.ai_next = &oversize.info;
    oversize.info.ai_next = &usable.info;

    EXPECT_EQ(&usable.info,
              barrier::selectPreferredAddressInfo(&nullAddress.info));
}
