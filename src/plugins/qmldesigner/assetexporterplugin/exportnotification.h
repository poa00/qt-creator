// Copyright (C) 2020 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <QString>

namespace QmlDesigner {
class ExportNotification
{
public:
    static void addError(const QString &errMsg);
    static void addWarning(const QString &warningMsg);
    static void addInfo(const QString &infoMsg);
};
} // QmlDesigner
