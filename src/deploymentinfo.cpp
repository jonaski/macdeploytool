// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include <QRegularExpression>

#include "deploymentinfo.h"

using namespace Qt::StringLiterals;

bool DeploymentInfo::ContainsModule(const QString &module, const QString &lib_in_fix) const {

  // Check for framework first
  if (deployed_frameworks.contains("Qt"_L1 + module + lib_in_fix + ".framework"_L1)) {
    return true;
  }
  // Check for dylib
  const QRegularExpression dylib_reg_exp("libQt[0-9]+"_L1 + module + lib_in_fix + (is_debug ? "_debug"_L1 : ""_L1) + ".[0-9]+.dylib"_L1);
  return deployed_frameworks.filter(dylib_reg_exp).size() > 0;

}
