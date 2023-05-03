// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "spotlightlocatorfilter.h"

#include "../coreplugintr.h"
#include "../messagemanager.h"

#include <extensionsystem/pluginmanager.h>

#include <utils/algorithm.h>
#include <utils/async.h>
#include <utils/commandline.h>
#include <utils/environment.h>
#include <utils/fancylineedit.h>
#include <utils/link.h>
#include <utils/macroexpander.h>
#include <utils/pathchooser.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>
#include <utils/stringutils.h>
#include <utils/variablechooser.h>

#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QWaitCondition>

using namespace Utils;

namespace Core {
namespace Internal {

// #pragma mark -- SpotlightIterator

class SpotlightIterator : public BaseFileFilter::Iterator
{
public:
    SpotlightIterator(const CommandLine &command);
    ~SpotlightIterator() override;

    void toFront() override;
    bool hasNext() const override;
    Utils::FilePath next() override;
    Utils::FilePath filePath() const override;

    void scheduleKillProcess();
    void killProcess();

private:
    void ensureNext();

    std::unique_ptr<QtcProcess> m_process;
    QMutex m_mutex;
    QWaitCondition m_waitForItems;
    FilePaths m_queue;
    FilePaths m_filePaths;
    int m_index;
    bool m_finished;
};

SpotlightIterator::SpotlightIterator(const CommandLine &command)
    : m_index(-1)
    , m_finished(false)
{
    QTC_ASSERT(!command.isEmpty(), return );
    m_process.reset(new QtcProcess);
    m_process->setCommand(command);
    m_process->setEnvironment(Utils::Environment::systemEnvironment());
    QObject::connect(m_process.get(), &QtcProcess::done,
                     m_process.get(), [this, exe = command.executable().toUserOutput()] {
        if (m_process->result() != ProcessResult::FinishedWithSuccess) {
            MessageManager::writeFlashing(Tr::tr(
                            "Locator: Error occurred when running \"%1\".").arg(exe));
        }
        scheduleKillProcess();
    });
    QObject::connect(m_process.get(), &QtcProcess::readyReadStandardOutput,
                     m_process.get(), [this] {
        QString output = m_process->readAllStandardOutput();
        output.replace("\r\n", "\n");
        const QStringList items = output.split('\n');
        QMutexLocker lock(&m_mutex);
        m_queue.append(Utils::transform(items, &FilePath::fromUserInput));
        if (m_filePaths.size() + m_queue.size() > 10000) // limit the amount of data
            scheduleKillProcess();
        m_waitForItems.wakeAll();
    });
    m_process->start();
}

SpotlightIterator::~SpotlightIterator()
{
    killProcess();
}

void SpotlightIterator::toFront()
{
    m_index = -1;
}

bool SpotlightIterator::hasNext() const
{
    auto that = const_cast<SpotlightIterator *>(this);
    that->ensureNext();
    return (m_index + 1 < m_filePaths.size());
}

Utils::FilePath SpotlightIterator::next()
{
    ensureNext();
    ++m_index;
    QTC_ASSERT(m_index < m_filePaths.size(), return FilePath());
    return m_filePaths.at(m_index);
}

Utils::FilePath SpotlightIterator::filePath() const
{
    QTC_ASSERT(m_index < m_filePaths.size(), return FilePath());
    return m_filePaths.at(m_index);
}

void SpotlightIterator::scheduleKillProcess()
{
    QMetaObject::invokeMethod(m_process.get(), [this] { killProcess(); }, Qt::QueuedConnection);
}

void SpotlightIterator::killProcess()
{
    if (!m_process)
        return;
    m_process->disconnect();
    QMutexLocker lock(&m_mutex);
    m_finished = true;
    m_waitForItems.wakeAll();
    m_process.reset();
}

void SpotlightIterator::ensureNext()
{
    if (m_index + 1 < m_filePaths.size()) // nothing to do
        return;
    // check if there are items in the queue, otherwise wait for some
    QMutexLocker lock(&m_mutex);
    if (m_queue.isEmpty() && !m_finished)
        m_waitForItems.wait(&m_mutex);
    m_filePaths.append(m_queue);
    m_queue.clear();
}

// #pragma mark -- SpotlightLocatorFilter

static QString defaultCommand()
{
    if (HostOsInfo::isMacHost())
        return "mdfind";
    if (HostOsInfo::isWindowsHost())
        return "es.exe";
    return "locate";
}

/*
    For the tools es [1] and locate [2], interpret space as AND operator.

    Currently doesn't support fine picking a file with a space in the path by escaped space.

    [1]: https://www.voidtools.com/support/everything/command_line_interface/
    [2]: https://www.gnu.org/software/findutils/manual/html_node/find_html/Invoking-locate.html
 */

static QString defaultArguments(Qt::CaseSensitivity sens = Qt::CaseInsensitive)
{
    if (HostOsInfo::isMacHost())
        return QString("\"kMDItemFSName = '*%{Query:EscapedWithWildcards}*'%1\"")
            .arg(sens == Qt::CaseInsensitive ? QString("c") : "");
    if (HostOsInfo::isWindowsHost())
        return QString("%1 -n 10000 %{Query:Escaped}")
            .arg(sens == Qt::CaseInsensitive ? QString() : "-i ");
    return QString("%1 -A -l 10000 %{Query:Escaped}")
        .arg(sens == Qt::CaseInsensitive ? QString() : "-i ");
}

const char kCommandKey[] = "command";
const char kArgumentsKey[] = "arguments";
const char kCaseSensitiveKey[] = "caseSensitive";

static QString escaped(const QString &query)
{
    QString quoted = query;
    quoted.replace('\\', "\\\\").replace('\'', "\\\'").replace('\"', "\\\"");
    return quoted;
}

static MacroExpander *createMacroExpander(const QString &query)
{
    MacroExpander *expander = new MacroExpander;
    expander->registerVariable("Query",
                               Tr::tr("Locator query string."),
                               [query] { return query; });
    expander->registerVariable("Query:Escaped",
                               Tr::tr("Locator query string with quotes escaped with backslash."),
                               [query] { return escaped(query); });
    expander->registerVariable("Query:EscapedWithWildcards",
                               Tr::tr("Locator query string with quotes escaped with backslash and "
                                      "spaces replaced with \"*\" wildcards."),
                               [query] {
                                   QString quoted = escaped(query);
                                   quoted.replace(' ', '*');
                                   return quoted;
                               });
    expander->registerVariable("Query:Regex",
                               Tr::tr("Locator query string as regular expression."),
                               [query] {
                                   QString regex = query;
                                   regex = regex.replace('*', ".*");
                                   regex = regex.replace(' ', ".*");
                                   return regex;
                               });
    return expander;
}

SpotlightLocatorFilter::SpotlightLocatorFilter()
{
    setId("SpotlightFileNamesLocatorFilter");
    setDefaultShortcutString("md");
    setDefaultIncludedByDefault(false);
    setDisplayName(Tr::tr("File Name Index"));
    setDescription(Tr::tr(
        "Locates files from a global file system index (Spotlight, Locate, Everything). Append "
        "\"+<number>\" or \":<number>\" to jump to the given line number. Append another "
        "\"+<number>\" or \":<number>\" to jump to the column number as well."));
    setConfigurable(true);
    reset();
}

static void matches(QPromise<void> &promise, const LocatorStorage &storage,
                    const CommandLine &command)
{
    // If search string contains spaces, treat them as wildcard '*' and search in full path
    const QString wildcardInput = QDir::fromNativeSeparators(storage.input()).replace(' ', '*');
    const Link inputLink = Link::fromString(wildcardInput, true);
    const QString newInput = inputLink.targetFilePath.toString();
    const QRegularExpression regExp = ILocatorFilter::createRegExp(newInput);
    if (!regExp.isValid())
        return;

    const bool hasPathSeparator = newInput.contains('/') || newInput.contains('*');
    LocatorFileCache::MatchedEntries entries = {};
    QEventLoop loop;
    QtcProcess process;
    process.setCommand(command);
    process.setEnvironment(Environment::systemEnvironment()); // TODO: Is it needed?

    QObject::connect(&process, &QtcProcess::readyReadStandardOutput, &process,
                     [&, entriesPtr = &entries] {
        QString output = process.readAllStandardOutput();
        output.replace("\r\n", "\n");
        const QStringList items = output.split('\n');
        const FilePaths filePaths = Utils::transform(items, &FilePath::fromUserInput);
        LocatorFileCache::processFilePaths(promise.future(), filePaths, hasPathSeparator, regExp,
                                           inputLink, entriesPtr);
        if (promise.isCanceled())
            loop.exit();
    });
    QObject::connect(&process, &QtcProcess::done, &process, [&] {
        if (process.result() != ProcessResult::FinishedWithSuccess) {
            MessageManager::writeFlashing(Tr::tr("Locator: Error occurred when running \"%1\".")
                                              .arg(command.executable().toUserOutput()));
        }
        loop.exit();
    });
    QFutureWatcher<void> watcher;
    watcher.setFuture(promise.future());
    QObject::connect(&watcher, &QFutureWatcherBase::canceled, &watcher, [&loop] { loop.exit(); });
    if (promise.isCanceled())
        return;
    process.start();
    loop.exec();

    for (auto &entry : entries) {
        if (promise.isCanceled())
            return;
        if (entry.size() < 1000)
            Utils::sort(entry, LocatorFilterEntry::compareLexigraphically);
    }
    if (promise.isCanceled())
        return;
    storage.reportOutput(std::accumulate(std::begin(entries), std::end(entries),
                                         LocatorFilterEntries()));
}

LocatorMatcherTasks SpotlightLocatorFilter::matchers()
{
    using namespace Tasking;

    TreeStorage<LocatorStorage> storage;

    const auto onSetup = [storage, command = m_command, insensArgs = m_arguments,
                          sensArgs = m_caseSensitiveArguments](Async<void> &async) {
        const Link link = Link::fromString(storage->input(), true);
        const FilePath input = link.targetFilePath;
        if (input.isEmpty())
            return TaskAction::StopWithDone;

        // only pass the file name part to allow searches like "somepath/*foo"
        const std::unique_ptr<MacroExpander> expander(createMacroExpander(input.fileName()));
        const QString args = caseSensitivity(input.toString()) == Qt::CaseInsensitive
                           ? insensArgs : sensArgs;
        const CommandLine cmd(FilePath::fromString(command), expander->expand(args),
                              CommandLine::Raw);
        async.setFutureSynchronizer(ExtensionSystem::PluginManager::futureSynchronizer());
        async.setConcurrentCallData(matches, *storage, cmd);
        return TaskAction::Continue;
    };

    return {{AsyncTask<void>(onSetup), storage}};
}

void SpotlightLocatorFilter::prepareSearch(const QString &entry)
{
    Link link = Utils::Link::fromString(entry, true);
    if (link.targetFilePath.isEmpty()) {
        setFileIterator(new BaseFileFilter::ListIterator(Utils::FilePaths()));
    } else {
        // only pass the file name part to allow searches like "somepath/*foo"

        std::unique_ptr<MacroExpander> expander(createMacroExpander(link.targetFilePath.fileName()));
        const QString argumentString = expander->expand(
            caseSensitivity(link.targetFilePath.toString()) == Qt::CaseInsensitive
                ? m_arguments
                : m_caseSensitiveArguments);
        const CommandLine cmd(FilePath::fromString(m_command), argumentString, CommandLine::Raw);
        setFileIterator(new SpotlightIterator(cmd));
    }
    BaseFileFilter::prepareSearch(entry);
}

bool SpotlightLocatorFilter::openConfigDialog(QWidget *parent, bool &needsRefresh)
{
    Q_UNUSED(needsRefresh)
    QWidget configWidget;
    QFormLayout *layout = new QFormLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    configWidget.setLayout(layout);
    PathChooser *commandEdit = new PathChooser;
    commandEdit->setExpectedKind(PathChooser::ExistingCommand);
    commandEdit->lineEdit()->setText(m_command);
    FancyLineEdit *argumentsEdit = new FancyLineEdit;
    argumentsEdit->setText(m_arguments);
    FancyLineEdit *caseSensitiveArgumentsEdit = new FancyLineEdit;
    caseSensitiveArgumentsEdit->setText(m_caseSensitiveArguments);
    layout->addRow(Tr::tr("Executable:"), commandEdit);
    layout->addRow(Tr::tr("Arguments:"), argumentsEdit);
    layout->addRow(Tr::tr("Case sensitive:"), caseSensitiveArgumentsEdit);
    std::unique_ptr<MacroExpander> expander(createMacroExpander(""));
    auto chooser = new VariableChooser(&configWidget);
    chooser->addMacroExpanderProvider([expander = expander.get()] { return expander; });
    chooser->addSupportedWidget(argumentsEdit);
    chooser->addSupportedWidget(caseSensitiveArgumentsEdit);
    const bool accepted = openConfigDialog(parent, &configWidget);
    if (accepted) {
        m_command = commandEdit->rawFilePath().toString();
        m_arguments = argumentsEdit->text();
        m_caseSensitiveArguments = caseSensitiveArgumentsEdit->text();
    }
    return accepted;
}

void SpotlightLocatorFilter::saveState(QJsonObject &obj) const
{
    if (m_command != defaultCommand())
        obj.insert(kCommandKey, m_command);
    if (m_arguments != defaultArguments())
        obj.insert(kArgumentsKey, m_arguments);
    if (m_caseSensitiveArguments != defaultArguments(Qt::CaseSensitive))
        obj.insert(kCaseSensitiveKey, m_caseSensitiveArguments);
}

void SpotlightLocatorFilter::restoreState(const QJsonObject &obj)
{
    m_command = obj.value(kCommandKey).toString(defaultCommand());
    m_arguments = obj.value(kArgumentsKey).toString(defaultArguments());
    m_caseSensitiveArguments = obj.value(kCaseSensitiveKey).toString(defaultArguments(Qt::CaseSensitive));
}

void SpotlightLocatorFilter::reset()
{
    m_command = defaultCommand();
    m_arguments = defaultArguments();
    m_caseSensitiveArguments = defaultArguments(Qt::CaseSensitive);
}

} // Internal
} // Core
