// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "indenter.h"

#include <texteditor/texteditor_global.h>
#include <utils/changeset.h>
#include <utils/fileutils.h>
#include <utils/textfileformat.h>

#include <QList>
#include <QPair>
#include <QSharedPointer>
#include <QString>

QT_BEGIN_NAMESPACE
class QTextDocument;
QT_END_NAMESPACE

namespace TextEditor {
class TextDocument;
class TextEditorWidget;
class RefactoringFile;
using RefactoringFilePtr = QSharedPointer<RefactoringFile>;
class RefactoringFileFactory;
using RefactoringSelections = QVector<QPair<QTextCursor, QTextCursor>>;

// ### listen to the m_editor::destroyed signal?
class TEXTEDITOR_EXPORT RefactoringFile
{
    Q_DISABLE_COPY(RefactoringFile)
public:
    using Range = Utils::ChangeSet::Range;

    virtual ~RefactoringFile();

    bool isValid() const;

    const QTextDocument *document() const;
    // mustn't use the cursor to change the document
    const QTextCursor cursor() const;
    Utils::FilePath filePath() const;
    TextEditorWidget *editor() const;

    // converts 1-based line and column into 0-based source offset
    int position(int line, int column) const;
    // converts 0-based source offset into 1-based line and column
    void lineAndColumn(int offset, int *line, int *column) const;

    QChar charAt(int pos) const;
    QString textOf(int start, int end) const;
    QString textOf(const Range &range) const;

    Utils::ChangeSet changeSet() const;
    void setChangeSet(const Utils::ChangeSet &changeSet);
    void appendIndentRange(const Range &range);
    void appendReindentRange(const Range &range);
    void setOpenEditor(bool activate = false, int pos = -1);
    bool apply();
    bool create(const QString &contents, bool reindent, bool openInEditor);

protected:
    // users may only get const access to RefactoringFiles created through
    // this constructor, because it can't be used to apply changes
    RefactoringFile(QTextDocument *document, const Utils::FilePath &filePath);

    RefactoringFile(TextEditorWidget *editor);
    RefactoringFile(const Utils::FilePath &filePath);

    QTextDocument *mutableDocument() const;

    // derived classes may want to clear language specific extra data
    virtual void fileChanged() {}

    enum IndentType {Indent, Reindent};
    void indentOrReindent(const RefactoringSelections &ranges, IndentType indent);

    void setupFormattingRanges(const QList<Utils::ChangeSet::EditOp> &replaceList);
    void doFormatting();

    TextEditorWidget *openEditor(bool activate, int line, int column);
    static RefactoringSelections rangesToSelections(QTextDocument *document,
                                                    const QList<Range> &ranges);

    virtual void indentSelection(const QTextCursor &selection,
                                 const TextDocument *textDocument) const;
    virtual void reindentSelection(const QTextCursor &selection,
                                   const TextDocument *textDocument) const;

    Utils::FilePath m_filePath;
    mutable Utils::TextFileFormat m_textFileFormat;
    mutable QTextDocument *m_document = nullptr;
    TextEditorWidget *m_editor = nullptr;
    Utils::ChangeSet m_changes;
    QList<Range> m_indentRanges;
    QList<Range> m_reindentRanges;
    QList<QTextCursor> m_formattingCursors;
    bool m_openEditor = false;
    bool m_activateEditor = false;
    int m_editorCursorPosition = -1;
    bool m_appliedOnce = false;
    bool m_formattingEnabled = false;

    friend class RefactoringFileFactory; // access to constructor
};

class TEXTEDITOR_EXPORT RefactoringFileFactory
{
public:
    virtual ~RefactoringFileFactory();

    // TODO: Make pure virtual and introduce dedicated subclass for generic refactoring,
    //       so no one instantiates this one by mistake.
    virtual RefactoringFilePtr file(const Utils::FilePath &filePath) const;
};

} // namespace TextEditor
