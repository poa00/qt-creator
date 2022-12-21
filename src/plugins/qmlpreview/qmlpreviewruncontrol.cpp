// Copyright (C) 2019 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "qmlpreviewruncontrol.h"

#include <qmlprojectmanager/qmlproject.h>
#include <qmlprojectmanager/qmlmainfileaspect.h>

#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/session.h>
#include <projectexplorer/target.h>

#include <qmldebug/qmldebugcommandlinearguments.h>
#include <qtsupport/baseqtversion.h>
#include <qtsupport/qtkitinformation.h>

#include <utils/filepath.h>
#include <utils/port.h>
#include <utils/qtcprocess.h>
#include <utils/url.h>

using namespace ProjectExplorer;
using namespace Utils;

namespace QmlPreview {

static const QString QmlServerUrl = "QmlServerUrl";

QmlPreviewRunner::QmlPreviewRunner(const QmlPreviewRunnerSetting &settings)
    : RunWorker(settings.runControl)
{
    setId("QmlPreviewRunner");
    m_connectionManager.setFileLoader(settings.fileLoader);
    m_connectionManager.setFileClassifier(settings.fileClassifier);
    m_connectionManager.setFpsHandler(settings.fpsHandler);
    m_connectionManager.setQmlDebugTranslationClientCreator(
        settings.createDebugTranslationClientMethod);

    connect(this, &QmlPreviewRunner::loadFile,
            &m_connectionManager, &Internal::QmlPreviewConnectionManager::loadFile);
    connect(this, &QmlPreviewRunner::rerun,
            &m_connectionManager, &Internal::QmlPreviewConnectionManager::rerun);

    connect(this, &QmlPreviewRunner::zoom,
            &m_connectionManager, &Internal::QmlPreviewConnectionManager::zoom);
    connect(this, &QmlPreviewRunner::language,
            &m_connectionManager, &Internal::QmlPreviewConnectionManager::language);

    connect(&m_connectionManager, &Internal::QmlPreviewConnectionManager::connectionOpened,
            this, [this, settings]() {
        if (settings.zoom > 0)
            emit zoom(settings.zoom);
        if (!settings.language.isEmpty())
            emit language(settings.language);

        emit ready();
    });

    connect(&m_connectionManager, &Internal::QmlPreviewConnectionManager::restart,
            runControl(), [this]() {
        if (!runControl()->isRunning())
            return;

        this->connect(runControl(), &RunControl::stopped, [this] {
            auto rc = new RunControl(ProjectExplorer::Constants::QML_PREVIEW_RUN_MODE);
            rc->copyDataFromRunControl(runControl());
            ProjectExplorerPlugin::startRunControl(rc);
        });

        runControl()->initiateStop();
    });
}

void QmlPreviewRunner::start()
{
    m_connectionManager.setTarget(runControl()->target());
    m_connectionManager.connectToServer(serverUrl());
    reportStarted();
}

void QmlPreviewRunner::stop()
{
    m_connectionManager.disconnectFromServer();
    reportStopped();
}

void QmlPreviewRunner::setServerUrl(const QUrl &serverUrl)
{
    recordData(QmlServerUrl, serverUrl);
}

QUrl QmlPreviewRunner::serverUrl() const
{
    return recordedData(QmlServerUrl).toUrl();
}

class LocalQmlPreviewSupport final : public SimpleTargetRunner
{
public:
    LocalQmlPreviewSupport(RunControl *runControl)
        : SimpleTargetRunner(runControl)
    {
        setId("LocalQmlPreviewSupport");
        const QUrl serverUrl = Utils::urlFromLocalSocket();

        QmlPreviewRunner *preview = qobject_cast<QmlPreviewRunner *>(
            runControl->createWorker(ProjectExplorer::Constants::QML_PREVIEW_RUNNER));
        preview->setServerUrl(serverUrl);

        addStopDependency(preview);
        addStartDependency(preview);

        setStartModifier([this, runControl, serverUrl] {
            CommandLine cmd = commandLine();

            if (const auto aspect = runControl->aspect<QmlProjectManager::QmlMainFileAspect>()) {
                const auto qmlBuildSystem = qobject_cast<QmlProjectManager::QmlBuildSystem *>(
                    runControl->target()->buildSystem());
                QTC_ASSERT(qmlBuildSystem, return);

                const QString mainScript = aspect->mainScript;
                const QString currentFile = aspect->currentFile;

                const QString mainScriptFromProject = qmlBuildSystem->targetFile(
                    FilePath::fromString(mainScript)).path();

                QStringList qmlProjectRunConfigurationArguments = cmd.splitArguments();

                if (!currentFile.isEmpty() && qmlProjectRunConfigurationArguments.last().contains(mainScriptFromProject)) {
                    qmlProjectRunConfigurationArguments.removeLast();
                    cmd = CommandLine(cmd.executable(), qmlProjectRunConfigurationArguments);
                    cmd.addArg(currentFile);
                }
            }

            cmd.addArg(QmlDebug::qmlDebugLocalArguments(QmlDebug::QmlPreviewServices, serverUrl.path()));
            setCommandLine(cmd);

            forceRunOnHost();
        });
    }
};

LocalQmlPreviewSupportFactory::LocalQmlPreviewSupportFactory()
{
    setProduct<LocalQmlPreviewSupport>();
    addSupportedRunMode(ProjectExplorer::Constants::QML_PREVIEW_RUN_MODE);
    addSupportedDeviceType(ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE);
}

} // QmlPreview
