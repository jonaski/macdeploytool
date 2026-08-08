// This file is part of macdeploytool, a fork of macdeployqt
//
// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2026 Jonas Kvinge <jonas@jkvinge.net>
//
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include <QString>
#include <QDebug>

#include "logging.h"

int log_level = 1;
int log_errors = 0;

// A QDebug that writes into a scratch buffer instead of the console, used to silently swallow a streamed log message (and everything streamed into it) when the current log_level doesn't call for that severity to be printed.
static QDebug Discard() {

  static QString discard_buffer;
  discard_buffer.clear();
  return QDebug(&discard_buffer);

}

QDebug LogError() {

  log_errors++;

  if (log_level < 0) {
    return Discard();
  }

  return qDebug() << "ERROR:";

}

QDebug LogWarning() {

  if (log_level < 1) {
    return Discard();
  }

  return qDebug() << "WARNING:";

}

QDebug LogNormal() {

  if (log_level < 2) {
    return Discard();
  }

  return qDebug() << "Log:";

}

QDebug LogDebug() {

  if (log_level < 3) {
    return Discard();
  }

  return qDebug() << "Log:";

}
