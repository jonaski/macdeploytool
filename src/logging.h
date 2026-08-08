// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#ifndef LOGGING_H
#define LOGGING_H

#include <QDebug>

// Verbosity threshold: 0 = no output, 1 = error/warning (default), 2 = normal, 3 = debug.
extern int log_level;
extern int log_errors;

// Each returns a QDebug that prints when the current log_level allows it for that severity, and silently discards the streamed message otherwise.
QDebug LogError();
QDebug LogWarning();
QDebug LogNormal();
QDebug LogDebug();

#endif  // LOGGING_H
