/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#pragma once

#include <cpptools/cpptoolstestcase.h>
#include <coreplugin/find/searchresultitem.h>
#include <utils/fileutils.h>

#include <QHash>
#include <QObject>
#include <QStringList>

namespace ProjectExplorer {
class Kit;
class Project;
}
namespace TextEditor { class TextDocument; }

namespace ClangCodeModel {
namespace Internal {
class ClangdClient;
namespace Tests {

class ClangdTest : public QObject
{
    Q_OBJECT
public:
    ~ClangdTest();

protected:
    // Convention: base bame == name of parent dir
    void setProjectFileName(const QString &fileName) { m_projectFileName = fileName; }

    void setSourceFileNames(const QStringList &fileNames) { m_sourceFileNames = fileNames; }
    void setMinimumVersion(int version) { m_minVersion = version; }

    ClangdClient *client() const { return m_client; }
    Utils::FilePath filePath(const QString &fileName) const;
    TextEditor::TextDocument *document(const QString &fileName) const {
        return m_sourceDocuments.value(fileName);
    }

protected slots:
    virtual void initTestCase();

private:
    CppTools::Tests::TemporaryCopiedDir *m_projectDir = nullptr;
    QString m_projectFileName;
    QStringList m_sourceFileNames;
    QHash<QString, TextEditor::TextDocument *> m_sourceDocuments;
    ProjectExplorer::Kit *m_kit = nullptr;
    ProjectExplorer::Project *m_project = nullptr;
    ClangdClient *m_client = nullptr;
    int m_minVersion = -1;
};

class ClangdTestFindReferences : public ClangdTest
{
    Q_OBJECT
public:
    ClangdTestFindReferences();

private slots:
    void initTestCase() override;
    void init() { m_actualResults.clear(); }
    void test_data();
    void test();

private:
    QList<Core::SearchResultItem> m_actualResults;
};

class ClangdTestFollowSymbol : public ClangdTest
{
    Q_OBJECT
public:
    ClangdTestFollowSymbol();

private slots:
    void test_data();
    void test();
};

} // namespace Tests
} // namespace Internal
} // namespace ClangCodeModel

