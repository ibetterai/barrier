/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ProcessRestartPolicy.h"

#include "common/common.h"

namespace barrier {

void ProcessRestartPolicy::requestStart()
{
    m_runIntended = true;
    m_retryPending = false;
}

void ProcessRestartPolicy::requestStop()
{
    m_runIntended = false;
    m_retryPending = false;
}

ProcessExitAction ProcessRestartPolicy::childExited(
    int exitCode, bool exitedNormally)
{
    m_retryPending = false;
    if (!m_runIntended) {
        return ProcessExitAction::Ignore;
    }
    if (exitedNormally && exitCode == kExitConfig) {
        m_runIntended = false;
        return ProcessExitAction::StopForConfigurationError;
    }

    m_retryPending = true;
    return ProcessExitAction::RetryAfterDelay;
}

bool ProcessRestartPolicy::takeScheduledRetry()
{
    if (!m_runIntended || !m_retryPending) {
        return false;
    }
    m_retryPending = false;
    return true;
}

bool ProcessRestartPolicy::runIntended() const
{
    return m_runIntended;
}

bool ProcessRestartPolicy::retryPending() const
{
    return m_retryPending;
}

} // namespace barrier
