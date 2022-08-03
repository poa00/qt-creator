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

#include "clangdclient.h"

#include "clangconstants.h"
#include "clangdast.h"
#include "clangdcompletion.h"
#include "clangdfindreferences.h"
#include "clangdfollowsymbol.h"
#include "clangdlocatorfilters.h"
#include "clangdquickfixes.h"
#include "clangdswitchdecldef.h"
#include "clangtextmark.h"
#include "clangutils.h"
#include "clangdsemantichighlighting.h"
#include "tasktimers.h"

#include <coreplugin/editormanager/editormanager.h>
#include <cplusplus/AST.h>
#include <cplusplus/ASTPath.h>
#include <cplusplus/Icons.h>
#include <cppeditor/cppcodemodelsettings.h>
#include <cppeditor/cppeditorwidget.h>
#include <cppeditor/cppmodelmanager.h>
#include <cppeditor/cpprefactoringchanges.h>
#include <cppeditor/cpptoolsreuse.h>
#include <cppeditor/cppvirtualfunctionassistprovider.h>
#include <cppeditor/cppvirtualfunctionproposalitem.h>
#include <cppeditor/semantichighlighter.h>
#include <cppeditor/cppsemanticinfo.h>
#include <languageclient/diagnosticmanager.h>
#include <languageclient/languageclienthoverhandler.h>
#include <languageclient/languageclientinterface.h>
#include <languageclient/languageclientmanager.h>
#include <languageclient/languageclientsymbolsupport.h>
#include <languageclient/languageclientutils.h>
#include <languageserverprotocol/clientcapabilities.h>
#include <languageserverprotocol/progresssupport.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projecttree.h>
#include <projectexplorer/taskhub.h>
#include <texteditor/codeassist/assistinterface.h>
#include <texteditor/codeassist/iassistprocessor.h>
#include <texteditor/codeassist/iassistprovider.h>
#include <texteditor/codeassist/textdocumentmanipulatorinterface.h>
#include <texteditor/texteditor.h>
#include <utils/algorithm.h>
#include <utils/fileutils.h>
#include <utils/itemviews.h>
#include <utils/runextensions.h>
#include <utils/treemodel.h>
#include <utils/utilsicons.h>

#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHeaderView>
#include <QMenu>
#include <QPair>
#include <QPointer>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <set>
#include <unordered_map>
#include <utility>

using namespace CPlusPlus;
using namespace Core;
using namespace LanguageClient;
using namespace LanguageServerProtocol;
using namespace ProjectExplorer;
using namespace TextEditor;

namespace ClangCodeModel {
namespace Internal {

Q_LOGGING_CATEGORY(clangdLog, "qtc.clangcodemodel.clangd", QtWarningMsg);
Q_LOGGING_CATEGORY(clangdLogAst, "qtc.clangcodemodel.clangd.ast", QtWarningMsg);
static Q_LOGGING_CATEGORY(clangdLogServer, "qtc.clangcodemodel.clangd.server", QtWarningMsg);
static QString indexingToken() { return "backgroundIndexProgress"; }

class SymbolDetails : public JsonObject
{
public:
    using JsonObject::JsonObject;

    static constexpr char usrKey[] = "usr";

    // the unqualified name of the symbol
    QString name() const { return typedValue<QString>(nameKey); }

    // the enclosing namespace, class etc (without trailing ::)
    // [NOTE: This is not true, the trailing colons are included]
    QString containerName() const { return typedValue<QString>(containerNameKey); }

    // the clang-specific “unified symbol resolution” identifier
    QString usr() const { return typedValue<QString>(usrKey); }

    // the clangd-specific opaque symbol ID
    Utils::optional<QString> id() const { return optionalValue<QString>(idKey); }

    bool isValid() const override
    {
        return contains(nameKey) && contains(containerNameKey) && contains(usrKey);
    }
};

class SymbolInfoRequest : public Request<LanguageClientArray<SymbolDetails>, std::nullptr_t, TextDocumentPositionParams>
{
public:
    using Request::Request;
    explicit SymbolInfoRequest(const TextDocumentPositionParams &params)
        : Request("textDocument/symbolInfo", params) {}
};

void setupClangdConfigFile()
{
    const Utils::FilePath targetConfigFile = CppEditor::ClangdSettings::clangdUserConfigFilePath();
    const Utils::FilePath baseDir = targetConfigFile.parentDir();
    baseDir.ensureWritableDir();
    Utils::FileReader configReader;
    const QByteArray firstLine = "# This file was generated by Qt Creator and will be overwritten "
                                 "unless you remove this line.";
    if (!configReader.fetch(targetConfigFile) || configReader.data().startsWith(firstLine)) {
        Utils::FileSaver saver(targetConfigFile);
        saver.write(firstLine + '\n');
        saver.write("Hover:\n");
        saver.write("  ShowAKA: Yes\n");
        saver.write("Diagnostics:\n");
        saver.write("  UnusedIncludes: Strict\n");
        QTC_CHECK(saver.finalize());
    }
}

static BaseClientInterface *clientInterface(Project *project, const Utils::FilePath &jsonDbDir)
{
    QString indexingOption = "--background-index";
    const CppEditor::ClangdSettings settings(CppEditor::ClangdProjectSettings(project).settings());
    if (!settings.indexingEnabled() || jsonDbDir.isEmpty())
        indexingOption += "=0";
    const QString headerInsertionOption = QString("--header-insertion=")
            + (settings.autoIncludeHeaders() ? "iwyu" : "never");
#ifdef WITH_TESTS
    // For the #include < test, which needs to get a local header file, but the list
    // is being flooded with system include headers. 4280 on Windows!
    const QString limitResults = QString("--limit-results=0");
#else
    const QString limitResults = QString("--limit-results=%1").arg(settings.completionResults());
#endif
    Utils::CommandLine cmd{settings.clangdFilePath(),
                           {indexingOption,
                            headerInsertionOption,
                            limitResults,
                            "--limit-references=0",
                            "--clang-tidy=0"}};
    if (settings.workerThreadLimit() != 0)
        cmd.addArg("-j=" + QString::number(settings.workerThreadLimit()));
    if (!jsonDbDir.isEmpty())
        cmd.addArg("--compile-commands-dir=" + jsonDbDir.toString());
    if (clangdLogServer().isDebugEnabled())
        cmd.addArgs({"--log=verbose", "--pretty"});
    cmd.addArg("--use-dirty-headers");
    const auto interface = new StdIOClientInterface;
    interface->setCommandLine(cmd);
    return interface;
}

class LocalRefsData {
public:
    LocalRefsData(quint64 id, TextDocument *doc, const QTextCursor &cursor,
                  CppEditor::RenameCallback &&callback)
        : id(id), document(doc), cursor(cursor), callback(std::move(callback)),
          uri(DocumentUri::fromFilePath(doc->filePath())), revision(doc->document()->revision())
    {}

    ~LocalRefsData()
    {
        if (callback)
            callback({}, {}, revision);
    }

    const quint64 id;
    const QPointer<TextDocument> document;
    const QTextCursor cursor;
    CppEditor::RenameCallback callback;
    const DocumentUri uri;
    const int revision;
};

class DiagnosticsCapabilities : public JsonObject
{
public:
    using JsonObject::JsonObject;
    void enableCategorySupport() { insert("categorySupport", true); }
    void enableCodeActionsInline() {insert("codeActionsInline", true);}
};

class ClangdTextDocumentClientCapabilities : public TextDocumentClientCapabilities
{
public:
    using TextDocumentClientCapabilities::TextDocumentClientCapabilities;


