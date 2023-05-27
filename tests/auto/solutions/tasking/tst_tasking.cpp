// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include <tasking/barrier.h>

#include <QTimer>
#include <QtTest>

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace Tasking;

template <bool SuccessOnDone>
class TASKING_EXPORT DurationTaskAdapter : public TaskAdapter<std::chrono::milliseconds>
{
public:
    DurationTaskAdapter() { *task() = std::chrono::milliseconds{0}; }
    void start() final { QTimer::singleShot(*task(), this, [this] { emit done(SuccessOnDone); }); }
};

TASKING_DECLARE_TASK(SuccessTask, DurationTaskAdapter<true>);
TASKING_DECLARE_TASK(FailingTask, DurationTaskAdapter<false>);

using TaskObject = milliseconds;

namespace PrintableEnums {

Q_NAMESPACE

enum class Handler {
    Setup,
    Done,
    Error,
    GroupSetup,
    GroupDone,
    GroupError,
    Sync,
    BarrierAdvance
};
Q_ENUM_NS(Handler);

enum class OnDone { Success, Failure };
Q_ENUM_NS(OnDone);

} // namespace PrintableEnums

using namespace PrintableEnums;

using Log = QList<QPair<int, Handler>>;

struct CustomStorage
{
    CustomStorage() { ++s_count; }
    ~CustomStorage() { --s_count; }
    Log m_log;
    static int instanceCount() { return s_count; }
private:
    static int s_count;
};

int CustomStorage::s_count = 0;
static const char s_taskIdProperty[] = "__taskId";

struct TestData {
    TreeStorage<CustomStorage> storage;
    Group root;
    Log expectedLog;
    int taskCount = 0;
    OnDone onDone = OnDone::Success;
};

class tst_Tasking : public QObject
{
    Q_OBJECT

private slots:
    void validConstructs(); // compile test
    void testTree_data();
    void testTree();
    void storageOperators();
    void storageDestructor();
};

void tst_Tasking::validConstructs()
{
    const Group task {
        parallel,
        SuccessTask([](TaskObject &) {}, [](const TaskObject &) {}),
        SuccessTask([](TaskObject &) {}, [](const TaskObject &) {}),
        SuccessTask([](TaskObject &) {}, [](const TaskObject &) {})
    };

    const Group group1 {
        task
    };

    const Group group2 {
        parallel,
        Group {
            parallel,
            SuccessTask([](TaskObject &) {}, [](const TaskObject &) {}),
            Group {
                parallel,
                SuccessTask([](TaskObject &) {}, [](const TaskObject &) {}),
                Group {
                    parallel,
                    SuccessTask([](TaskObject &) {}, [](const TaskObject &) {})
                }
            },
            Group {
                parallel,
                SuccessTask([](TaskObject &) {}, [](const TaskObject &) {}),
                onGroupDone([] {})
            }
        },
        task,
        onGroupDone([] {}),
        onGroupError([] {})
    };

    const auto setupHandler = [](TaskObject &) {};
    const auto doneHandler = [](const TaskObject &) {};
    const auto errorHandler = [](const TaskObject &) {};

    // Not fluent interface

    const Group task2 {
        parallel,
        SuccessTask(setupHandler),
        SuccessTask(setupHandler, doneHandler),
        SuccessTask(setupHandler, doneHandler, errorHandler),
        // need to explicitly pass empty handler for done
        SuccessTask(setupHandler, {}, errorHandler)
    };

    // Fluent interface

    const Group fluent {
        parallel,
        SuccessTask().onSetup(setupHandler),
        SuccessTask().onSetup(setupHandler).onDone(doneHandler),
        SuccessTask().onSetup(setupHandler).onDone(doneHandler).onError(errorHandler),
        // possible to skip the empty done
        SuccessTask().onSetup(setupHandler).onError(errorHandler),
        // possible to set handlers in a different order
        SuccessTask().onError(errorHandler).onDone(doneHandler).onSetup(setupHandler),
    };


    // When turning each of below blocks on, you should see the specific compiler error message.

#if 0
    {
        // "Sync element: The synchronous function has to return void or bool."
        const auto setupSync = [] { return 3; };
        const Sync sync(setupSync);
    }
#endif

#if 0
    {
        // "Sync element: The synchronous function can't take any arguments."
        const auto setupSync = [](int) { };
        const Sync sync(setupSync);
    }
#endif

#if 0
    {
        // "Sync element: The synchronous function can't take any arguments."
        const auto setupSync = [](int) { return true; };
        const Sync sync(setupSync);
    }
#endif
}

class TickAndDone : public QObject
{
    Q_OBJECT

public:
    void setInterval(const milliseconds &interval) { m_interval = interval; }
    void start() {
        QTimer::singleShot(0, this, [this] {
            emit tick();
            QTimer::singleShot(m_interval, this, &TickAndDone::done);
        });
    }

signals:
    void tick();
    void done();

private:
    milliseconds m_interval;
};

class TickAndDoneTaskAdapter : public TaskAdapter<TickAndDone>
{
public:
    TickAndDoneTaskAdapter() { connect(task(), &TickAndDone::done, this,
                                       [this] { emit done(true); }); }
    void start() final { task()->start(); }
};

TASKING_DECLARE_TASK(TickAndDoneTask, TickAndDoneTaskAdapter);

template <typename SharedBarrierType>
TaskItem createBarrierAdvance(const TreeStorage<CustomStorage> &storage,
                              const SharedBarrierType &barrier, int taskId)
{
    return TickAndDoneTask([storage, barrier, taskId](TickAndDone &tickAndDone) {
        tickAndDone.setInterval(1ms);
        storage->m_log.append({taskId, Handler::Setup});

        CustomStorage *currentStorage = storage.activeStorage();
        Barrier *sharedBarrier = barrier->barrier();
        QObject::connect(&tickAndDone, &TickAndDone::tick, sharedBarrier,
                         [currentStorage, sharedBarrier, taskId] {
            currentStorage->m_log.append({taskId, Handler::BarrierAdvance});
            sharedBarrier->advance();
        });
    });
}

