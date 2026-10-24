#ifndef TODOIST_CASCADES_APP_BRIDGE_HPP
#define TODOIST_CASCADES_APP_BRIDGE_HPP

#include "todoist/ui/cascades_task_list_model.hpp"
#include "todoist/ui/todoist_app_controller.hpp"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtNetwork/QNetworkConfiguration>
#include <QtNetwork/QNetworkConfigurationManager>
#include <bb/cascades/ArrayDataModel>

namespace todoist {

class CascadesAppBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bb::cascades::DataModel* taskModel READ taskModel CONSTANT)
    Q_PROPERTY(QAbstractListModel* qtTaskModel READ qtTaskModel CONSTANT)
    Q_PROPERTY(bb::cascades::DataModel* projectModel READ projectModel CONSTANT)
    Q_PROPERTY(bb::cascades::DataModel* coverTaskModel READ coverTaskModel CONSTANT)
    Q_PROPERTY(QVariantList availableProjects READ availableProjects NOTIFY filterChanged)
    Q_PROPERTY(QString filterMode READ filterMode NOTIFY filterChanged)
    Q_PROPERTY(QString activeProject READ activeProject NOTIFY filterChanged)
    Q_PROPERTY(QString inboxProjectId READ inboxProjectId WRITE setInboxProjectId NOTIFY inboxProjectChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY filterChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedTask READ selectedTask NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap detailTask READ detailTask NOTIFY sheetsChanged)
    Q_PROPERTY(bool createSheetOpen READ createSheetOpen NOTIFY sheetsChanged)
    Q_PROPERTY(bool detailSheetOpen READ detailSheetOpen NOTIFY sheetsChanged)
    Q_PROPERTY(bool settingsSheetOpen READ settingsSheetOpen NOTIFY sheetsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged)
    Q_PROPERTY(bool hasToken READ hasToken NOTIFY authChanged)
    Q_PROPERTY(bool requiresAuth READ requiresAuth NOTIFY authChanged)
    Q_PROPERTY(QString maskedToken READ maskedToken NOTIFY authChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY statusChanged)
    Q_PROPERTY(QString networkStateText READ networkStateText NOTIFY statusChanged)
    Q_PROPERTY(int totalIncompleteCount READ totalIncompleteCount NOTIFY countsChanged)
    Q_PROPERTY(int todayIncompleteCount READ todayIncompleteCount NOTIFY countsChanged)
    Q_PROPERTY(int completedCount READ completedCount NOTIFY countsChanged)
    Q_PROPERTY(int pendingSyncCount READ pendingSyncCount NOTIFY countsChanged)
    Q_PROPERTY(int deadLetterCount READ deadLetterCount NOTIFY countsChanged)
    Q_PROPERTY(QString coverHeadline READ coverHeadline NOTIFY coverChanged)

public:
    CascadesAppBridge(const QString& dbPath,
                      const QString& caBundlePath,
                      QObject* parent = 0);
    virtual ~CascadesAppBridge();

    bb::cascades::DataModel* taskModel() { return &m_taskModel; }
    QAbstractListModel* qtTaskModel() { return &m_qtTaskModel; }
    bb::cascades::DataModel* projectModel() { return &m_projectModel; }
    bb::cascades::DataModel* coverTaskModel() { return &m_coverTaskModel; }

    QVariantList availableProjects() const;
    QString filterMode() const;
    QString activeProject() const;
    QString inboxProjectId() const { return QString::fromUtf8(m_controller.getInboxProjectId().c_str()); }
    QString searchQuery() const;
    int selectedIndex() const { return m_controller.getSelectedIndex(); }
    QVariantMap selectedTask() const;
    QVariantMap detailTask() const;

    bool createSheetOpen() const { return m_controller.isCreateSheetOpen(); }
    bool detailSheetOpen() const { return m_controller.isDetailSheetOpen(); }
    bool settingsSheetOpen() const { return m_controller.isSettingsSheetOpen(); }

    bool busy() const { return m_controller.isSyncing(); }
    bool hasToken() const { return m_controller.hasApiToken(); }
    bool requiresAuth() const { return m_controller.requiresAuth(); }
    QString maskedToken() const;
    QString statusText() const;
    QString errorText() const;
    QString networkStateText() const;

    int totalIncompleteCount() const { return m_controller.getTotalIncompleteCount(); }
    int todayIncompleteCount() const { return m_controller.getTodayIncompleteCount(); }
    int completedCount() const { return m_controller.getCompletedCount(); }
    int pendingSyncCount() const { return m_controller.getPendingCommandCount(); }
    int deadLetterCount() const { return m_controller.getDeadLetterCount(); }
    QString coverHeadline() const;

    Q_INVOKABLE bool createTask(const QString& content,
                                const QString& description,
                                int priority,
                                const QString& dueDate,
                                const QString& projectId);
    Q_INVOKABLE bool updateTask(const QString& taskId,
                                const QString& content,
                                const QString& description,
                                int priority,
                                const QString& dueDate);
    Q_INVOKABLE bool toggleTaskCompleted(const QString& taskId);
    Q_INVOKABLE bool toggleSelectedTaskCompleted();
    Q_INVOKABLE bool deleteTask(const QString& taskId);
    Q_INVOKABLE bool deleteSelectedTask();

    Q_INVOKABLE bool selectTask(int index);
    Q_INVOKABLE void selectProject(const QString& projectId);
    Q_INVOKABLE void refreshProjects();
    Q_INVOKABLE QString projectDisplayName(const QString& projectId);
    Q_INVOKABLE bool isInboxProject(const QString& projectId) const;
    Q_INVOKABLE bool setInboxProjectId(const QString& id);
    Q_INVOKABLE void setFilter(const QString& mode, const QString& projectId = QString());
    Q_INVOKABLE void setSearchQuery(const QString& query);

    Q_INVOKABLE void openCreateSheet();
    Q_INVOKABLE bool openSelectedTaskDetail();
    Q_INVOKABLE bool openTaskDetail(const QString& taskId);
    Q_INVOKABLE void openSettingsSheet();
    Q_INVOKABLE void closeSheets();

    Q_INVOKABLE bool saveApiToken(const QString& token);
    Q_INVOKABLE bool clearApiToken();
    Q_INVOKABLE bool triggerSync();
    Q_INVOKABLE bool triggerFullSync();
    Q_INVOKABLE bool handleShortcut(const QString& key);

public slots:
    void initialize();
    void updateNetworkReachability();
    void onFullscreen();
    void onThumbnail();
    void onInvisible();
    void onAboutToQuit();

signals:
    void filterChanged();
    void inboxProjectChanged();
    void selectionChanged();
    void sheetsChanged();
    void statusChanged();
    void authChanged();
    void countsChanged();
    void coverChanged();

private slots:
    void onTimerTick();
    void onOnlineStateChanged(bool isOnline);
    void onConfigurationChanged(const QNetworkConfiguration& config);

private:
    void syncModelsAndNotify();
    static uint64_t currentEpochMs();

    QString m_dbPath;
    DatabaseManager m_db;
    CurlSyncTransport m_transport;
    TodoistAppController m_controller;
    CascadesTaskListModel m_taskModel;
    QtTaskListModel m_qtTaskModel;
    bb::cascades::ArrayDataModel m_projectModel;
    bb::cascades::ArrayDataModel m_coverTaskModel;
    QTimer m_tickTimer;
    QNetworkConfigurationManager m_netManager;
};

} // namespace todoist

#endif // TODOIST_CASCADES_APP_BRIDGE_HPP
