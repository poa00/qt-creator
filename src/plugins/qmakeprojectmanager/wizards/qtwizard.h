// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "qtprojectparameters.h"
#include <projectexplorer/baseprojectwizarddialog.h>
#include <projectexplorer/customwizard/customwizard.h>

namespace ProjectExplorer {
class Kit;
class TargetSetupPage;
} // namespace ProjectExplorer

namespace QmakeProjectManager {

class QmakeProject;

namespace Internal {

/* Base class for wizard creating Qt projects using QtProjectParameters.
 * To implement a project wizard, overwrite:
 * - createWizardDialog() to create up the dialog
 * - generateFiles() to set their contents
 * The base implementation provides the wizard parameters and opens
 * the finished project in postGenerateFiles().
 * The pro-file must be the last one of the generated files. */

class QtWizard : public Core::BaseFileWizardFactory
{
    Q_OBJECT

protected:
    QtWizard();

public:
    static QString templateDir();

    static QString sourceSuffix();
    static QString headerSuffix();
    static QString formSuffix();
    static QString profileSuffix();

    // Query CppEditor settings for the class wizard settings
    static bool lowerCaseFiles();

    static bool qt4ProjectPostGenerateFiles(const QWizard *w, const Core::GeneratedFiles &l, QString *errorMessage);

private:
    bool postGenerateFiles(const QWizard *w, const Core::GeneratedFiles &l,
                           QString *errorMessage) const override;
};

// A custom wizard with an additional Qt 4 target page
class CustomQmakeProjectWizard : public ProjectExplorer::CustomProjectWizard
{
    Q_OBJECT

public:
    CustomQmakeProjectWizard();

private:
    Core::BaseFileWizard *create(QWidget *parent,
                                 const Core::WizardDialogParameters &parameters) const override;
    bool postGenerateFiles(const QWizard *, const Core::GeneratedFiles &l,
                           QString *errorMessage) const override;

private:
    enum { targetPageId = 1 };
};

/* BaseQmakeProjectWizardDialog: Additionally offers modules page
 * and getter/setter for blank-delimited modules list, transparently
 * handling the visibility of the modules page list as well as a page
 * to select targets and Qt versions.
 */

class BaseQmakeProjectWizardDialog : public ProjectExplorer::BaseProjectWizardDialog
{
    Q_OBJECT
protected:
    explicit BaseQmakeProjectWizardDialog(const Core::BaseFileWizardFactory *factory,
                                          Utils::ProjectIntroPage *introPage,
                                          int introId, QWidget *parent,
                                          const Core::WizardDialogParameters &parameters);
public:
    explicit BaseQmakeProjectWizardDialog(const Core::BaseFileWizardFactory *factory,
                                          QWidget *parent,
                                          const Core::WizardDialogParameters &parameters);
    ~BaseQmakeProjectWizardDialog() override;

    int addTargetSetupPage(int id = -1);

    bool writeUserFile(const Utils::FilePath &proFile) const;
    QList<Utils::Id> selectedKits() const;

private:
    void generateProfileName(const QString &name, const Utils::FilePath &path);

    ProjectExplorer::TargetSetupPage *m_targetSetupPage = nullptr;
    QList<Utils::Id> m_profileIds;
};

} // namespace Internal
} // namespace QmakeProjectManager