void tst_Tasking::testTree_data()
{
    QTest::addColumn<TestData>("testData");

    TreeStorage<CustomStorage> storage;

    const auto setupTask = [storage](int taskId, milliseconds timeout) {
        return [storage, taskId, timeout](TaskObject &taskObject) {
            taskObject = timeout;
            storage->m_log.append({taskId, Handler::Setup});
        };
    };

    const auto setupDynamicTask = [storage](int taskId, TaskAction action) {
        return [storage, taskId, action](TaskObject &) {
            storage->m_log.append({taskId, Handler::Setup});
            return action;
        };
    };

    const auto setupDone = [storage](int taskId) {
        return [storage, taskId](const TaskObject &) {
            storage->m_log.append({taskId, Handler::Done});
        };
    };

    const auto setupError = [storage](int taskId) {
        return [storage, taskId](const TaskObject &) {
            storage->m_log.append({taskId, Handler::Error});
        };
    };

    const auto createTask = [storage, setupTask, setupDone, setupError](
            int taskId, bool successTask, milliseconds timeout = 0ms) -> TaskItem {
        if (successTask)
            return SuccessTask(setupTask(taskId, timeout), setupDone(taskId), setupError(taskId));
        return FailingTask(setupTask(taskId, timeout), setupDone(taskId), setupError(taskId));
    };

    const auto createSuccessTask = [createTask](int taskId, milliseconds timeout = 0ms) {
        return createTask(taskId, true, timeout);
    };

    const auto createFailingTask = [createTask](int taskId, milliseconds timeout = 0ms) {
        return createTask(taskId, false, timeout);
    };

    const auto createDynamicTask = [storage, setupDynamicTask, setupDone, setupError](
                                       int taskId, TaskAction action) {
        return SuccessTask(setupDynamicTask(taskId, action), setupDone(taskId), setupError(taskId));
    };

    const auto groupSetup = [storage](int taskId) {
        return onGroupSetup([=] { storage->m_log.append({taskId, Handler::GroupSetup}); });
    };
    const auto groupDone = [storage](int taskId) {
        return onGroupDone([=] { storage->m_log.append({taskId, Handler::GroupDone}); });
    };
    const auto groupError = [storage](int taskId) {
        return onGroupError([=] { storage->m_log.append({taskId, Handler::GroupError}); });
    };
    const auto createSync = [storage](int taskId) {
        return Sync([=] { storage->m_log.append({taskId, Handler::Sync}); });
    };
    const auto createSyncWithReturn = [storage](int taskId, bool success) {
        return Sync([=] { storage->m_log.append({taskId, Handler::Sync}); return success; });
    };

    {
        const Group root1 {
            Storage(storage),
            groupDone(0),
            groupError(0)
        };
        const Group root2 {
            Storage(storage),
            onGroupSetup([] { return TaskAction::Continue; }),
            groupDone(0),
            groupError(0)
        };
        const Group root3 {
            Storage(storage),
            onGroupSetup([] { return TaskAction::StopWithDone; }),
            groupDone(0),
            groupError(0)
        };
        const Group root4 {
            Storage(storage),
            onGroupSetup([] { return TaskAction::StopWithError; }),
            groupDone(0),
            groupError(0)
        };
        const Log logDone {{0, Handler::GroupDone}};
        const Log logError {{0, Handler::GroupError}};
        QTest::newRow("Empty") << TestData{storage, root1, logDone, 0, OnDone::Success};
        QTest::newRow("EmptyContinue") << TestData{storage, root2, logDone, 0, OnDone::Success};
        QTest::newRow("EmptyDone") << TestData{storage, root3, logDone, 0, OnDone::Success};
        QTest::newRow("EmptyError") << TestData{storage, root4, logError, 0, OnDone::Failure};
    }

    {
        const Group root {
            Storage(storage),
            createDynamicTask(1, TaskAction::StopWithDone),
            createDynamicTask(2, TaskAction::StopWithDone)
        };
        const Log log {{1, Handler::Setup}, {2, Handler::Setup}};
        QTest::newRow("DynamicTaskDone") << TestData{storage, root, log, 2, OnDone::Success};
    }

    {
        const Group root {
            Storage(storage),
            createDynamicTask(1, TaskAction::StopWithError),
            createDynamicTask(2, TaskAction::StopWithError)
        };
        const Log log {{1, Handler::Setup}};
        QTest::newRow("DynamicTaskError") << TestData{storage, root, log, 2, OnDone::Failure};
    }

    {
        const Group root {
            Storage(storage),
            createDynamicTask(1, TaskAction::Continue),
            createDynamicTask(2, TaskAction::Continue),
            createDynamicTask(3, TaskAction::StopWithError),
            createDynamicTask(4, TaskAction::Continue)
        };
        const Log log {
            {1, Handler::Setup},
            {1, Handler::Done},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Setup}
        };
        QTest::newRow("DynamicMixed") << TestData{storage, root, log, 4, OnDone::Failure};
    }

    {
        const Group root {
            parallel,
            Storage(storage),
            createDynamicTask(1, TaskAction::Continue),
            createDynamicTask(2, TaskAction::Continue),
            createDynamicTask(3, TaskAction::StopWithError),
            createDynamicTask(4, TaskAction::Continue)
        };
        const Log log {
            {1, Handler::Setup},
            {2, Handler::Setup},
            {3, Handler::Setup},
            {1, Handler::Error},
            {2, Handler::Error}
        };
        QTest::newRow("DynamicParallel") << TestData{storage, root, log, 4, OnDone::Failure};
    }

    {
        const Group root {
            parallel,
            Storage(storage),
            createDynamicTask(1, TaskAction::Continue),
            createDynamicTask(2, TaskAction::Continue),
            Group {
                createDynamicTask(3, TaskAction::StopWithError)
            },
            createDynamicTask(4, TaskAction::Continue)
        };
        const Log log {
            {1, Handler::Setup},
            {2, Handler::Setup},
            {3, Handler::Setup},
            {1, Handler::Error},
            {2, Handler::Error}
        };
        QTest::newRow("DynamicParallelGroup") << TestData{storage, root, log, 4, OnDone::Failure};
    }

    {
        const Group root {
            parallel,
            Storage(storage),
            createDynamicTask(1, TaskAction::Continue),
            createDynamicTask(2, TaskAction::Continue),
            Group {
                onGroupSetup([storage] {
                    storage->m_log.append({0, Handler::GroupSetup});
                    return TaskAction::StopWithError;
                }),
                createDynamicTask(3, TaskAction::Continue)
            },
            createDynamicTask(4, TaskAction::Continue)
        };
        const Log log {
            {1, Handler::Setup},
            {2, Handler::Setup},
            {0, Handler::GroupSetup},
            {1, Handler::Error},
            {2, Handler::Error}
        };
        QTest::newRow("DynamicParallelGroupSetup")
            << TestData{storage, root, log, 4, OnDone::Failure};
    }

    {
        const Group root {
            Storage(storage),
            Group {
                Group {
                    Group {
                        Group {
                            Group {
                                createSuccessTask(5),
                                groupSetup(5),
                                groupDone(5)
                            },
                            groupSetup(4),
                            groupDone(4)
                        },
                        groupSetup(3),
                        groupDone(3)
                    },
                    groupSetup(2),
                    groupDone(2)
                },
                groupSetup(1),
                groupDone(1)
            },
            groupDone(0)
        };
        const Log log {
            {1, Handler::GroupSetup},
            {2, Handler::GroupSetup},
            {3, Handler::GroupSetup},
            {4, Handler::GroupSetup},
            {5, Handler::GroupSetup},
            {5, Handler::Setup},
            {5, Handler::Done},
            {5, Handler::GroupDone},
            {4, Handler::GroupDone},
            {3, Handler::GroupDone},
            {2, Handler::GroupDone},
            {1, Handler::GroupDone},
            {0, Handler::GroupDone}
        };
        QTest::newRow("Nested") << TestData{storage, root, log, 1, OnDone::Success};
    }

    {
        const Group root {
            Storage(storage),
            parallel,
            createSuccessTask(1),
            createSuccessTask(2),
            createSuccessTask(3),
            createSuccessTask(4),
            createSuccessTask(5),
            groupDone(0)
        };
        const Log log {
            {1, Handler::Setup}, // Setup order is determined in parallel mode
            {2, Handler::Setup},
            {3, Handler::Setup},
            {4, Handler::Setup},
            {5, Handler::Setup},
            {1, Handler::Done},
            {2, Handler::Done},
            {3, Handler::Done},
            {4, Handler::Done},
            {5, Handler::Done},
            {0, Handler::GroupDone}
        };
        QTest::newRow("Parallel") << TestData{storage, root, log, 5, OnDone::Success};
    }

    {
        auto setupSubTree = [storage, createSuccessTask](TaskTree &taskTree) {
            const Group nestedRoot {
                Storage(storage),
                createSuccessTask(2),
                createSuccessTask(3),
                createSuccessTask(4)
            };
            taskTree.setupRoot(nestedRoot);
            CustomStorage *activeStorage = storage.activeStorage();
            auto collectSubLog = [activeStorage](CustomStorage *subTreeStorage){
                activeStorage->m_log += subTreeStorage->m_log;
            };
            taskTree.onStorageDone(storage, collectSubLog);
        };
        const Group root1 {
            Storage(storage),
            createSuccessTask(1),
            createSuccessTask(2),
            createSuccessTask(3),
            createSuccessTask(4),
            createSuccessTask(5),
            groupDone(0)
        };
        const Group root2 {
            Storage(storage),
            Group { createSuccessTask(1) },
            Group { createSuccessTask(2) },
            Group { createSuccessTask(3) },
            Group { createSuccessTask(4) },
            Group { createSuccessTask(5) },
            groupDone(0)
        };
        const Group root3 {
            Storage(storage),
            createSuccessTask(1),
            TaskTreeTask(setupSubTree),
            createSuccessTask(5),
            groupDone(0)
        };
        const Log log {
            {1, Handler::Setup},
            {1, Handler::Done},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Setup},
            {3, Handler::Done},
            {4, Handler::Setup},
            {4, Handler::Done},
            {5, Handler::Setup},
            {5, Handler::Done},
            {0, Handler::GroupDone}
        };
        QTest::newRow("Sequential") << TestData{storage, root1, log, 5, OnDone::Success};
        QTest::newRow("SequentialEncapsulated") << TestData{storage, root2, log, 5, OnDone::Success};
        // We don't inspect subtrees, so taskCount is 3, not 5.
        QTest::newRow("SequentialSubTree") << TestData{storage, root3, log, 3, OnDone::Success};
    }

    {
        const Group root {
            Storage(storage),
            Group {
                createSuccessTask(1),
                Group {
                    createSuccessTask(2),
                    Group {
                        createSuccessTask(3),
                        Group {
                            createSuccessTask(4),
                            Group {
                                createSuccessTask(5),
                                groupDone(5)
                            },
                            groupDone(4)
                        },
                        groupDone(3)
                    },
                    groupDone(2)
                },
                groupDone(1)
            },
            groupDone(0)
        };
        const Log log {
            {1, Handler::Setup},
            {1, Handler::Done},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Setup},
            {3, Handler::Done},
            {4, Handler::Setup},
            {4, Handler::Done},
            {5, Handler::Setup},
            {5, Handler::Done},
            {5, Handler::GroupDone},
            {4, Handler::GroupDone},
            {3, Handler::GroupDone},
            {2, Handler::GroupDone},
            {1, Handler::GroupDone},
            {0, Handler::GroupDone}
        };
        QTest::newRow("SequentialNested") << TestData{storage, root, log, 5, OnDone::Success};
    }

    {
        const Group root {
            Storage(storage),
            createSuccessTask(1),
            createSuccessTask(2),
            createFailingTask(3),
            createSuccessTask(4),
            createSuccessTask(5),
            groupDone(0),
            groupError(0)
        };
        const Log log {
            {1, Handler::Setup},
            {1, Handler::Done},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Setup},
            {3, Handler::Error},
            {0, Handler::GroupError}
        };
        QTest::newRow("SequentialError") << TestData{storage, root, log, 5, OnDone::Failure};
    }

    {
        const auto createRoot = [storage, groupDone, groupError](WorkflowPolicy policy) {
            return Group {
                Storage(storage),
                workflowPolicy(policy),
                groupDone(0),
                groupError(0)
            };
        };

        const Log log = {{0, Handler::GroupDone}};

        const Group root1 = createRoot(WorkflowPolicy::StopOnError);
        QTest::newRow("EmptyStopOnError") << TestData{storage, root1, log, 0, OnDone::Success};

        const Group root2 = createRoot(WorkflowPolicy::ContinueOnError);
        QTest::newRow("EmptyContinueOnError") << TestData{storage, root2, log, 0, OnDone::Success};

        const Group root3 = createRoot(WorkflowPolicy::StopOnDone);
        QTest::newRow("EmptyStopOnDone") << TestData{storage, root3, log, 0, OnDone::Success};

        const Group root4 = createRoot(WorkflowPolicy::ContinueOnDone);
        QTest::newRow("EmptyContinueOnDone") << TestData{storage, root4, log, 0, OnDone::Success};

        const Group root5 = createRoot(WorkflowPolicy::StopOnFinished);
        QTest::newRow("EmptyStopOnFinished") << TestData{storage, root5, log, 0, OnDone::Success};

        const Group root6 = createRoot(WorkflowPolicy::Optional);
        QTest::newRow("EmptyOptional") << TestData{storage, root6, log, 0, OnDone::Success};
    }

    {
        const auto createRoot = [storage, createSuccessTask, groupDone, groupError](
                                    WorkflowPolicy policy) {
            return Group {
                Storage(storage),
                workflowPolicy(policy),
                createSuccessTask(1),
                groupDone(0),
                groupError(0)
            };
        };

        const Log log = {
            {1, Handler::Setup},
            {1, Handler::Done},
            {0, Handler::GroupDone}
        };

        const Group root1 = createRoot(WorkflowPolicy::StopOnError);
        QTest::newRow("DoneStopOnError") << TestData{storage, root1, log, 1, OnDone::Success};

        const Group root2 = createRoot(WorkflowPolicy::ContinueOnError);
        QTest::newRow("DoneContinueOnError") << TestData{storage, root2, log, 1, OnDone::Success};

        const Group root3 = createRoot(WorkflowPolicy::StopOnDone);
        QTest::newRow("DoneStopOnDone") << TestData{storage, root3, log, 1, OnDone::Success};

        const Group root4 = createRoot(WorkflowPolicy::ContinueOnDone);
        QTest::newRow("DoneContinueOnDone") << TestData{storage, root4, log, 1, OnDone::Success};

        const Group root5 = createRoot(WorkflowPolicy::StopOnFinished);
        QTest::newRow("DoneStopOnFinished") << TestData{storage, root5, log, 1, OnDone::Success};

        const Group root6 = createRoot(WorkflowPolicy::Optional);
        QTest::newRow("DoneOptional") << TestData{storage, root6, log, 1, OnDone::Success};
    }

    {
        const auto createRoot = [storage, createFailingTask, groupDone, groupError](
                                    WorkflowPolicy policy) {
            return Group {
                Storage(storage),
                workflowPolicy(policy),
                createFailingTask(1),
                groupDone(0),
                groupError(0)
            };
        };

        const Log log = {
            {1, Handler::Setup},
            {1, Handler::Error},
            {0, Handler::GroupError}
        };

        const Log optionalLog = {
            {1, Handler::Setup},
            {1, Handler::Error},
            {0, Handler::GroupDone}
        };

        const Group root1 = createRoot(WorkflowPolicy::StopOnError);
        QTest::newRow("ErrorStopOnError") << TestData{storage, root1, log, 1, OnDone::Failure};

        const Group root2 = createRoot(WorkflowPolicy::ContinueOnError);
        QTest::newRow("ErrorContinueOnError") << TestData{storage, root2, log, 1, OnDone::Failure};

        const Group root3 = createRoot(WorkflowPolicy::StopOnDone);
        QTest::newRow("ErrorStopOnDone") << TestData{storage, root3, log, 1, OnDone::Failure};

        const Group root4 = createRoot(WorkflowPolicy::ContinueOnDone);
        QTest::newRow("ErrorContinueOnDone") << TestData{storage, root4, log, 1, OnDone::Failure};

        const Group root5 = createRoot(WorkflowPolicy::StopOnFinished);
        QTest::newRow("ErrorStopOnFinished") << TestData{storage, root5, log, 1, OnDone::Failure};

        const Group root6 = createRoot(WorkflowPolicy::Optional);
        QTest::newRow("ErrorOptional") << TestData{storage, root6, optionalLog, 1, OnDone::Success};
    }

    {
        const auto createRoot = [storage, createSuccessTask, createFailingTask, groupDone,
                                 groupError](WorkflowPolicy policy) {
            return Group {
                Storage(storage),
                workflowPolicy(policy),
                createSuccessTask(1),
                createFailingTask(2),
                createSuccessTask(3),
                groupDone(0),
                groupError(0)
            };
        };

        const Group root1 = createRoot(WorkflowPolicy::StopOnError);
        const Log log1 {
            {1, Handler::Setup},
            {1, Handler::Done},
            {2, Handler::Setup},
            {2, Handler::Error},
            {0, Handler::GroupError}
        };
        QTest::newRow("StopOnError") << TestData{storage, root1, log1, 3, OnDone::Failure};

        const Group root2 = createRoot(WorkflowPolicy::ContinueOnError);
        const Log log2 {
            {1, Handler::Setup},
            {1, Handler::Done},
            {2, Handler::Setup},
            {2, Handler::Error},
            {3, Handler::Setup},
            {3, Handler::Done},
            {0, Handler::GroupError}
        };
        QTest::newRow("ContinueOnError") << TestData{storage, root2, log2, 3, OnDone::Failure};

        const Group root3 = createRoot(WorkflowPolicy::StopOnDone);
        const Log log3 {
            {1, Handler::Setup},
            {1, Handler::Done},
            {0, Handler::GroupDone}
        };
        QTest::newRow("StopOnDone") << TestData{storage, root3, log3, 3, OnDone::Success};

        const Group root4 = createRoot(WorkflowPolicy::ContinueOnDone);
        const Log log4 {
            {1, Handler::Setup},
            {1, Handler::Done},
            {2, Handler::Setup},
            {2, Handler::Error},
            {3, Handler::Setup},
            {3, Handler::Done},
            {0, Handler::GroupDone}
        };
        QTest::newRow("ContinueOnDone") << TestData{storage, root4, log4, 3, OnDone::Success};

        const Group root5 = createRoot(WorkflowPolicy::StopOnFinished);
        const Log log5 {
            {1, Handler::Setup},
            {1, Handler::Done},
            {0, Handler::GroupDone}
        };
        QTest::newRow("StopOnFinished") << TestData{storage, root5, log5, 3, OnDone::Success};
    }

    {
        const auto createRoot = [storage, createTask, groupDone, groupError](
                                    bool firstSuccess, bool secondSuccess) {
            return Group {
                parallel,
                stopOnFinished,
                Storage(storage),
                createTask(1, firstSuccess, 1000ms),
                createTask(2, secondSuccess, 1ms),
                groupDone(0),
                groupError(0)
            };
        };

        const Group root1 = createRoot(true, true);
        const Group root2 = createRoot(true, false);
        const Group root3 = createRoot(false, true);
        const Group root4 = createRoot(false, false);

        const Log success {
            {1, Handler::Setup},
            {2, Handler::Setup},
            {2, Handler::Done},
            {1, Handler::Error},
            {0, Handler::GroupDone}
        };
        const Log failure {
            {1, Handler::Setup},
            {2, Handler::Setup},
            {2, Handler::Error},
            {1, Handler::Error},
            {0, Handler::GroupError}
        };

        QTest::newRow("StopOnFinished1") << TestData{storage, root1, success, 2, OnDone::Success};
        QTest::newRow("StopOnFinished2") << TestData{storage, root2, failure, 2, OnDone::Failure};
        QTest::newRow("StopOnFinished3") << TestData{storage, root3, success, 2, OnDone::Success};
        QTest::newRow("StopOnFinished4") << TestData{storage, root4, failure, 2, OnDone::Failure};
    }

    {
        const Group root {
            Storage(storage),
            optional,
            createFailingTask(1),
            createFailingTask(2),
            groupDone(0),
            groupError(0)
        };
        const Log log {
            {1, Handler::Setup},
            {1, Handler::Error},
            {2, Handler::Setup},
            {2, Handler::Error},
            {0, Handler::GroupDone}
        };
        QTest::newRow("Optional") << TestData{storage, root, log, 2, OnDone::Success};
    }

    {
        const auto createRoot = [storage, createSuccessTask, groupDone, groupError](
                                    TaskAction taskAction) {
            return Group {
                Storage(storage),
                Group {
                    createSuccessTask(1)
                },
                Group {
                    onGroupSetup([=] { return taskAction; }),
                    createSuccessTask(2),
                    createSuccessTask(3),
                    createSuccessTask(4)
                },
                groupDone(0),
                groupError(0)
            };
        };

        const Group root1 = createRoot(TaskAction::StopWithDone);
        const Log log1 {
            {1, Handler::Setup},
            {1, Handler::Done},
            {0, Handler::GroupDone}
        };
        QTest::newRow("DynamicSetupDone") << TestData{storage, root1, log1, 4, OnDone::Success};

        const Group root2 = createRoot(TaskAction::StopWithError);
        const Log log2 {
            {1, Handler::Setup},
            {1, Handler::Done},
            {0, Handler::GroupError}
        };
        QTest::newRow("DynamicSetupError") << TestData{storage, root2, log2, 4, OnDone::Failure};

        const Group root3 = createRoot(TaskAction::Continue);
        const Log log3 {
            {1, Handler::Setup},
            {1, Handler::Done},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Setup},
            {3, Handler::Done},
            {4, Handler::Setup},
            {4, Handler::Done},
            {0, Handler::GroupDone}
        };
        QTest::newRow("DynamicSetupContinue") << TestData{storage, root3, log3, 4, OnDone::Success};
    }

    {
        const Group root {
            parallelLimit(2),
            Storage(storage),
            Group {
                groupSetup(1),
                createSuccessTask(1)
            },
            Group {
                groupSetup(2),
                createSuccessTask(2)
            },
            Group {
                groupSetup(3),
                createSuccessTask(3)
            },
            Group {
                groupSetup(4),
                createSuccessTask(4)
            }
        };
        const Log log {
            {1, Handler::GroupSetup},
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {1, Handler::Done},
            {3, Handler::GroupSetup},
            {3, Handler::Setup},
            {2, Handler::Done},
            {4, Handler::GroupSetup},
            {4, Handler::Setup},
            {3, Handler::Done},
            {4, Handler::Done}
        };
        QTest::newRow("NestedParallel") << TestData{storage, root, log, 4, OnDone::Success};
    }

    {
        const Group root {
            parallelLimit(2),
            Storage(storage),
            Group {
                groupSetup(1),
                createSuccessTask(1)
            },
            Group {
                groupSetup(2),
                createSuccessTask(2)
            },
            Group {
                groupSetup(3),
                createDynamicTask(3, TaskAction::StopWithDone)
            },
            Group {
                groupSetup(4),
                createSuccessTask(4)
            },
            Group {
                groupSetup(5),
                createSuccessTask(5)
            }
        };
        const Log log {
            {1, Handler::GroupSetup},
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {1, Handler::Done},
            {3, Handler::GroupSetup},
            {3, Handler::Setup},
            {4, Handler::GroupSetup},
            {4, Handler::Setup},
            {2, Handler::Done},
            {5, Handler::GroupSetup},
            {5, Handler::Setup},
            {4, Handler::Done},
            {5, Handler::Done}
        };
        QTest::newRow("NestedParallelDone") << TestData{storage, root, log, 5, OnDone::Success};
    }

    {
        const Group root1 {
            parallelLimit(2),
            Storage(storage),
            Group {
                groupSetup(1),
                createSuccessTask(1)
            },
            Group {
                groupSetup(2),
                createSuccessTask(2)
            },
            Group {
                groupSetup(3),
                createDynamicTask(3, TaskAction::StopWithError)
            },
            Group {
                groupSetup(4),
                createSuccessTask(4)
            },
            Group {
                groupSetup(5),
                createSuccessTask(5)
            }
        };
        const Log log1 {
            {1, Handler::GroupSetup},
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {1, Handler::Done},
            {3, Handler::GroupSetup},
            {3, Handler::Setup},
            {2, Handler::Error}
        };

        // Inside this test the task 2 should finish first, then synchonously:
        // - task 3 should exit setup with error
        // - task 1 should be stopped as a consequence of the error inside the group
        // - tasks 4 and 5 should be skipped
        const Group root2 {
            parallelLimit(2),
            Storage(storage),
            Group {
                groupSetup(1),
                createSuccessTask(1, 10ms)
            },
            Group {
                groupSetup(2),
                createSuccessTask(2)
            },
            Group {
                groupSetup(3),
                createDynamicTask(3, TaskAction::StopWithError)
            },
            Group {
                groupSetup(4),
                createSuccessTask(4)
            },
            Group {
                groupSetup(5),
                createSuccessTask(5)
            }
        };
        const Log log2 {
            {1, Handler::GroupSetup},
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::GroupSetup},
            {3, Handler::Setup},
            {1, Handler::Error}
        };

        // This test ensures that the task 1 doesn't invoke its done handler,
        // being ready while sleeping in the task's 2 done handler.
        // Inside this test the task 2 should finish first, then synchonously:
        // - task 3 should exit setup with error
        // - task 1 should be stopped as a consequence of error inside the group
        // - task 4 should be skipped
        // - the first child group of root should finish with error
        // - task 5 should be started (because of root's continueOnError policy)
        const Group root3 {
            continueOnError,
            Storage(storage),
            Group {
                parallelLimit(2),
                Group {
                    groupSetup(1),
                    createSuccessTask(1, 10ms)
                },
                Group {
                    groupSetup(2),
                    createSuccessTask(2, 1ms)
                },
                Group {
                    groupSetup(3),
                    createDynamicTask(3, TaskAction::StopWithError)
                },
                Group {
                    groupSetup(4),
                    createSuccessTask(4)
                }
            },
            Group {
                groupSetup(5),
                createSuccessTask(5)
            }
        };
        const Log log3 {
            {1, Handler::GroupSetup},
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::GroupSetup},
            {3, Handler::Setup},
            {1, Handler::Error},
            {5, Handler::GroupSetup},
            {5, Handler::Setup},
            {5, Handler::Done}
        };
        QTest::newRow("NestedParallelError1")
            << TestData{storage, root1, log1, 5, OnDone::Failure};
        QTest::newRow("NestedParallelError2")
            << TestData{storage, root2, log2, 5, OnDone::Failure};
        QTest::newRow("NestedParallelError3")
            << TestData{storage, root3, log3, 5, OnDone::Failure};
    }

    {
        const Group root {
            parallelLimit(2),
            Storage(storage),
            Group {
                groupSetup(1),
                Group {
                    parallel,
                    createSuccessTask(1)
                }
            },
            Group {
                groupSetup(2),
                Group {
                    parallel,
                    createSuccessTask(2)
                }
            },
            Group {
                groupSetup(3),
                Group {
                    parallel,
                    createSuccessTask(3)
                }
            },
            Group {
                groupSetup(4),
                Group {
                    parallel,
                    createSuccessTask(4)
                }
            }
        };
        const Log log {
            {1, Handler::GroupSetup},
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {1, Handler::Done},
            {3, Handler::GroupSetup},
            {3, Handler::Setup},
            {2, Handler::Done},
            {4, Handler::GroupSetup},
            {4, Handler::Setup},
            {3, Handler::Done},
            {4, Handler::Done}
        };
        QTest::newRow("DeeplyNestedParallel") << TestData{storage, root, log, 4, OnDone::Success};
    }

    {
        const Group root {
            parallelLimit(2),
            Storage(storage),
            Group {
                groupSetup(1),
                Group { createSuccessTask(1) }
            },
            Group {
                groupSetup(2),
                Group { createSuccessTask(2) }
            },
            Group {
                groupSetup(3),
                Group { createDynamicTask(3, TaskAction::StopWithDone) }
            },
            Group {
                groupSetup(4),
                Group { createSuccessTask(4) }
            },
            Group {
                groupSetup(5),
                Group { createSuccessTask(5) }
            }
        };
        const Log log {
            {1, Handler::GroupSetup},
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {1, Handler::Done},
            {3, Handler::GroupSetup},
            {3, Handler::Setup},
            {4, Handler::GroupSetup},
            {4, Handler::Setup},
            {2, Handler::Done},
            {5, Handler::GroupSetup},
            {5, Handler::Setup},
            {4, Handler::Done},
            {5, Handler::Done}
        };
        QTest::newRow("DeeplyNestedParallelDone")
            << TestData{storage, root, log, 5, OnDone::Success};
    }

    {
        const Group root {
            parallelLimit(2),
            Storage(storage),
            Group {
                groupSetup(1),
                Group { createSuccessTask(1) }
            },
            Group {
                groupSetup(2),
                Group { createSuccessTask(2) }
            },
            Group {
                groupSetup(3),
                Group { createDynamicTask(3, TaskAction::StopWithError) }
            },
            Group {
                groupSetup(4),
                Group { createSuccessTask(4) }
            },
            Group {
                groupSetup(5),
                Group { createSuccessTask(5) }
            }
        };
        const Log log {
            {1, Handler::GroupSetup},
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {1, Handler::Done},
            {3, Handler::GroupSetup},
            {3, Handler::Setup},
            {2, Handler::Error}
        };
        QTest::newRow("DeeplyNestedParallelError")
            << TestData{storage, root, log, 5, OnDone::Failure};
    }

    {
        const Group root {
            Storage(storage),
            createSync(1),
            createSync(2),
            createSync(3),
            createSync(4),
            createSync(5)
        };
        const Log log {
            {1, Handler::Sync},
            {2, Handler::Sync},
            {3, Handler::Sync},
            {4, Handler::Sync},
            {5, Handler::Sync}
        };
        QTest::newRow("SyncSequential") << TestData{storage, root, log, 0, OnDone::Success};
    }

    {
        const Group root {
            Storage(storage),
            createSyncWithReturn(1, true),
            createSyncWithReturn(2, true),
            createSyncWithReturn(3, true),
            createSyncWithReturn(4, true),
            createSyncWithReturn(5, true)
        };
        const Log log {
            {1, Handler::Sync},
            {2, Handler::Sync},
            {3, Handler::Sync},
            {4, Handler::Sync},
            {5, Handler::Sync}
        };
        QTest::newRow("SyncWithReturn") << TestData{storage, root, log, 0, OnDone::Success};
    }

    {
        const Group root {
            Storage(storage),
            parallel,
            createSync(1),
            createSync(2),
            createSync(3),
            createSync(4),
            createSync(5)
        };
        const Log log {
            {1, Handler::Sync},
            {2, Handler::Sync},
            {3, Handler::Sync},
            {4, Handler::Sync},
            {5, Handler::Sync}
        };
        QTest::newRow("SyncParallel") << TestData{storage, root, log, 0, OnDone::Success};
    }

    {
        const Group root {
            Storage(storage),
            parallel,
            createSync(1),
            createSync(2),
            createSyncWithReturn(3, false),
            createSync(4),
            createSync(5)
        };
        const Log log {
            {1, Handler::Sync},
            {2, Handler::Sync},
            {3, Handler::Sync}
        };
        QTest::newRow("SyncError") << TestData{storage, root, log, 0, OnDone::Failure};
    }

    {
        const Group root {
            Storage(storage),
            createSync(1),
            createSuccessTask(2),
            createSync(3),
            createSuccessTask(4),
            createSync(5),
            groupDone(0)
        };
        const Log log {
            {1, Handler::Sync},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Sync},
            {4, Handler::Setup},
            {4, Handler::Done},
            {5, Handler::Sync},
            {0, Handler::GroupDone}
        };
        QTest::newRow("SyncAndAsync") << TestData{storage, root, log, 2, OnDone::Success};
    }

    {
        const Group root {
            Storage(storage),
            createSync(1),
            createSuccessTask(2),
            createSyncWithReturn(3, false),
            createSuccessTask(4),
            createSync(5),
            groupError(0)
        };
        const Log log {
            {1, Handler::Sync},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Sync},
            {0, Handler::GroupError}
        };
        QTest::newRow("SyncAndAsyncError") << TestData{storage, root, log, 2, OnDone::Failure};
    }

    {
        SingleBarrier barrier;

        // Test that barrier advance, triggered from inside the task described by
        // setupBarrierAdvance, placed BEFORE the group containing the waitFor() element
        // in the tree order, works OK in SEQUENTIAL mode.
        const Group root1 {
            Storage(storage),
            Storage(barrier),
            sequential,
            createBarrierAdvance(storage, barrier, 1),
            Group {
                groupSetup(2),
                WaitForBarrierTask(barrier),
                createSuccessTask(2),
                createSuccessTask(3)
            }
        };
        const Log log1 {
            {1, Handler::Setup},
            {1, Handler::BarrierAdvance},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Setup},
            {3, Handler::Done}
        };

        // Test that barrier advance, triggered from inside the task described by
        // setupTaskWithCondition, placed BEFORE the group containing the waitFor() element
        // in the tree order, works OK in PARALLEL mode.
        const Group root2 {
            Storage(storage),
            Storage(barrier),
            parallel,
            createBarrierAdvance(storage, barrier, 1),
            Group {
                groupSetup(2),
                WaitForBarrierTask(barrier),
                createSuccessTask(2),
                createSuccessTask(3)
            }
        };
        const Log log2 {
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {1, Handler::BarrierAdvance},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Setup},
            {3, Handler::Done}
        };

        // Test that barrier advance, triggered from inside the task described by
        // setupTaskWithCondition, placed AFTER the group containing the waitFor() element
        // in the tree order, works OK in PARALLEL mode.
        //
        // Notice: This won't work in SEQUENTIAL mode, since the advancing barrier, placed after the
        // group containing the WaitFor element, has no chance to be started in SEQUENTIAL mode,
        // as in SEQUENTIAL mode the next task may only be started after the previous one finished.
        // In this case, the previous task (Group element) awaits for the barrier's advance to
        // come from the not yet started next task, causing a deadlock.
        // The minimal requirement for this scenario to succeed is to set parallelLimit(2) or more.
        const Group root3 {
            Storage(storage),
            Storage(barrier),
            parallel,
            Group {
                groupSetup(2),
                WaitForBarrierTask(barrier),
                createSuccessTask(2),
                createSuccessTask(3)
            },
            createBarrierAdvance(storage, barrier, 1)
        };
        const Log log3 {
            {2, Handler::GroupSetup},
            {1, Handler::Setup},
            {1, Handler::BarrierAdvance},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Setup},
            {3, Handler::Done}
        };

        // Test that barrier advance, triggered from inside the task described by
        // setupBarrierAdvance, placed BEFORE the groups containing the waitFor() element
        // in the tree order, wakes both waitFor tasks.
        const Group root4 {
            Storage(storage),
            Storage(barrier),
            parallel,
            createBarrierAdvance(storage, barrier, 1),
            Group {
                groupSetup(2),
                WaitForBarrierTask(barrier),
                createSuccessTask(4)
            },
            Group {
                groupSetup(3),
                WaitForBarrierTask(barrier),
                createSuccessTask(5)
            }
        };
        const Log log4 {
            {1, Handler::Setup},
            {2, Handler::GroupSetup},
            {3, Handler::GroupSetup},
            {1, Handler::BarrierAdvance},
            {4, Handler::Setup},
            {5, Handler::Setup},
            {4, Handler::Done},
            {5, Handler::Done}
        };

        // Test two separate single barriers.

        SingleBarrier barrier2;

        const Group root5 {
            Storage(storage),
            Storage(barrier),
            Storage(barrier2),
            parallel,
            createBarrierAdvance(storage, barrier, 1),
            createBarrierAdvance(storage, barrier2, 2),
            Group {
                Group {
                    parallel,
                    groupSetup(1),
                    WaitForBarrierTask(barrier),
                    WaitForBarrierTask(barrier2)
                },
                createSuccessTask(3)
            },
        };
        const Log log5 {
            {1, Handler::Setup},
            {2, Handler::Setup},
            {1, Handler::GroupSetup},
            {1, Handler::BarrierAdvance},
            {2, Handler::BarrierAdvance},
            {3, Handler::Setup},
            {3, Handler::Done}
        };

        // Notice the different log order for each scenario.
        QTest::newRow("BarrierSequential")
            << TestData{storage, root1, log1, 4, OnDone::Success};
        QTest::newRow("BarrierParallelAdvanceFirst")
            << TestData{storage, root2, log2, 4, OnDone::Success};
        QTest::newRow("BarrierParallelWaitForFirst")
            << TestData{storage, root3, log3, 4, OnDone::Success};
        QTest::newRow("BarrierParallelMultiWaitFor")
            << TestData{storage, root4, log4, 5, OnDone::Success};
        QTest::newRow("BarrierParallelTwoSingleBarriers")
            << TestData{storage, root5, log5, 5, OnDone::Success};
    }

    {
        MultiBarrier<2> barrier;

        // Test that multi barrier advance, triggered from inside the tasks described by
        // setupBarrierAdvance, placed BEFORE the group containing the waitFor() element
        // in the tree order, works OK in SEQUENTIAL mode.
        const Group root1 {
            Storage(storage),
            Storage(barrier),
            sequential,
            createBarrierAdvance(storage, barrier, 1),
            createBarrierAdvance(storage, barrier, 2),
            Group {
                groupSetup(2),
                WaitForBarrierTask(barrier),
                createSuccessTask(2),
                createSuccessTask(3)
            }
        };
        const Log log1 {
            {1, Handler::Setup},
            {1, Handler::BarrierAdvance},
            {2, Handler::Setup},
            {2, Handler::BarrierAdvance},
            {2, Handler::GroupSetup},
            {2, Handler::Setup},
            {2, Handler::Done},
            {3, Handler::Setup},
            {3, Handler::Done}
        };

        // Test that multi barrier advance, triggered from inside the tasks described by
        // setupBarrierAdvance, placed BEFORE the group containing the waitFor() element
        // in the tree order, works OK in PARALLEL mode.
        const Group root2 {
            Storage(storage),
            Storage(barrier),
            parallel,
            createBarrierAdvance(storage, barrier, 1),
            createBarrierAdvance(storage, barrier, 2),
            Group {
                groupSetup(2),
                WaitForBarrierTask(barrier),
                createSuccessTask(3),
                createSuccessTask(4)
            }
        };
        const Log log2 {
            {1, Handler::Setup},
            {2, Handler::Setup},
            {2, Handler::GroupSetup},
            {1, Handler::BarrierAdvance},
            {2, Handler::BarrierAdvance},
            {3, Handler::Setup},
            {3, Handler::Done},
            {4, Handler::Setup},
            {4, Handler::Done}
        };

        // Test that multi barrier advance, triggered from inside the tasks described by
        // setupBarrierAdvance, placed AFTER the group containing the waitFor() element
        // in the tree order, works OK in PARALLEL mode.
        //
        // Notice: This won't work in SEQUENTIAL mode, since the advancing barriers, placed after
        // the group containing the WaitFor element, has no chance to be started in SEQUENTIAL mode,
        // as in SEQUENTIAL mode the next task may only be started after the previous one finished.
        // In this case, the previous task (Group element) awaits for the barrier's advance to
        // come from the not yet started next task, causing a deadlock.
        // The minimal requirement for this scenario to succeed is to set parallelLimit(2) or more.
        const Group root3 {
            Storage(storage),
            Storage(barrier),
            parallel,
            Group {
                groupSetup(2),
                WaitForBarrierTask(barrier),
                createSuccessTask(3),
                createSuccessTask(4)
            },
            createBarrierAdvance(storage, barrier, 1),
            createBarrierAdvance(storage, barrier, 2)
        };
        const Log log3 {
            {2, Handler::GroupSetup},
            {1, Handler::Setup},
            {2, Handler::Setup},
            {1, Handler::BarrierAdvance},
            {2, Handler::BarrierAdvance},
            {3, Handler::Setup},
            {3, Handler::Done},
            {4, Handler::Setup},
            {4, Handler::Done}
        };

        // Test that multi barrier advance, triggered from inside the task described by
        // setupBarrierAdvance, placed BEFORE the groups containing the waitFor() element
        // in the tree order, wakes both waitFor tasks.
        const Group root4 {
            Storage(storage),
            Storage(barrier),
            parallel,
            createBarrierAdvance(storage, barrier, 1),
            createBarrierAdvance(storage, barrier, 2),
            Group {
                groupSetup(2),
                WaitForBarrierTask(barrier),
                createSuccessTask(3)
            },
            Group {
                groupSetup(3),
                WaitForBarrierTask(barrier),
                createSuccessTask(4)
            }
        };
        const Log log4 {
            {1, Handler::Setup},
            {2, Handler::Setup},
            {2, Handler::GroupSetup},
            {3, Handler::GroupSetup},
            {1, Handler::BarrierAdvance},
            {2, Handler::BarrierAdvance},
            {3, Handler::Setup},
            {4, Handler::Setup},
            {3, Handler::Done},
            {4, Handler::Done}
        };

        // Notice the different log order for each scenario.
        QTest::newRow("MultiBarrierSequential")
            << TestData{storage, root1, log1, 5, OnDone::Success};
        QTest::newRow("MultiBarrierParallelAdvanceFirst")
            << TestData{storage, root2, log2, 5, OnDone::Success};
        QTest::newRow("MultiBarrierParallelWaitForFirst")
            << TestData{storage, root3, log3, 5, OnDone::Success};
        QTest::newRow("MultiBarrierParallelMultiWaitFor")
            << TestData{storage, root4, log4, 6, OnDone::Success};
    }
}

