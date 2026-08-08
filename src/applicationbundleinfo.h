// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#ifndef APPLICATIONBUNDLEINFO_H
#define APPLICATIONBUNDLEINFO_H

#include <QString>
#include <QStringList>
#include <QDebug>

class ApplicationBundleInfo {
 public:
  QString path;
  QString binary_path;
  QStringList library_paths;
};

inline QDebug operator<<(QDebug debug, const ApplicationBundleInfo &info);

#endif  // APPLICATIONBUNDLEINFO_H