    void setPublishDiagnostics(const DiagnosticsCapabilities &caps)
    { insert("publishDiagnostics", caps); }
};

static qint64 getRevision(const TextDocument *doc)
{
    return doc->document()->revision();
}
static qint64 getRevision(const Utils::FilePath &fp)
{
    return fp.lastModified().toMSecsSinceEpoch();
}

template<typename DocType, typename DataType> class VersionedDocData
{
public:
    VersionedDocData(const DocType &doc, const DataType &data) :
        revision(getRevision(doc)), data(data) {}

    const qint64 revision;
    const DataType data;
};

template<typename DocType, typename DataType> class VersionedDataCache
{
public:
    void insert(const DocType &doc, const DataType &data)
    {
        m_data.emplace(std::make_pair(doc, VersionedDocData(doc, data)));
    }
    void remove(const DocType &doc) { m_data.erase(doc); }
    Utils::optional<VersionedDocData<DocType, DataType>> take(const DocType &doc)
    {
        const auto it = m_data.find(doc);
        if (it == m_data.end())
            return {};
        const auto data = it->second;
        m_data.erase(it);
        return data;
    }
    Utils::optional<DataType> get(const DocType &doc)
    {
        const auto it = m_data.find(doc);
        if (it == m_data.end())
            return {};
        if (it->second.revision != getRevision(doc)) {
            m_data.erase(it);
            return {};
        }
        return it->second.data;
    }
private:
    std::unordered_map<DocType, VersionedDocData<DocType, DataType>> m_data;
};

class MemoryTreeModel;
class MemoryUsageWidget : public QWidget
{
    Q_DECLARE_TR_FUNCTIONS(MemoryUsageWidget)
public:
    MemoryUsageWidget(ClangdClient *client);
    ~MemoryUsageWidget();

private:
    void setupUi();
    void getMemoryTree();

    ClangdClient * const m_client;
    MemoryTreeModel * const m_model;
    Utils::TreeView m_view;
    Utils::optional<MessageId> m_currentRequest;
};

class HighlightingData
{
public:
    // For all QPairs, the int member is the corresponding document version.
    QPair<QList<ExpandedSemanticToken>, int> previousTokens;

    // The ranges of symbols referring to virtual functions,
    // as extracted by the highlighting procedure.
    QPair<QList<Range>, int> virtualRanges;

    // The highlighter is owned by its document.
    CppEditor::SemanticHighlighter *highlighter = nullptr;
};

class ClangdClient::Private
{
public:
    Private(ClangdClient *q, Project *project)
        : q(q), settings(CppEditor::ClangdProjectSettings(project).settings()) {}

    void findUsages(TextDocument *document, const QTextCursor &cursor,
                    const QString &searchTerm, const Utils::optional<QString> &replacement,
                    bool categorize);

    void handleDeclDefSwitchReplies();

    static CppEditor::CppEditorWidget *widgetFromDocument(const TextDocument *doc);
    QString searchTermFromCursor(const QTextCursor &cursor) const;
    QTextCursor adjustedCursor(const QTextCursor &cursor, const TextDocument *doc);

    void setHelpItemForTooltip(const MessageId &token, const QString &fqn = {},
                               HelpItem::Category category = HelpItem::Unknown,
                               const QString &type = {});

    void handleSemanticTokens(TextDocument *doc, const QList<ExpandedSemanticToken> &tokens,
                              int version, bool force);

    MessageId getAndHandleAst(const TextDocOrFile &doc, const AstHandler &astHandler,
                              AstCallbackMode callbackMode, const Range &range = {});

    ClangdClient * const q;
    const CppEditor::ClangdSettings::Data settings;
    ClangdFollowSymbol *followSymbol = nullptr;
    ClangdSwitchDeclDef *switchDeclDef = nullptr;
    Utils::optional<LocalRefsData> localRefsData;
    Utils::optional<QVersionNumber> versionNumber;

    QHash<TextDocument *, HighlightingData> highlightingData;
    QHash<Utils::FilePath, CppEditor::BaseEditorDocumentParser::Configuration> parserConfigs;
    QHash<Utils::FilePath, Tasks> issuePaneEntries;