void tst_Tasking::testTree()
{
    QFETCH(TestData, testData);

    TaskTree taskTree(testData.root);
    QCOMPARE(taskTree.taskCount(), testData.taskCount);
    Log actualLog;
    const auto collectLog = [&actualLog](CustomStorage *storage) { actualLog = storage->m_log; };
    taskTree.onStorageDone(testData.storage, collectLog);
    const OnDone result = taskTree.runBlocking(2000) ? OnDone::Success : OnDone::Failure;
    QCOMPARE(taskTree.isRunning(), false);

    QCOMPARE(taskTree.progressValue(), testData.taskCount);
    QCOMPARE(actualLog, testData.expectedLog);
    QCOMPARE(CustomStorage::instanceCount(), 0);

    QCOMPARE(result, testData.onDone);
}

void tst_Tasking::storageOperators()
{
    TreeStorageBase storage1 = TreeStorage<CustomStorage>();
    TreeStorageBase storage2 = TreeStorage<CustomStorage>();
    TreeStorageBase storage3 = storage1;

    QVERIFY(storage1 == storage3);
    QVERIFY(storage1 != storage2);
    QVERIFY(storage2 != storage3);
}

// This test checks whether a running task tree may be safely destructed.
// It also checks whether the destructor of a task tree deletes properly the storage created
// while starting the task tree. When running task tree is destructed, the storage done
// handler shouldn't be invoked.
void tst_Tasking::storageDestructor()
{
    bool setupCalled = false;
    const auto setupHandler = [&setupCalled](CustomStorage *) {
        setupCalled = true;
    };
    bool doneCalled = false;
    const auto doneHandler = [&doneCalled](CustomStorage *) {
        doneCalled = true;
    };
    QCOMPARE(CustomStorage::instanceCount(), 0);
    {
        TreeStorage<CustomStorage> storage;
        const auto setupSleepingTask = [](TaskObject &taskObject) {
            taskObject = 1000ms;
        };
        const Group root {
            Storage(storage),
            SuccessTask(setupSleepingTask)
        };

        TaskTree taskTree(root);
        QCOMPARE(CustomStorage::instanceCount(), 0);
        taskTree.onStorageSetup(storage, setupHandler);
        taskTree.onStorageDone(storage, doneHandler);
        taskTree.start();
        QCOMPARE(CustomStorage::instanceCount(), 1);
    }
    QCOMPARE(CustomStorage::instanceCount(), 0);
    QVERIFY(setupCalled);
    QVERIFY(!doneCalled);
}

QTEST_GUILESS_MAIN(tst_Tasking)

#include "tst_tasking.moc"
