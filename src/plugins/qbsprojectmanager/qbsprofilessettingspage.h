// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include <coreplugin/dialogs/ioptionspage.h>

namespace QbsProjectManager {
namespace Internal {
class QbsProfilesSettingsWidget;

class QbsProfilesSettingsPage : public Core::IOptionsPage
{
public:
    QbsProfilesSettingsPage();

private:
    QWidget *widget() override;
    void apply() override { }
    void finish() override;

    QbsProfilesSettingsWidget *m_widget = nullptr;
};

} // namespace Internal
} // namespace QbsProjectManager