    VersionedDataCache<const TextDocument *, ClangdAstNode> astCache;
    VersionedDataCache<Utils::FilePath, ClangdAstNode> externalAstCache;
    TaskTimer highlightingTimer{"highlighting"};
    quint64 nextJobId = 0;
    bool isFullyIndexed = false;
    bool isTesting = false;
};

static void addToCompilationDb(QJsonObject &cdb,
                               const CppEditor::ProjectPart &projectPart,
                               CppEditor::UsePrecompiledHeaders usePch,
                               const QJsonArray &projectPartOptions,
                               const Utils::FilePath &workingDir,
                               const CppEditor::ProjectFile &sourceFile,
                               bool clStyle)
{
    QJsonArray args = clangOptionsForFile(sourceFile, projectPart, projectPartOptions, usePch,
                                          clStyle);

    // TODO: clangd seems to apply some heuristics depending on what we put here.
    //       Should we make use of them or keep using our own?
    args.prepend("clang");

    const QString fileString = Utils::FilePath::fromString(sourceFile.path).toUserOutput();
    args.append(fileString);
    QJsonObject value;
    value.insert("workingDirectory", workingDir.toString());
    value.insert("compilationCommand", args);
    cdb.insert(fileString, value);
}

static void addCompilationDb(QJsonObject &parentObject, const QJsonObject &cdb)
{
    parentObject.insert("compilationDatabaseChanges", cdb);
}

ClangdClient::ClangdClient(Project *project, const Utils::FilePath &jsonDbDir)
    : Client(clientInterface(project, jsonDbDir)), d(new Private(this, project))
{
    setName(tr("clangd"));
    LanguageFilter langFilter;
    langFilter.mimeTypes = QStringList{"text/x-chdr", "text/x-csrc",
            "text/x-c++hdr", "text/x-c++src", "text/x-objc++src", "text/x-objcsrc"};
    setSupportedLanguage(langFilter);
    setActivateDocumentAutomatically(true);
    setLogTarget(LogTarget::Console);
    setCompletionAssistProvider(new ClangdCompletionAssistProvider(this));
    setQuickFixAssistProvider(new ClangdQuickFixProvider(this));
    if (!project) {
        QJsonObject initOptions;
        const Utils::FilePath includeDir
                = CppEditor::ClangdSettings(d->settings).clangdIncludePath();
        CppEditor::CompilerOptionsBuilder optionsBuilder = clangOptionsBuilder(
                    *CppEditor::CppModelManager::instance()->fallbackProjectPart(),
                    warningsConfigForProject(nullptr), includeDir);
        const CppEditor::UsePrecompiledHeaders usePch = CppEditor::getPchUsage();
        const QJsonArray projectPartOptions = fullProjectPartOptions(
                    optionsBuilder, globalClangOptions());
        const QJsonArray clangOptions = clangOptionsForFile({}, optionsBuilder.projectPart(),
                                                            projectPartOptions, usePch,
                                                            optionsBuilder.isClStyle());
        initOptions.insert("fallbackFlags", clangOptions);
        setInitializationOptions(initOptions);
    }
    auto isRunningClangdClient = [](const LanguageClient::Client *c) {
        return qobject_cast<const ClangdClient *>(c) && c->state() != Client::ShutdownRequested
               && c->state() != Client::Shutdown;
    };
    const QList<Client *> clients =
        Utils::filtered(LanguageClientManager::clientsForProject(project), isRunningClangdClient);
    QTC_CHECK(clients.isEmpty());
    for (const Client *client : clients)
        qCWarning(clangdLog) << client->name() << client->stateString();
    ClientCapabilities caps = Client::defaultClientCapabilities();
    Utils::optional<TextDocumentClientCapabilities> textCaps = caps.textDocument();
    if (textCaps) {
        ClangdTextDocumentClientCapabilities clangdTextCaps(*textCaps);
        clangdTextCaps.clearDocumentHighlight();
        DiagnosticsCapabilities diagnostics;
        diagnostics.enableCategorySupport();
        diagnostics.enableCodeActionsInline();
        clangdTextCaps.setPublishDiagnostics(diagnostics);
        Utils::optional<TextDocumentClientCapabilities::CompletionCapabilities> completionCaps
                = textCaps->completion();
        if (completionCaps)
            clangdTextCaps.setCompletion(ClangdCompletionCapabilities(*completionCaps));
        caps.setTextDocument(clangdTextCaps);
    }
    caps.clearExperimental();
    setClientCapabilities(caps);
    setLocatorsEnabled(false);
    setAutoRequestCodeActions(false); // clangd sends code actions inside diagnostics
    if (project) {
        setProgressTitleForToken(indexingToken(),
                                 tr("Indexing %1 with clangd").arg(project->displayName()));
    }
    setCurrentProject(project);
    setDocumentChangeUpdateThreshold(d->settings.documentUpdateThreshold);
    setSymbolStringifier(displayNameFromDocumentSymbol);
    setSemanticTokensHandler([this](TextDocument *doc, const QList<ExpandedSemanticToken> &tokens,
                                    int version, bool force) {
        d->handleSemanticTokens(doc, tokens, version, force);
    });
    hoverHandler()->setHelpItemProvider([this](const HoverRequest::Response &response,
                                               const DocumentUri &uri) {
        gatherHelpItemForTooltip(response, uri);
    });

    connect(this, &Client::workDone, this,
            [this, p = QPointer(project)](const ProgressToken &token) {
        const QString * const val = Utils::get_if<QString>(&token);
        if (val && *val == indexingToken()) {
            d->isFullyIndexed = true;
            emit indexingFinished();
#ifdef WITH_TESTS
            if (p)
                emit p->indexingFinished("Indexer.Clangd");
#endif
        }
    });

    connect(this, &Client::initialized, this, [this] {
        auto currentDocumentFilter = static_cast<ClangdCurrentDocumentFilter *>(
            CppEditor::CppModelManager::instance()->currentDocumentFilter());
        currentDocumentFilter->updateCurrentClient();
    });

    start();
}

ClangdClient::~ClangdClient()
{
    if (d->followSymbol)
        d->followSymbol->clear();
    delete d;
}

bool ClangdClient::isFullyIndexed() const { return d->isFullyIndexed; }

void ClangdClient::openExtraFile(const Utils::FilePath &filePath, const QString &content)
{
    QFile cxxFile(filePath.toString());
    if (content.isEmpty() && !cxxFile.open(QIODevice::ReadOnly))
        return;
    TextDocumentItem item;
    item.setLanguageId("cpp");
    item.setUri(DocumentUri::fromFilePath(filePath));
    item.setText(!content.isEmpty() ? content : QString::fromUtf8(cxxFile.readAll()));
    item.setVersion(0);
    sendMessage(DidOpenTextDocumentNotification(DidOpenTextDocumentParams(item)),
                SendDocUpdates::Ignore);
}

void ClangdClient::closeExtraFile(const Utils::FilePath &filePath)
{
    sendMessage(DidCloseTextDocumentNotification(DidCloseTextDocumentParams(
            TextDocumentIdentifier{DocumentUri::fromFilePath(filePath)})),
                SendDocUpdates::Ignore);
}

void ClangdClient::findUsages(TextDocument *document, const QTextCursor &cursor,
                              const Utils::optional<QString> &replacement)
{
    // Quick check: Are we even on anything searchable?
    const QTextCursor adjustedCursor = d->adjustedCursor(cursor, document);
    const QString searchTerm = d->searchTermFromCursor(adjustedCursor);
    if (searchTerm.isEmpty())
        return;

    const bool categorize = CppEditor::codeModelSettings()->categorizeFindReferences();

    // If it's a "normal" symbol, go right ahead.
    if (searchTerm != "operator" && Utils::allOf(searchTerm, [](const QChar &c) {
            return c.isLetterOrNumber() || c == '_';
    })) {
        d->findUsages(document, adjustedCursor, searchTerm, replacement, categorize);
        return;
    }

    // Otherwise get the proper spelling of the search term from clang, so we can put it into the
    // search widget.
    const auto symbolInfoHandler = [this, doc = QPointer(document), adjustedCursor, replacement, categorize]
            (const QString &name, const QString &, const MessageId &) {
        if (!doc)
            return;
        if (name.isEmpty())
            return;
        d->findUsages(doc.data(), adjustedCursor, name, replacement, categorize);
    };
    requestSymbolInfo(document->filePath(), Range(adjustedCursor).start(), symbolInfoHandler);
}

void ClangdClient::handleDiagnostics(const PublishDiagnosticsParams &params)
{
    const DocumentUri &uri = params.uri();
    Client::handleDiagnostics(params);
    const int docVersion = documentVersion(uri.toFilePath());
    if (params.version().value_or(docVersion) != docVersion)
        return;
    for (const Diagnostic &diagnostic : params.diagnostics()) {
        const ClangdDiagnostic clangdDiagnostic(diagnostic);
        auto codeActions = clangdDiagnostic.codeActions();
        if (codeActions && !codeActions->isEmpty()) {
            for (CodeAction &action : *codeActions)
                action.setDiagnostics({diagnostic});
            LanguageClient::updateCodeActionRefactoringMarker(this, *codeActions, uri);
        } else {
            // We know that there's only one kind of diagnostic for which clangd has
            // a quickfix tweak, so let's not be wasteful.
            const Diagnostic::Code code = diagnostic.code().value_or(Diagnostic::Code());
            const QString * const codeString = Utils::get_if<QString>(&code);
            if (codeString && *codeString == "-Wswitch")
                requestCodeActions(uri, diagnostic);
        }
    }
}

void ClangdClient::handleDocumentOpened(TextDocument *doc)
{
    const auto data = d->externalAstCache.take(doc->filePath());
    if (!data)
        return;
    if (data->revision == getRevision(doc->filePath()))
       d->astCache.insert(doc, data->data);
}

void ClangdClient::handleDocumentClosed(TextDocument *doc)
{
    d->highlightingData.remove(doc);
    d->astCache.remove(doc);
    d->parserConfigs.remove(doc->filePath());
}

QTextCursor ClangdClient::adjustedCursorForHighlighting(const QTextCursor &cursor,
                                                        TextEditor::TextDocument *doc)
{
    return d->adjustedCursor(cursor, doc);
}

const LanguageClient::Client::CustomInspectorTabs ClangdClient::createCustomInspectorTabs()
{
    return {std::make_pair(new MemoryUsageWidget(this), tr("Memory Usage"))};
}

class ClangdDiagnosticManager : public LanguageClient::DiagnosticManager
{
    using LanguageClient::DiagnosticManager::DiagnosticManager;

    ClangdClient *getClient() const { return qobject_cast<ClangdClient *>(client()); }

    bool isCurrentDocument(const Utils::FilePath &filePath) const
    {
        const IDocument * const doc = EditorManager::currentDocument();
        return doc && doc->filePath() == filePath;
    }

    void showDiagnostics(const DocumentUri &uri, int version) override
    {
        const Utils::FilePath filePath = uri.toFilePath();
        getClient()->clearTasks(filePath);
        DiagnosticManager::showDiagnostics(uri, version);
        if (isCurrentDocument(filePath))
            getClient()->switchIssuePaneEntries(filePath);
    }

