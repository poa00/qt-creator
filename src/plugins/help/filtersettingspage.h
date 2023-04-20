// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <coreplugin/dialogs/ioptionspage.h>

namespace Help::Internal {

class FilterSettingsPage : public Core::IOptionsPage
{
    Q_OBJECT

public:
    FilterSettingsPage();

signals:
    void filtersChanged();
};

} // Help::Internal
