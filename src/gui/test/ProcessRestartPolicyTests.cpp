/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ProcessRestartPolicy.h"

#include "common/common.h"

#include <gtest/gtest.h>

TEST(ProcessRestartPolicyTests, explicitStopCancelsPendingRetry)
{
    barrier::ProcessRestartPolicy policy;
    policy.requestStart();

    EXPECT_EQ(barrier::ProcessExitAction::RetryAfterDelay,
              policy.childExited(kExitFailed, true));
    ASSERT_TRUE(policy.retryPending());

    policy.requestStop();

    EXPECT_FALSE(policy.runIntended());
    EXPECT_FALSE(policy.retryPending());
    EXPECT_FALSE(policy.takeScheduledRetry());
}

TEST(ProcessRestartPolicyTests, configurationExitStopsWithoutRetry)
{
    barrier::ProcessRestartPolicy policy;
    policy.requestStart();

    EXPECT_EQ(barrier::ProcessExitAction::StopForConfigurationError,
              policy.childExited(kExitConfig, true));
    EXPECT_FALSE(policy.runIntended());
    EXPECT_FALSE(policy.retryPending());
    EXPECT_FALSE(policy.takeScheduledRetry());
}

TEST(ProcessRestartPolicyTests, recoverableExitAllowsOneScheduledRetry)
{
    barrier::ProcessRestartPolicy policy;
    policy.requestStart();

    EXPECT_EQ(barrier::ProcessExitAction::RetryAfterDelay,
              policy.childExited(kExitFailed, true));
    EXPECT_TRUE(policy.runIntended());
    EXPECT_TRUE(policy.takeScheduledRetry());
    EXPECT_FALSE(policy.takeScheduledRetry());
}

TEST(ProcessRestartPolicyTests, explicitStartBeginsANewIntentEpoch)
{
    barrier::ProcessRestartPolicy policy;
    policy.requestStart();
    ASSERT_EQ(barrier::ProcessExitAction::StopForConfigurationError,
              policy.childExited(kExitConfig, true));

    policy.requestStart();

    EXPECT_TRUE(policy.runIntended());
    EXPECT_FALSE(policy.retryPending());
}

TEST(ProcessRestartPolicyTests, crashExitCodeCannotMasqueradeAsConfigurationError)
{
    barrier::ProcessRestartPolicy policy;
    policy.requestStart();

    EXPECT_EQ(barrier::ProcessExitAction::RetryAfterDelay,
              policy.childExited(kExitConfig, false));
    EXPECT_TRUE(policy.runIntended());
    EXPECT_TRUE(policy.retryPending());
}