    void hideDiagnostics(const Utils::FilePath &filePath) override
    {
        DiagnosticManager::hideDiagnostics(filePath);
        if (isCurrentDocument(filePath))
            TaskHub::clearTasks(Constants::TASK_CATEGORY_DIAGNOSTICS);
    }

    QList<Diagnostic> filteredDiagnostics(const QList<Diagnostic> &diagnostics) const override
    {
        return Utils::filtered(diagnostics, [](const Diagnostic &diag){
            const Diagnostic::Code code = diag.code().value_or(Diagnostic::Code());
            const QString * const codeString = Utils::get_if<QString>(&code);
            return !codeString || *codeString != "drv_unknown_argument";
        });
    }

    TextMark *createTextMark(const Utils::FilePath &filePath,
                             const Diagnostic &diagnostic,
                             bool isProjectFile) const override
    {
        return new ClangdTextMark(filePath, diagnostic, isProjectFile, getClient());
    }
};

DiagnosticManager *ClangdClient::createDiagnosticManager()
{
    auto diagnosticManager = new ClangdDiagnosticManager(this);
    if (d->isTesting) {
        connect(diagnosticManager, &DiagnosticManager::textMarkCreated,
                this, &ClangdClient::textMarkCreated);
    }
    return diagnosticManager;
}

bool ClangdClient::referencesShadowFile(const TextEditor::TextDocument *doc,
                                        const Utils::FilePath &candidate)
{
    const QRegularExpression includeRex("#include.*" + candidate.fileName() + R"([>"])");
    const QTextCursor includePos = doc->document()->find(includeRex);
    return !includePos.isNull();
}

RefactoringChangesData *ClangdClient::createRefactoringChangesBackend() const
{
    return new CppEditor::CppRefactoringChangesData(
                CppEditor::CppModelManager::instance()->snapshot());
}

QVersionNumber ClangdClient::versionNumber() const
{
    if (d->versionNumber)
        return d->versionNumber.value();

    const QRegularExpression versionPattern("^clangd version (\\d+)\\.(\\d+)\\.(\\d+).*$");
    QTC_CHECK(versionPattern.isValid());
    const QRegularExpressionMatch match = versionPattern.match(serverVersion());
    if (match.isValid()) {
        d->versionNumber.emplace({match.captured(1).toInt(), match.captured(2).toInt(),
                                 match.captured(3).toInt()});
    } else {
        qCWarning(clangdLog) << "Failed to parse clangd server string" << serverVersion();
        d->versionNumber.emplace({0});
    }
    return d->versionNumber.value();
}

CppEditor::ClangdSettings::Data ClangdClient::settingsData() const { return d->settings; }

void ClangdClient::Private::findUsages(TextDocument *document,
        const QTextCursor &cursor, const QString &searchTerm,
        const Utils::optional<QString> &replacement, bool categorize)
{
    const auto findRefs = new ClangdFindReferences(q, document, cursor, searchTerm, replacement,
                                                   categorize);
    if (isTesting) {
        connect(findRefs, &ClangdFindReferences::foundReferences,
                q, &ClangdClient::foundReferences);
        connect(findRefs, &ClangdFindReferences::done, q, &ClangdClient::findUsagesDone);
    }
}

void ClangdClient::enableTesting()
{
    d->isTesting = true;
}

bool ClangdClient::testingEnabled() const
{
    return d->isTesting;
}

QString ClangdClient::displayNameFromDocumentSymbol(SymbolKind kind, const QString &name,
                                                    const QString &detail)
{
    switch (kind) {
    case SymbolKind::Constructor:
        return name + detail;
    case SymbolKind::Method:
    case SymbolKind::Function: {
        const int lastParenOffset = detail.lastIndexOf(')');
        if (lastParenOffset == -1)
            return name;
        int leftParensNeeded = 1;
        int i = -1;
        for (i = lastParenOffset - 1; i >= 0; --i) {
            switch (detail.at(i).toLatin1()) {
            case ')':
                ++leftParensNeeded;
                break;
            case '(':
                --leftParensNeeded;
                break;
            default:
                break;
            }
            if (leftParensNeeded == 0)
                break;
        }
        if (leftParensNeeded > 0)
            return name;
        return name + detail.mid(i) + " -> " + detail.left(i);
    }
    case SymbolKind::Variable:
    case SymbolKind::Field:
    case SymbolKind::Constant:
        if (detail.isEmpty())
            return name;
        return name + " -> " + detail;
    default:
        return name;
    }
}

// Force re-parse of all open files that include the changed ui header.
// Otherwise, we potentially have stale diagnostics.
void ClangdClient::handleUiHeaderChange(const QString &fileName)
{
    const QRegularExpression includeRex("#include.*" + fileName + R"([>"])");
    const QList<Client *> &allClients = LanguageClientManager::clients();
    for (Client * const client : allClients) {
        if (!client->reachable() || !qobject_cast<ClangdClient *>(client))
            continue;
        for (IDocument * const doc : DocumentModel::openedDocuments()) {
            const auto textDoc = qobject_cast<TextDocument *>(doc);
            if (!textDoc || !client->documentOpen(textDoc))
                continue;
            const QTextCursor includePos = textDoc->document()->find(includeRex);
            if (includePos.isNull())
                continue;
            qCDebug(clangdLog) << "updating" << textDoc->filePath() << "due to change in UI header"
                               << fileName;
            client->documentContentsChanged(textDoc, 0, 0, 0);
            break; // No sane project includes the same UI header twice.
        }
    }
}

void ClangdClient::updateParserConfig(const Utils::FilePath &filePath,
        const CppEditor::BaseEditorDocumentParser::Configuration &config)
{
    if (config.preferredProjectPartId.isEmpty())
        return;

    CppEditor::BaseEditorDocumentParser::Configuration &cachedConfig = d->parserConfigs[filePath];
    if (cachedConfig == config)
        return;
    cachedConfig = config;

    // TODO: Also handle editorDefines (and usePrecompiledHeaders?)
    const auto projectPart = CppEditor::CppModelManager::instance()
            ->projectPartForId(config.preferredProjectPartId);
    if (!projectPart)
        return;
    QJsonObject cdbChanges;
    const Utils::FilePath includeDir = CppEditor::ClangdSettings(d->settings).clangdIncludePath();
    CppEditor::CompilerOptionsBuilder optionsBuilder = clangOptionsBuilder(
                *projectPart, warningsConfigForProject(project()), includeDir);
    const CppEditor::ProjectFile file(filePath.toString(),
                                      CppEditor::ProjectFile::classify(filePath.toString()));
    const QJsonArray projectPartOptions = fullProjectPartOptions(
                optionsBuilder, globalClangOptions());
    addToCompilationDb(cdbChanges, *projectPart, CppEditor::getPchUsage(), projectPartOptions,
                       filePath.parentDir(), file, optionsBuilder.isClStyle());
    QJsonObject settings;
    addCompilationDb(settings, cdbChanges);
    DidChangeConfigurationParams configChangeParams;
    configChangeParams.setSettings(settings);
    sendMessage(DidChangeConfigurationNotification(configChangeParams));
}

void ClangdClient::switchIssuePaneEntries(const Utils::FilePath &filePath)
{
    TaskHub::clearTasks(Constants::TASK_CATEGORY_DIAGNOSTICS);
    const Tasks tasks = d->issuePaneEntries.value(filePath);
    for (const Task &t : tasks)
        TaskHub::addTask(t);
}

void ClangdClient::addTask(const ProjectExplorer::Task &task)
{
    d->issuePaneEntries[task.file] << task;
}

void ClangdClient::clearTasks(const Utils::FilePath &filePath)
{
    d->issuePaneEntries[filePath].clear();
}

