// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#ifndef DEPLOYMENTINFO_H
#define DEPLOYMENTINFO_H

#include <QString>
#include <QStringList>
#include <QList>

class DeploymentInfo {
 public:
  QString qt_path;
  QString plugin_path;
  QStringList deployed_frameworks;
  QList<QString> rpaths_used;
  bool use_loader_path;
  bool is_framework;
  bool is_debug;
  QString gio_modules_directory;
  QString gstreamer_plugins_directory;

  bool ContainsModule(const QString &module, const QString &lib_in_fix) const;
};

#endif  // DEPLOYMENTINFO_H
