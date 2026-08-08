// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "frameworkinfo.h"

using namespace Qt::StringLiterals;

FrameworkInfo::FrameworkInfo() : is_dylib(false) {}

bool FrameworkInfo::IsDebugLibrary() const {

  return is_dylib ? binary_name.contains("_debug."_L1) : binary_name.endsWith("_debug"_L1);

}