Utils::optional<bool> ClangdClient::hasVirtualFunctionAt(TextDocument *doc, int revision,
                                                         const Range &range)
{
    const auto highlightingData = d->highlightingData.constFind(doc);
    if (highlightingData == d->highlightingData.constEnd()
            || highlightingData->virtualRanges.second != revision) {
        return {};
    }
    const auto matcher = [range](const Range &r) { return range.overlaps(r); };
    return Utils::contains(highlightingData->virtualRanges.first, matcher);
}

MessageId ClangdClient::getAndHandleAst(const TextDocOrFile &doc, const AstHandler &astHandler,
                                        AstCallbackMode callbackMode, const Range &range)
{
    return d->getAndHandleAst(doc, astHandler, callbackMode, range);
}

MessageId ClangdClient::requestSymbolInfo(const Utils::FilePath &filePath, const Position &position,
                                          const SymbolInfoHandler &handler)
{
    const TextDocumentIdentifier docId(DocumentUri::fromFilePath(filePath));
    const TextDocumentPositionParams params(docId, position);
    SymbolInfoRequest symReq(params);
    symReq.setResponseCallback([handler, reqId = symReq.id()]
                               (const SymbolInfoRequest::Response &response) {
        const auto result = response.result();
        if (!result) {
            handler({}, {}, reqId);
            return;
        }

        // According to the documentation, we should receive a single
        // object here, but it's a list. No idea what it means if there's
        // more than one entry. We choose the first one.
        const auto list = Utils::get_if<QList<SymbolDetails>>(&result.value());
        if (!list || list->isEmpty()) {
            handler({}, {}, reqId);
            return;
        }

        const SymbolDetails &sd = list->first();
        handler(sd.name(), sd.containerName(), reqId);
    });
    sendMessage(symReq);
    return symReq.id();
}


void ClangdClient::followSymbol(TextDocument *document,
        const QTextCursor &cursor,
        CppEditor::CppEditorWidget *editorWidget,
        const Utils::LinkHandler &callback,
        bool resolveTarget,
        bool openInSplit
        )
{
    QTC_ASSERT(documentOpen(document), openDocument(document));

    delete d->followSymbol;
    d->followSymbol = nullptr;

    const QTextCursor adjustedCursor = d->adjustedCursor(cursor, document);
    if (!resolveTarget) {
        symbolSupport().findLinkAt(document, adjustedCursor, callback, false);
        return;
    }

    qCDebug(clangdLog) << "follow symbol requested" << document->filePath()
                       << adjustedCursor.blockNumber() << adjustedCursor.positionInBlock();
    d->followSymbol = new ClangdFollowSymbol(this, adjustedCursor, editorWidget, document, callback,
                                             openInSplit);
    connect(d->followSymbol, &ClangdFollowSymbol::done, this, [this] {
        d->followSymbol->deleteLater();
        d->followSymbol = nullptr;
    });
}

void ClangdClient::switchDeclDef(TextDocument *document, const QTextCursor &cursor,
                                 CppEditor::CppEditorWidget *editorWidget,
                                 const Utils::LinkHandler &callback)
{
    QTC_ASSERT(documentOpen(document), openDocument(document));

    qCDebug(clangdLog) << "switch decl/dev requested" << document->filePath()
                       << cursor.blockNumber() << cursor.positionInBlock();
    if (d->switchDeclDef)
        delete d->switchDeclDef;
    d->switchDeclDef = new ClangdSwitchDeclDef(this, document, cursor, editorWidget, callback);
    connect(d->switchDeclDef, &ClangdSwitchDeclDef::done, this, [this] {
        d->switchDeclDef->deleteLater();
        d->switchDeclDef = nullptr;
    });
}

void ClangdClient::switchHeaderSource(const Utils::FilePath &filePath, bool inNextSplit)
{
    class SwitchSourceHeaderRequest : public Request<QJsonValue, std::nullptr_t, TextDocumentIdentifier>
    {
    public:
        using Request::Request;
        explicit SwitchSourceHeaderRequest(const Utils::FilePath &filePath)
            : Request("textDocument/switchSourceHeader",
                      TextDocumentIdentifier(DocumentUri::fromFilePath(filePath))) {}
    };
    SwitchSourceHeaderRequest req(filePath);
    req.setResponseCallback([inNextSplit](const SwitchSourceHeaderRequest::Response &response) {
        if (const Utils::optional<QJsonValue> result = response.result()) {
            const DocumentUri uri = DocumentUri::fromProtocol(result->toString());
            const Utils::FilePath filePath = uri.toFilePath();
            if (!filePath.isEmpty())
                CppEditor::openEditor(filePath, inNextSplit);
        }
    });
    sendMessage(req);
}

void ClangdClient::findLocalUsages(TextDocument *document, const QTextCursor &cursor,
        CppEditor::RenameCallback &&callback)
{
    QTC_ASSERT(documentOpen(document), openDocument(document));

    qCDebug(clangdLog) << "local references requested" << document->filePath()
                       << (cursor.blockNumber() + 1) << (cursor.positionInBlock() + 1);

    d->localRefsData.emplace(++d->nextJobId, document, cursor, std::move(callback));
    const QString searchTerm = d->searchTermFromCursor(cursor);
    if (searchTerm.isEmpty()) {
        d->localRefsData.reset();
        return;
    }

    // Step 1: Go to definition
    const auto gotoDefCallback = [this, id = d->localRefsData->id](const Utils::Link &link) {
        qCDebug(clangdLog) << "received go to definition response" << link.targetFilePath
                           << link.targetLine << (link.targetColumn + 1);
        if (!d->localRefsData || id != d->localRefsData->id)
            return;
        if (!link.hasValidTarget()) {
            d->localRefsData.reset();
            return;
        }

        // Step 2: Get AST and check whether it's a local variable.
        const auto astHandler = [this, link, id](const ClangdAstNode &ast, const MessageId &) {
            qCDebug(clangdLog) << "received ast response";
            if (!d->localRefsData || id != d->localRefsData->id)
                return;
            if (!ast.isValid() || !d->localRefsData->document) {
                d->localRefsData.reset();
                return;
            }

            const Position linkPos(link.targetLine - 1, link.targetColumn);
            const ClangdAstPath astPath = getAstPath(ast, linkPos);
            bool isVar = false;
            for (auto it = astPath.rbegin(); it != astPath.rend(); ++it) {
                if (it->role() == "declaration"
                        && (it->kind() == "Function" || it->kind() == "CXXMethod"
                            || it->kind() == "CXXConstructor" || it->kind() == "CXXDestructor"
                            || it->kind() == "Lambda")) {
                    if (!isVar)
                        break;

                    // Step 3: Find references.
                    qCDebug(clangdLog) << "finding references for local var";
                    symbolSupport().findUsages(d->localRefsData->document,
                                               d->localRefsData->cursor,
                                               [this, id](const QList<Location> &locations) {
                        qCDebug(clangdLog) << "found" << locations.size() << "local references";
                        if (!d->localRefsData || id != d->localRefsData->id)
                            return;
                        const Utils::Links links = Utils::transform(locations, &Location::toLink);

                        // The callback only uses the symbol length, so we just create a dummy.
                        // Note that the calculation will be wrong for identifiers with
                        // embedded newlines, but we've never supported that.
                        QString symbol;
                        if (!locations.isEmpty()) {
                            const Range r = locations.first().range();
                            symbol = QString(r.end().character() - r.start().character(), 'x');
                        }
                        d->localRefsData->callback(symbol, links, d->localRefsData->revision);
                        d->localRefsData->callback = {};
                        d->localRefsData.reset();
                    });
                    return;
                }
                if (!isVar && it->role() == "declaration"
                        && (it->kind() == "Var" || it->kind() == "ParmVar")) {
                    isVar = true;
                }
            }
            d->localRefsData.reset();
        };
        qCDebug(clangdLog) << "sending ast request for link";
        d->getAndHandleAst(d->localRefsData->document, astHandler,
                           AstCallbackMode::SyncIfPossible);
    };
    symbolSupport().findLinkAt(document, cursor, std::move(gotoDefCallback), true);
}

