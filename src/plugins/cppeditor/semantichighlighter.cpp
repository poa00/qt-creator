// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "semantichighlighter.h"

#include <texteditor/fontsettings.h>
#include <texteditor/semantichighlighter.h>
#include <texteditor/syntaxhighlighter.h>
#include <texteditor/textdocument.h>
#include <texteditor/textdocumentlayout.h>

#include <utils/algorithm.h>
#include <utils/qtcassert.h>

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QTextDocument>

using namespace TextEditor;

using SemanticHighlighter::incrementalApplyExtraAdditionalFormats;
using SemanticHighlighter::clearExtraAdditionalFormatsUntilEnd;

static Q_LOGGING_CATEGORY(log, "qtc.cppeditor.semantichighlighter", QtWarningMsg)

namespace CppEditor {

static Utils::Id parenSource() { return "CppEditor"; }

static const QList<std::pair<HighlightingResult, QTextBlock>>
splitRawStringLiteral(const HighlightingResult &result, const QTextBlock &startBlock)
{
    if (result.textStyles.mainStyle != C_STRING)
        return {{result, startBlock}};

    QTextCursor cursor(startBlock);
    cursor.setPosition(cursor.position() + result.column - 1);
    cursor.setPosition(cursor.position() + result.length, QTextCursor::KeepAnchor);
    const QString theString = cursor.selectedText();

    // Find all the components of a raw string literal. If we don't succeed, then it's
    // something else.
    if (!theString.endsWith('"'))
        return {{result, startBlock}};
    int rOffset = -1;
    if (theString.startsWith("R\"")) {
        rOffset = 0;
    } else if (theString.startsWith("LR\"")
               || theString.startsWith("uR\"")
               || theString.startsWith("UR\"")) {
        rOffset = 1;
    } else if (theString.startsWith("u8R\"")) {
        rOffset = 2;
    }
    if (rOffset == -1)
        return {{result, startBlock}};
    const int delimiterOffset = rOffset + 2;
    const int openParenOffset = theString.indexOf('(', delimiterOffset);
    if (openParenOffset == -1)
        return {{result, startBlock}};
    const QStringView delimiter = theString.mid(delimiterOffset, openParenOffset - delimiterOffset);
    const int endDelimiterOffset = theString.length() - 1 - delimiter.length();
    if (theString.mid(endDelimiterOffset, delimiter.length()) != delimiter)
        return {{result, startBlock}};
    if (theString.at(endDelimiterOffset - 1) != ')')
        return {{result, startBlock}};

    // Now split the result. For clarity, we display only the actual content as a string,
    // and the rest (including the delimiter) as a keyword.
    HighlightingResult prefix = result;
    prefix.textStyles.mainStyle = C_KEYWORD;
    prefix.textStyles.mixinStyles = {};
    prefix.length = delimiterOffset + delimiter.length() + 1;
    cursor.setPosition(startBlock.position() + result.column - 1 + prefix.length);
    QTextBlock stringBlock = cursor.block();
    HighlightingResult actualString = result;
    actualString.line = stringBlock.blockNumber() + 1;
    actualString.column = cursor.positionInBlock() + 1;
    actualString.length = endDelimiterOffset - openParenOffset - 2;
    cursor.setPosition(cursor.position() + actualString.length);
    QTextBlock suffixBlock = cursor.block();
    HighlightingResult suffix = result;
    suffix.textStyles.mainStyle = C_KEYWORD;
    suffix.textStyles.mixinStyles = {};
    suffix.line = suffixBlock.blockNumber() + 1;
    suffix.column = cursor.positionInBlock() + 1;
    suffix.length = delimiter.length() + 2;
    QTC_CHECK(prefix.length + actualString.length + suffix.length == result.length);

    return {{prefix, startBlock}, {actualString, stringBlock}, {suffix, suffixBlock}};
}

SemanticHighlighter::SemanticHighlighter(TextDocument *baseTextDocument)
    : QObject(baseTextDocument)
    , m_baseTextDocument(baseTextDocument)
{
    QTC_CHECK(m_baseTextDocument);
    updateFormatMapFromFontSettings();
}

SemanticHighlighter::~SemanticHighlighter()
{
    if (m_watcher) {
        disconnectWatcher();
        m_watcher->cancel();
        m_watcher->waitForFinished();
    }
}

void SemanticHighlighter::setHighlightingRunner(HighlightingRunner highlightingRunner)
{
    m_highlightingRunner = highlightingRunner;
}

void SemanticHighlighter::run()
{
    QTC_ASSERT(m_highlightingRunner, return);

    qCDebug(log) << "SemanticHighlighter: run()";

    if (m_watcher) {
        disconnectWatcher();
        m_watcher->cancel();
    }
    m_watcher.reset(new QFutureWatcher<HighlightingResult>);
    connectWatcher();

    m_revision = documentRevision();
    qCDebug(log) << "starting runner for document revision" << m_revision;
    m_watcher->setFuture(m_highlightingRunner());
}

static Parentheses getClearedParentheses(const QTextBlock &block)
{
    return Utils::filtered(TextDocumentLayout::parentheses(block), [](const Parenthesis &p) {
        return p.source != parenSource();
    });
}

void SemanticHighlighter::onHighlighterResultAvailable(int from, int to)
{
    qCDebug(log) << "onHighlighterResultAvailable()" << from << to;
    if (documentRevision() != m_revision) {
        qCDebug(log) << "ignoring results: revision changed from" << m_revision << "to"
                     << documentRevision();
        return;
    }
    if (!m_watcher || m_watcher->isCanceled()) {
        qCDebug(log) << "ignoring results: future was canceled";
        return;
    }

    QElapsedTimer t;
    t.start();

    SyntaxHighlighter *highlighter = m_baseTextDocument->syntaxHighlighter();
    QTC_ASSERT(highlighter, return);
    incrementalApplyExtraAdditionalFormats(highlighter, m_watcher->future(), from, to, m_formatMap,
                                           &splitRawStringLiteral);

    // In addition to the paren matching that the syntactic highlighter does
    // (parentheses, braces, brackets, comments), here we inject info from the code model
    // for angle brackets in templates and the ternary operator.
    QPair<QTextBlock, Parentheses> parentheses;
    for (int i = from; i < to; ++i) {
        const HighlightingResult &result = m_watcher->future().resultAt(i);
        QTC_ASSERT(result.line <= m_baseTextDocument->document()->blockCount(), continue);
        if (result.kind != AngleBracketOpen && result.kind != AngleBracketClose
                && result.kind != DoubleAngleBracketClose
                && result.kind != TernaryIf && result.kind != TernaryElse) {
            const QTextBlock firstBlockForResult =
                    m_baseTextDocument->document()->findBlockByNumber(result.line - 1);
            const int startRange = firstBlockForResult.position() + result.column - 1;
            const int endRange = startRange + result.length;
            const QTextBlock lastBlockForResult = m_baseTextDocument->document()
                    ->findBlock(endRange);
            const QTextBlock endBlock = lastBlockForResult.next();
            for (QTextBlock block = firstBlockForResult;
                 block.isValid() && block != endBlock;
                 block = block.next()) {
                Parentheses syntacticParens = getClearedParentheses(block);

                // Remove mis-detected parentheses inserted by syntactic highlighter.
                // This typically happens with raw string literals.
                if (result.textStyles.mainStyle != C_PUNCTUATION) {
                    for (auto it = syntacticParens.begin(); it != syntacticParens.end();) {
                        const int absParenPos = block.position() + it->pos;
                        if (absParenPos >= startRange && absParenPos < endRange)
                            it = syntacticParens.erase(it);
                        else
                            ++it;
                    }
                }
                TextDocumentLayout::setParentheses(block, syntacticParens);
            }
            continue;
        }
        if (parentheses.first.isValid() && result.line - 1 > parentheses.first.blockNumber()) {
            TextDocumentLayout::setParentheses(parentheses.first, parentheses.second);
            parentheses = {};
        }
        if (!parentheses.first.isValid()) {
            parentheses.first = m_baseTextDocument->document()->findBlockByNumber(result.line - 1);
            parentheses.second = getClearedParentheses(parentheses.first);
        }
        Parenthesis paren;
        if (result.kind == AngleBracketOpen) {
            paren = {Parenthesis::Opened, '<', result.column - 1};
        } else if (result.kind == AngleBracketClose) {
            paren = {Parenthesis::Closed, '>', result.column - 1};
        } else if (result.kind == DoubleAngleBracketClose) {
            Parenthesis extraParen = {Parenthesis::Closed, '>', result.column - 1};
            extraParen.source = parenSource();
            insertSorted(parentheses.second, extraParen);
            paren = {Parenthesis::Closed, '>', result.column};
        } else if (result.kind == TernaryIf) {
            paren = {Parenthesis::Opened, '?', result.column - 1};
        } else if (result.kind == TernaryElse) {
            paren = {Parenthesis::Closed, ':', result.column - 1};
        }
        QTC_ASSERT(paren.pos != -1, continue);
        paren.source = parenSource();
        insertSorted(parentheses.second, paren);
    }
    if (parentheses.first.isValid())
        TextDocumentLayout::setParentheses(parentheses.first, parentheses.second);

    qCDebug(log) << "onHighlighterResultAvailable() took" << t.elapsed() << "ms";
}

void SemanticHighlighter::onHighlighterFinished()
{
    QTC_ASSERT(m_watcher, return);

    QElapsedTimer t;
    t.start();

    if (!m_watcher->isCanceled() && documentRevision() == m_revision) {
        SyntaxHighlighter *highlighter = m_baseTextDocument->syntaxHighlighter();
        if (QTC_GUARD(highlighter)) {
            qCDebug(log) << "onHighlighterFinished() - clearing formats";
            clearExtraAdditionalFormatsUntilEnd(highlighter, m_watcher->future());
        }
    }

    // Clear out previous "semantic parentheses".
    QTextBlock firstResultBlock;
    QTextBlock lastResultBlock;
    if (m_watcher->future().resultCount() == 0) {
        firstResultBlock = lastResultBlock = m_baseTextDocument->document()->lastBlock();
    } else {
        firstResultBlock = m_baseTextDocument->document()->findBlockByNumber(
                    m_watcher->resultAt(0).line - 1);
        const HighlightingResult &lastResult
                = m_watcher->future().resultAt(m_watcher->future().resultCount() - 1);
        const QTextBlock lastResultStartBlock
                = m_baseTextDocument->document()->findBlockByNumber(lastResult.line - 1);
        lastResultBlock = m_baseTextDocument->document()->findBlock(
                    lastResultStartBlock.position() + lastResult.column - 1 + lastResult.length);
    }

    for (QTextBlock currentBlock = m_baseTextDocument->document()->firstBlock();
         currentBlock != firstResultBlock; currentBlock = currentBlock.next()) {
        TextDocumentLayout::setParentheses(currentBlock, getClearedParentheses(currentBlock));
    }
    for (QTextBlock currentBlock = lastResultBlock.next(); currentBlock.isValid();
         currentBlock = currentBlock.next()) {
        TextDocumentLayout::setParentheses(currentBlock, getClearedParentheses(currentBlock));
    }

    m_watcher.reset();
    qCDebug(log) << "onHighlighterFinished() took" << t.elapsed() << "ms";
}

void SemanticHighlighter::connectWatcher()
{
    using Watcher = QFutureWatcher<HighlightingResult>;
    connect(m_watcher.data(), &Watcher::resultsReadyAt,
            this, &SemanticHighlighter::onHighlighterResultAvailable);
    connect(m_watcher.data(), &Watcher::finished,
            this, &SemanticHighlighter::onHighlighterFinished);
}

void SemanticHighlighter::disconnectWatcher()
{
    using Watcher = QFutureWatcher<HighlightingResult>;
    disconnect(m_watcher.data(), &Watcher::resultsReadyAt,
               this, &SemanticHighlighter::onHighlighterResultAvailable);
    disconnect(m_watcher.data(), &Watcher::finished,
               this, &SemanticHighlighter::onHighlighterFinished);
}

unsigned SemanticHighlighter::documentRevision() const
{
    return m_baseTextDocument->document()->revision();
}

void SemanticHighlighter::updateFormatMapFromFontSettings()
{
    QTC_ASSERT(m_baseTextDocument, return);

    const FontSettings &fs = m_baseTextDocument->fontSettings();

    m_formatMap[TypeUse] = fs.toTextCharFormat(C_TYPE);
    m_formatMap[NamespaceUse] = fs.toTextCharFormat(C_NAMESPACE);
    m_formatMap[LocalUse] = fs.toTextCharFormat(C_LOCAL);
    m_formatMap[FieldUse] = fs.toTextCharFormat(C_FIELD);
    m_formatMap[EnumerationUse] = fs.toTextCharFormat(C_ENUMERATION);
    m_formatMap[VirtualMethodUse] = fs.toTextCharFormat(C_VIRTUAL_METHOD);
    m_formatMap[LabelUse] = fs.toTextCharFormat(C_LABEL);
    m_formatMap[MacroUse] = fs.toTextCharFormat(C_MACRO);
    m_formatMap[FunctionUse] = fs.toTextCharFormat(C_FUNCTION);
    m_formatMap[FunctionDeclarationUse] =
            fs.toTextCharFormat(TextStyles::mixinStyle(C_FUNCTION, C_DECLARATION));
    m_formatMap[VirtualFunctionDeclarationUse] =
            fs.toTextCharFormat(TextStyles::mixinStyle(C_VIRTUAL_METHOD, C_DECLARATION));
    m_formatMap[PseudoKeywordUse] = fs.toTextCharFormat(C_KEYWORD);
    m_formatMap[StaticFieldUse]
            = fs.toTextCharFormat(TextStyles::mixinStyle(C_FIELD, C_STATIC_MEMBER));
    m_formatMap[StaticMethodUse]
            = fs.toTextCharFormat(TextStyles::mixinStyle(C_FUNCTION, C_STATIC_MEMBER));
    m_formatMap[StaticMethodDeclarationUse] = fs.toTextCharFormat(
                TextStyles::mixinStyle(C_FUNCTION, {C_DECLARATION, C_STATIC_MEMBER}));
}

} // namespace CppEditor
