// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#ifndef FRAMEWORKINFO_H
#define FRAMEWORKINFO_H

#include <QString>
#include <QDebug>

class FrameworkInfo {
 public:
  FrameworkInfo();

  bool is_dylib;
  QString framework_directory;
  QString framework_name;
  QString framework_path;
  QString binary_directory;
  QString binary_name;
  QString binary_path;
  QString rpath_used;
  QString version;
  QString install_name;
  QString deployed_install_name;
  QString source_file_path;
  QString framework_destination_directory;
  QString binary_destination_directory;

  bool IsDebugLibrary() const;
};

bool operator==(const FrameworkInfo &a, const FrameworkInfo &b);
QDebug operator<<(QDebug debug, const FrameworkInfo &info);

#endif  // FRAMEWORKINFO_H