void ClangdClient::gatherHelpItemForTooltip(const HoverRequest::Response &hoverResponse,
                                            const DocumentUri &uri)
{
    if (const Utils::optional<HoverResult> result = hoverResponse.result()) {
        if (auto hover = Utils::get_if<Hover>(&(*result))) {
            const HoverContent content = hover->content();
            const MarkupContent *const markup = Utils::get_if<MarkupContent>(&content);
            if (markup) {
                const QString markupString = markup->content();

                // Macros aren't locatable via the AST, so parse the formatted string.
                static const QString magicMacroPrefix = "### macro `";
                if (markupString.startsWith(magicMacroPrefix)) {
                    const int nameStart = magicMacroPrefix.length();
                    const int closingQuoteIndex = markupString.indexOf('`', nameStart);
                    if (closingQuoteIndex != -1) {
                        const QString macroName = markupString.mid(nameStart,
                                                                   closingQuoteIndex - nameStart);
                        d->setHelpItemForTooltip(hoverResponse.id(), macroName, HelpItem::Macro);
                        return;
                    }
                }

                // Is it the file path for an include directive?
                QString cleanString = markupString;
                cleanString.remove('`');
                const QStringList lines = cleanString.trimmed().split('\n');
                if (!lines.isEmpty()) {
                    const auto filePath = Utils::FilePath::fromUserInput(lines.last().simplified());
                    if (filePath.exists()) {
                        d->setHelpItemForTooltip(hoverResponse.id(),
                                                 filePath.fileName(),
                                                 HelpItem::Brief);
                        return;
                    }
                }
            }
        }
    }

    const TextDocument * const doc = documentForFilePath(uri.toFilePath());
    QTC_ASSERT(doc, return);
    const auto astHandler = [this, uri, hoverResponse](const ClangdAstNode &ast, const MessageId &) {
        const MessageId id = hoverResponse.id();
        Range range;
        if (const Utils::optional<HoverResult> result = hoverResponse.result()) {
            if (auto hover = Utils::get_if<Hover>(&(*result)))
                range = hover->range().value_or(Range());
        }
        const ClangdAstPath path = getAstPath(ast, range);
        if (path.isEmpty()) {
            d->setHelpItemForTooltip(id);
            return;
        }
        ClangdAstNode node = path.last();
        if (node.role() == "expression" && node.kind() == "ImplicitCast") {
            const Utils::optional<QList<ClangdAstNode>> children = node.children();
            if (children && !children->isEmpty())
                node = children->first();
        }
        while (node.kind() == "Qualified") {
            const Utils::optional<QList<ClangdAstNode>> children = node.children();
            if (children && !children->isEmpty())
                node = children->first();
        }
        if (clangdLogAst().isDebugEnabled())
            node.print(0);

        QString type = node.type();
        const auto stripTemplatePartOffType = [&type] {
            const int angleBracketIndex = type.indexOf('<');
            if (angleBracketIndex != -1)
                type = type.left(angleBracketIndex);
        };

        const bool isMemberFunction = node.role() == "expression" && node.kind() == "Member"
                && (node.arcanaContains("member function") || type.contains('('));
        const bool isFunction = node.role() == "expression" && node.kind() == "DeclRef"
                && type.contains('(');
        if (isMemberFunction || isFunction) {
            const auto symbolInfoHandler = [this, id, type, isFunction]
                    (const QString &name, const QString &prefix, const MessageId &) {
                qCDebug(clangdLog) << "handling symbol info reply";
                const QString fqn = prefix + name;

                // Unfortunately, the arcana string contains the signature only for
                // free functions, so we can't distinguish member function overloads.
                // But since HtmlDocExtractor::getFunctionDescription() is always called
                // with mainOverload = true, such information would get ignored anyway.
                if (!fqn.isEmpty())
                    d->setHelpItemForTooltip(id, fqn, HelpItem::Function, isFunction ? type : "()");
            };
            requestSymbolInfo(uri.toFilePath(), range.start(), symbolInfoHandler);
            return;
        }
        if ((node.role() == "expression" && node.kind() == "DeclRef")
                || (node.role() == "declaration"
                    && (node.kind() == "Var" || node.kind() == "ParmVar"
                        || node.kind() == "Field"))) {
            if (node.arcanaContains("EnumConstant")) {
                d->setHelpItemForTooltip(id, node.detail().value_or(QString()),
                                         HelpItem::Enum, type);
                return;
            }
            stripTemplatePartOffType();
            type.remove("&").remove("*").remove("const ").remove(" const")
                    .remove("volatile ").remove(" volatile");
            type = type.simplified();
            if (type != "int" && !type.contains(" int")
                    && type != "char" && !type.contains(" char")
                    && type != "double" && !type.contains(" double")
                    && type != "float" && type != "bool") {
                d->setHelpItemForTooltip(id, type, node.qdocCategoryForDeclaration(
                                             HelpItem::ClassOrNamespace));
            } else {
                d->setHelpItemForTooltip(id);
            }
            return;
        }
        if (node.isNamespace()) {
            QString ns = node.detail().value_or(QString());
            for (auto it = path.rbegin() + 1; it != path.rend(); ++it) {
                if (it->isNamespace()) {
                    const QString name = it->detail().value_or(QString());
                    if (!name.isEmpty())
                        ns.prepend("::").prepend(name);
                }
            }
            d->setHelpItemForTooltip(hoverResponse.id(), ns, HelpItem::ClassOrNamespace);
            return;
        }
        if (node.role() == "type") {
            if (node.kind() == "Enum") {
                d->setHelpItemForTooltip(id, node.detail().value_or(QString()), HelpItem::Enum);
            } else if (node.kind() == "Record" || node.kind() == "TemplateSpecialization") {
                stripTemplatePartOffType();
                d->setHelpItemForTooltip(id, type, HelpItem::ClassOrNamespace);
            } else if (node.kind() == "Typedef") {
                d->setHelpItemForTooltip(id, type, HelpItem::Typedef);
            } else {
                d->setHelpItemForTooltip(id);
            }
            return;
        }
        if (node.role() == "expression" && node.kind() == "CXXConstruct") {
            const QString name = node.detail().value_or(QString());
            if (!name.isEmpty())
                type = name;
            d->setHelpItemForTooltip(id, type, HelpItem::ClassOrNamespace);
        }
        if (node.role() == "specifier" && node.kind() == "NamespaceAlias") {
            d->setHelpItemForTooltip(id, node.detail().value_or(QString()).chopped(2),
                                     HelpItem::ClassOrNamespace);
            return;
        }
        d->setHelpItemForTooltip(id);
    };
    d->getAndHandleAst(doc, astHandler, AstCallbackMode::SyncIfPossible);
}

void ClangdClient::setVirtualRanges(const Utils::FilePath &filePath, const QList<Range> &ranges,
                                    int revision)
{
    TextDocument * const doc = documentForFilePath(filePath);
    if (doc && doc->document()->revision() == revision)
        d->highlightingData[doc].virtualRanges = {ranges, revision};
}

CppEditor::CppEditorWidget *ClangdClient::Private::widgetFromDocument(const TextDocument *doc)
{
    IEditor * const editor = Utils::findOrDefault(EditorManager::visibleEditors(),
            [doc](const IEditor *editor) { return editor->document() == doc; });
    return qobject_cast<CppEditor::CppEditorWidget *>(TextEditorWidget::fromEditor(editor));
}

