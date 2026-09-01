/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

namespace barrier {

enum class ProcessExitAction {
    Ignore,
    RetryAfterDelay,
    StopForConfigurationError
};

class ProcessRestartPolicy
{
public:
    void requestStart();
    void requestStop();
    ProcessExitAction childExited(int exitCode, bool exitedNormally);
    bool takeScheduledRetry();

    bool runIntended() const;
    bool retryPending() const;

private:
    bool m_runIntended{false};
    bool m_retryPending{false};
};

} // namespace barrier
