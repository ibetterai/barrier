/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "base/Sha256.h"

#include <gtest/gtest.h>

TEST(Sha256Tests, hashesKnownVector)
{
    EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad",
              barrier::sha256Hex("abc"));
}