QString ClangdClient::Private::searchTermFromCursor(const QTextCursor &cursor) const
{
    QTextCursor termCursor(cursor);
    termCursor.select(QTextCursor::WordUnderCursor);
    return termCursor.selectedText();
}

// https://github.com/clangd/clangd/issues/936
QTextCursor ClangdClient::Private::adjustedCursor(const QTextCursor &cursor,
                                                  const TextDocument *doc)
{
    CppEditor::CppEditorWidget * const widget = widgetFromDocument(doc);
    if (!widget)
        return cursor;
    const Document::Ptr cppDoc = widget->semanticInfo().doc;
    if (!cppDoc)
        return cursor;
    const QList<AST *> builtinAstPath = ASTPath(cppDoc)(cursor);
    if (builtinAstPath.isEmpty())
        return cursor;
    const TranslationUnit * const tu = cppDoc->translationUnit();
    const auto posForToken = [doc, tu](int tok) {
        int line, column;
        tu->getTokenPosition(tok, &line, &column);
        return Utils::Text::positionInText(doc->document(), line, column);
    };
    const auto endPosForToken = [doc, tu](int tok) {
        int line, column;
        tu->getTokenEndPosition(tok, &line, &column);
        return Utils::Text::positionInText(doc->document(), line, column);
    };
    const auto leftMovedCursor = [cursor] {
        QTextCursor c = cursor;
        c.setPosition(cursor.position() - 1);
        return c;
    };

    // enum E { v1|, v2 };
    if (const EnumeratorAST * const enumAst = builtinAstPath.last()->asEnumerator()) {
        if (endPosForToken(enumAst->identifier_token) == cursor.position())
            return leftMovedCursor();
        return cursor;
    }

    for (auto it = builtinAstPath.rbegin(); it != builtinAstPath.rend(); ++it) {

        // s|.x or s|->x
        if (const MemberAccessAST * const memberAccess = (*it)->asMemberAccess()) {
            switch (tu->tokenAt(memberAccess->access_token).kind()) {
            case T_DOT:
                break;
            case T_ARROW: {
                const Utils::optional<ClangdAstNode> clangdAst = astCache.get(doc);
                if (!clangdAst)
                    return cursor;
                const ClangdAstPath clangdAstPath = getAstPath(*clangdAst, Range(cursor));
                for (auto it = clangdAstPath.rbegin(); it != clangdAstPath.rend(); ++it) {
                    if (it->detailIs("operator->") && it->arcanaContains("CXXMethod"))
                        return cursor;
                }
                break;
            }
            default:
                return cursor;
            }
            if (posForToken(memberAccess->access_token) != cursor.position())
                return cursor;
            return leftMovedCursor();
        }

        // f(arg1|, arg2)
        if (const CallAST *const callAst = (*it)->asCall()) {
            const int tok = builtinAstPath.last()->lastToken();
            if (posForToken(tok) != cursor.position())
                return cursor;
            if (tok == callAst->rparen_token)
                return leftMovedCursor();
            if (tu->tokenKind(tok) != T_COMMA)
                return cursor;

            // Guard against edge case of overloaded comma operator.
            for (auto list = callAst->expression_list; list; list = list->next) {
                if (list->value->lastToken() == tok)
                    return leftMovedCursor();
            }
            return cursor;
        }

        // ~My|Class
        if (const DestructorNameAST * const destrAst = (*it)->asDestructorName()) {
            QTextCursor c = cursor;
            c.setPosition(posForToken(destrAst->tilde_token));
            return c;
        }

        // QVector<QString|>
        if (const TemplateIdAST * const templAst = (*it)->asTemplateId()) {
            if (posForToken(templAst->greater_token) == cursor.position())
                return leftMovedCursor();
            return cursor;
        }
    }
    return cursor;
}

void ClangdClient::Private::setHelpItemForTooltip(const MessageId &token, const QString &fqn,
                                                  HelpItem::Category category,
                                                  const QString &type)
{
    QStringList helpIds;
    QString mark;
    if (!fqn.isEmpty()) {
        helpIds << fqn;
        int sepSearchStart = 0;
        while (true) {
            sepSearchStart = fqn.indexOf("::", sepSearchStart);
            if (sepSearchStart == -1)
                break;
            sepSearchStart += 2;
            helpIds << fqn.mid(sepSearchStart);
        }
        mark = helpIds.last();
        if (category == HelpItem::Function)
            mark += type.mid(type.indexOf('('));
    }
    if (category == HelpItem::Enum && !type.isEmpty())
        mark = type;

    HelpItem helpItem(helpIds, mark, category);
    if (isTesting)
        emit q->helpItemGathered(helpItem);
    else
        q->hoverHandler()->setHelpItem(token, helpItem);
}

// Unfortunately, clangd ignores almost everything except symbols when sending
// semantic token info, so we need to consult the AST for additional information.
// In particular, we inspect the following constructs:
//    - Raw string literals, because our built-in lexer does not parse them properly.
//      While we're at it, we also handle other types of literals.
//    - Ternary expressions (for the matching of "?" and ":").
//    - Template declarations and instantiations (for the matching of "<" and ">").
//    - Function declarations, to find out whether a declaration is also a definition.
//    - Function arguments, to find out whether they correspond to output parameters.
//    - We consider most other tokens to be simple enough to be handled by the built-in code model.
//      Sometimes we have no choice, as for #include directives, which appear neither
//      in the semantic tokens nor in the AST.
void ClangdClient::Private::handleSemanticTokens(TextDocument *doc,
                                                 const QList<ExpandedSemanticToken> &tokens,
                                                 int version, bool force)
{
    SubtaskTimer t(highlightingTimer);
    qCInfo(clangdLogHighlight) << "handling LSP tokens" << doc->filePath()
                               << version << tokens.size();
    if (version != q->documentVersion(doc->filePath())) {
        qCInfo(clangdLogHighlight) << "LSP tokens outdated; aborting highlighting procedure"
                                    << version << q->documentVersion(doc->filePath());
        return;
    }
    force = force || isTesting;
    const auto data = highlightingData.find(doc);
    if (data != highlightingData.end()) {
        if (!force && data->previousTokens.first == tokens
                && data->previousTokens.second == version) {
            qCInfo(clangdLogHighlight) << "tokens and version same as last time; nothing to do";
            return;
        }
        data->previousTokens.first = tokens;
        data->previousTokens.second = version;
    } else {
        HighlightingData data;
        data.previousTokens = qMakePair(tokens, version);
        highlightingData.insert(doc, data);
    }
    for (const ExpandedSemanticToken &t : tokens)
        qCDebug(clangdLogHighlight()) << '\t' << t.line << t.column << t.length << t.type
                                      << t.modifiers;

    const auto astHandler = [this, tokens, doc, version](const ClangdAstNode &ast, const MessageId &) {
        FinalizingSubtaskTimer t(highlightingTimer);
        if (!q->documentOpen(doc))
            return;
        if (version != q->documentVersion(doc->filePath())) {
            qCInfo(clangdLogHighlight) << "AST not up to date; aborting highlighting procedure"
                                        << version << q->documentVersion(doc->filePath());
            return;
        }
        if (clangdLogAst().isDebugEnabled())
            ast.print();

        const auto runner = [tokens, filePath = doc->filePath(),
                             text = doc->document()->toPlainText(), ast,
                             doc = QPointer(doc), rev = doc->document()->revision(),
                             clangdVersion = q->versionNumber(),
                             this] {
            return Utils::runAsync(doSemanticHighlighting, filePath, tokens, text, ast, doc, rev,
                                   clangdVersion, highlightingTimer);
        };

        if (isTesting) {
            const auto watcher = new QFutureWatcher<HighlightingResult>(q);
            connect(watcher, &QFutureWatcher<HighlightingResult>::finished,
                    q, [this, watcher, fp = doc->filePath()] {
                emit q->highlightingResultsReady(watcher->future().results(), fp);
                watcher->deleteLater();
            });
            watcher->setFuture(runner());
            return;
        }

        auto &data = highlightingData[doc];
        if (!data.highlighter)
            data.highlighter = new CppEditor::SemanticHighlighter(doc);
        else
            data.highlighter->updateFormatMapFromFontSettings();
        data.highlighter->setHighlightingRunner(runner);
        data.highlighter->run();
    };
    getAndHandleAst(doc, astHandler, AstCallbackMode::SyncIfPossible);
}

