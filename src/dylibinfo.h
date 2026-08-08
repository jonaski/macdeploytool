// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#ifndef DYLIBINFO_H
#define DYLIBINFO_H

#include <QString>
#include <QVersionNumber>

class DylibInfo {
 public:
  QString binary_path;
  QVersionNumber current_version;
  QVersionNumber compatibility_version;
};

#endif  // DYLIBINFO_H