Utils::optional<QList<CodeAction> > ClangdDiagnostic::codeActions() const
{
    auto actions = optionalArray<LanguageServerProtocol::CodeAction>("codeActions");
    if (!actions)
        return actions;
    static const QStringList badCodeActions{
        "remove constant to silence this warning", // QTCREATORBUG-18593
    };
    for (auto it = actions->begin(); it != actions->end();) {
        if (badCodeActions.contains(it->title()))
            it = actions->erase(it);
        else
            ++it;
    }
    return actions;
}

QString ClangdDiagnostic::category() const
{
    return typedValue<QString>("category");
}

MessageId ClangdClient::Private::getAndHandleAst(const TextDocOrFile &doc,
                                                 const AstHandler &astHandler,
                                                 AstCallbackMode callbackMode, const Range &range)
{
    const auto textDocPtr = Utils::get_if<const TextDocument *>(&doc);
    const TextDocument * const textDoc = textDocPtr ? *textDocPtr : nullptr;
    const Utils::FilePath filePath = textDoc ? textDoc->filePath()
                                             : Utils::get<Utils::FilePath>(doc);

    // If the entire AST is requested and the document's AST is in the cache and it is up to date,
    // call the handler.
    const bool fullAstRequested = !range.isValid();
    if (fullAstRequested) {
        if (const auto ast = textDoc ? astCache.get(textDoc) : externalAstCache.get(filePath)) {
            qCDebug(clangdLog) << "using AST from cache";
            switch (callbackMode) {
            case AstCallbackMode::SyncIfPossible:
                astHandler(*ast, {});
                break;
            case AstCallbackMode::AlwaysAsync:
                QMetaObject::invokeMethod(q, [ast, astHandler] { astHandler(*ast, {}); },
                                      Qt::QueuedConnection);
                break;
            }
            return {};
        }
    }

    // Otherwise retrieve the AST from clangd.
    const auto wrapperHandler = [this, filePath, guardedTextDoc = QPointer(textDoc), astHandler,
            fullAstRequested, docRev = textDoc ? getRevision(textDoc) : -1,
            fileRev = getRevision(filePath)](const ClangdAstNode &ast, const MessageId &reqId) {
        qCDebug(clangdLog) << "retrieved AST from clangd";
        if (fullAstRequested) {
            if (guardedTextDoc) {
                if (docRev == getRevision(guardedTextDoc))
                    astCache.insert(guardedTextDoc, ast);
            } else if (fileRev == getRevision(filePath) && !q->documentForFilePath(filePath)) {
                externalAstCache.insert(filePath, ast);
            }
        }
        astHandler(ast, reqId);
    };
    qCDebug(clangdLog) << "requesting AST for" << filePath;
    return requestAst(q, filePath, range, wrapperHandler);
}

class MemoryTree : public JsonObject
{
public:
    using JsonObject::JsonObject;

    // number of bytes used, including child components
    qint64 total() const { return qint64(typedValue<double>(totalKey())); }

    // number of bytes used, excluding child components
    qint64 self() const { return qint64(typedValue<double>(selfKey())); }

    // named child components
    using NamedComponent = std::pair<MemoryTree, QString>;
    QList<NamedComponent> children() const
    {
        QList<NamedComponent> components;
        const auto obj = operator const QJsonObject &();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.key() == totalKey() || it.key() == selfKey())
                continue;
            components << std::make_pair(MemoryTree(it.value()), it.key());
        }
        return components;
    }

private:
    static QString totalKey() { return QLatin1String("_total"); }
    static QString selfKey() { return QLatin1String("_self"); }
};

class MemoryTreeItem : public Utils::TreeItem
{
    Q_DECLARE_TR_FUNCTIONS(MemoryTreeItem)
public:
    MemoryTreeItem(const QString &displayName, const MemoryTree &tree)
        : m_displayName(displayName), m_bytesUsed(tree.total())
    {
        for (const MemoryTree::NamedComponent &component : tree.children())
            appendChild(new MemoryTreeItem(component.second, component.first));
    }

private:
    QVariant data(int column, int role) const override
    {
        switch (role) {
        case Qt::DisplayRole:
            if (column == 0)
                return m_displayName;
            return memString();
        case Qt::TextAlignmentRole:
            if (column == 1)
                return Qt::AlignRight;
            break;
        default:
            break;
        }
        return {};
    }

    QString memString() const
    {
        static const QList<std::pair<int, QString>> factors{
            std::make_pair(1000000000, QString("GB")),
            std::make_pair(1000000, QString("MB")),
            std::make_pair(1000, QString("KB")),
        };
        for (const auto &factor : factors) {
            if (m_bytesUsed > factor.first)
                return QString::number(qint64(std::round(double(m_bytesUsed) / factor.first)))
                        + ' ' + factor.second;
        }
        return QString::number(m_bytesUsed) + "  B";
    }

    const QString m_displayName;
    const qint64 m_bytesUsed;
};

class MemoryTreeModel : public Utils::BaseTreeModel
{
public:
    MemoryTreeModel(QObject *parent) : BaseTreeModel(parent)
    {
        setHeader({MemoryUsageWidget::tr("Component"), MemoryUsageWidget::tr("Total Memory")});
    }

    void update(const MemoryTree &tree)
    {
        setRootItem(new MemoryTreeItem({}, tree));
    }
};

MemoryUsageWidget::MemoryUsageWidget(ClangdClient *client)
    : m_client(client), m_model(new MemoryTreeModel(this))
{
    setupUi();
    getMemoryTree();
}

MemoryUsageWidget::~MemoryUsageWidget()
{
    if (m_currentRequest.has_value())
        m_client->cancelRequest(m_currentRequest.value());
}

void MemoryUsageWidget::setupUi()
{
    const auto layout = new QVBoxLayout(this);
    m_view.setContextMenuPolicy(Qt::CustomContextMenu);
    m_view.header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_view.header()->setStretchLastSection(false);
    m_view.setModel(m_model);
    layout->addWidget(&m_view);
    connect(&m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu;
        menu.addAction(tr("Update"), [this] { getMemoryTree(); });
        menu.exec(m_view.mapToGlobal(pos));
    });
}

void MemoryUsageWidget::getMemoryTree()
{
    Request<MemoryTree, std::nullptr_t, JsonObject> request("$/memoryUsage", {});
    request.setResponseCallback([this](decltype(request)::Response response) {
        m_currentRequest.reset();
        qCDebug(clangdLog) << "received memory usage response";
        if (const auto result = response.result())
            m_model->update(*result);
    });
    qCDebug(clangdLog) << "sending memory usage request";
    m_currentRequest = request.id();
    m_client->sendMessage(request, ClangdClient::SendDocUpdates::Ignore);
}

} // namespace Internal
} // namespace ClangCodeModel
