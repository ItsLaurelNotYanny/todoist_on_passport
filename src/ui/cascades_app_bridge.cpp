#include "todoist/ui/cascades_app_bridge.hpp"
#include "todoist/uuid_util.hpp"

namespace todoist {

CascadesAppBridge::CascadesAppBridge(const QString& dbPath,
                                     const QString& caBundlePath,
                                     QObject* parent)
    : QObject(parent)
    , m_dbPath(dbPath)
    , m_db()
    , m_transport(caBundlePath.toUtf8().constData())
    , m_controller(m_db, m_transport, &m_transport)
    , m_taskModel(this)
    , m_qtTaskModel(this)
    , m_projectModel(this)
    , m_coverTaskModel(this)
    , m_tickTimer(this)
    , m_netManager(this)
{
    m_tickTimer.setInterval(150);
    connect(&m_tickTimer, SIGNAL(timeout()), this, SLOT(onTimerTick()));
    connect(&m_netManager, SIGNAL(onlineStateChanged(bool)), this, SLOT(onOnlineStateChanged(bool)));
    connect(&m_netManager, SIGNAL(configurationChanged(const QNetworkConfiguration&)), this, SLOT(onConfigurationChanged(const QNetworkConfiguration&)));
}

CascadesAppBridge::~CascadesAppBridge() {
    m_tickTimer.stop();
    m_controller.shutdown();
    m_db.close();
}

uint64_t CascadesAppBridge::currentEpochMs() {
    return static_cast<uint64_t>(util::currentEpochMs());
}

void CascadesAppBridge::initialize() {
    m_db.open(m_dbPath.toUtf8().constData());
    m_controller.initialize(currentEpochMs(), true);
    updateNetworkReachability();
    syncModelsAndNotify();
    m_tickTimer.start();
}

void CascadesAppBridge::updateNetworkReachability() {
    if (!m_netManager.isOnline()) {
        m_controller.setNetworkConnectionType(NetworkConnectionType::Offline, currentEpochMs());
    } else {
        QNetworkConfiguration config = m_netManager.defaultConfiguration();
        QNetworkConfiguration::BearerType bearer = config.bearerType();
        if (bearer == QNetworkConfiguration::BearerWLAN || bearer == QNetworkConfiguration::BearerEthernet) {
            m_controller.setNetworkConnectionType(NetworkConnectionType::WiFi, currentEpochMs());
        } else {
            m_controller.setNetworkConnectionType(NetworkConnectionType::Cellular, currentEpochMs());
        }
    }
}

void CascadesAppBridge::onOnlineStateChanged(bool) {
    updateNetworkReachability();
    syncModelsAndNotify();
}

void CascadesAppBridge::onConfigurationChanged(const QNetworkConfiguration&) {
    updateNetworkReachability();
    syncModelsAndNotify();
}

void CascadesAppBridge::onFullscreen() {
    updateNetworkReachability();
    m_controller.setLifecycleState(AppLifecycleState::Foreground, currentEpochMs());
    m_tickTimer.setInterval(150);
    syncModelsAndNotify();
}

void CascadesAppBridge::onThumbnail() {
    m_controller.setLifecycleState(AppLifecycleState::ActiveFrameThumbnail, currentEpochMs());
    m_tickTimer.setInterval(1000);
    syncModelsAndNotify();
}

void CascadesAppBridge::onInvisible() {
    m_controller.setLifecycleState(AppLifecycleState::InvisibleBackground, currentEpochMs());
    m_tickTimer.setInterval(2000);
    syncModelsAndNotify();
}

void CascadesAppBridge::onAboutToQuit() {
    m_tickTimer.stop();
    m_controller.shutdown();
    m_db.close();
}

void CascadesAppBridge::onTimerTick() {
    int actions = m_controller.tick(currentEpochMs());
    if (actions > 0) {
        syncModelsAndNotify();
    }
}

QVariantList CascadesAppBridge::availableProjects() const {
    QVariantList list;
    const std::vector<std::string>& projs = m_controller.getAvailableProjects();
    for (size_t i = 0; i < projs.size(); ++i) {
        list.append(QString::fromUtf8(projs[i].c_str()));
    }
    return list;
}

QString CascadesAppBridge::filterMode() const {
    return QString::fromUtf8(m_controller.getFilterModeString().c_str());
}

QString CascadesAppBridge::activeProject() const {
    return QString::fromUtf8(m_controller.getActiveProjectId().c_str());
}

QString CascadesAppBridge::searchQuery() const {
    return QString::fromUtf8(m_controller.getSearchQuery().c_str());
}

QVariantMap CascadesAppBridge::selectedTask() const {
    const TaskViewItem* sel = m_controller.getSelectedTask();
    if (!sel) {
        return QVariantMap();
    }
    return CascadesTaskListModel::taskToVariantMap(*sel, m_controller.getSelectedIndex());
}

QVariantMap CascadesAppBridge::detailTask() const {
    return CascadesTaskListModel::taskToVariantMap(m_controller.getDetailTask(),
                                                   m_controller.getSelectedIndex());
}

QString CascadesAppBridge::maskedToken() const {
    return QString::fromUtf8(m_controller.getMaskedApiToken().c_str());
}

QString CascadesAppBridge::statusText() const {
    return QString::fromUtf8(m_controller.getStatusText().c_str());
}

QString CascadesAppBridge::errorText() const {
    return QString::fromUtf8(m_controller.getLastErrorText().c_str());
}

QString CascadesAppBridge::networkStateText() const {
    switch (m_controller.getNetworkConnectionType()) {
        case NetworkConnectionType::WiFi:
            return QString::fromUtf8("Wi-Fi");
        case NetworkConnectionType::Cellular:
            return QString::fromUtf8("Cellular");
        case NetworkConnectionType::Offline:
            return QString::fromUtf8("Offline");
        default:
            return QString::fromUtf8("Unknown");
    }
}

QString CascadesAppBridge::coverHeadline() const {
    return QString::fromUtf8(m_controller.getActiveFrameSnapshot().statusHeadline.c_str());
}

bool CascadesAppBridge::createTask(const QString& content,
                                   const QString& description,
                                   int priority,
                                   const QString& dueDate,
                                   const QString& projectId) {
    bool ok = m_controller.createTask(content.toUtf8().constData(),
                                      description.toUtf8().constData(),
                                      priority,
                                      dueDate.toUtf8().constData(),
                                      projectId.toUtf8().constData(),
                                      currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::updateTask(const QString& taskId,
                                   const QString& content,
                                   const QString& description,
                                   int priority,
                                   const QString& dueDate) {
    bool ok = m_controller.updateTask(taskId.toUtf8().constData(),
                                      content.toUtf8().constData(),
                                      description.toUtf8().constData(),
                                      priority,
                                      dueDate.toUtf8().constData(),
                                      currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::toggleTaskCompleted(const QString& taskId) {
    bool ok = m_controller.toggleTaskCompleted(taskId.toUtf8().constData(), currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::toggleSelectedTaskCompleted() {
    bool ok = m_controller.toggleSelectedTaskCompleted(currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::deleteTask(const QString& taskId) {
    bool ok = m_controller.deleteTask(taskId.toUtf8().constData(), currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::deleteSelectedTask() {
    bool ok = m_controller.deleteSelectedTask(currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::selectTask(int index) {
    bool ok = m_controller.selectTaskByIndex(index);
    syncModelsAndNotify();
    return ok;
}

void CascadesAppBridge::selectProject(const QString& projectId) {
    std::string resolved = m_controller.resolveProjectId(projectId.toUtf8().constData());
    m_controller.setFilter(FilterMode::Project, resolved);
    syncModelsAndNotify();
}

void CascadesAppBridge::refreshProjects() {
    syncModelsAndNotify();
}

QString CascadesAppBridge::projectDisplayName(const QString& projectId) {
    return QString::fromUtf8(m_controller.getProjectDisplayName(projectId.toUtf8().constData()).c_str());
}

bool CascadesAppBridge::isInboxProject(const QString& projectId) const {
    return m_controller.isInboxProject(projectId.toUtf8().constData());
}

bool CascadesAppBridge::setInboxProjectId(const QString& id) {
    bool ok = m_controller.setInboxProjectId(id.toUtf8().constData());
    if (ok) {
        syncModelsAndNotify();
        emit inboxProjectChanged();
    } else {
        emit statusChanged();
    }
    return ok;
}

void CascadesAppBridge::setFilter(const QString& mode, const QString& projectId) {
    m_controller.setFilterByName(mode.toUtf8().constData(), projectId.toUtf8().constData());
    syncModelsAndNotify();
}

void CascadesAppBridge::setSearchQuery(const QString& query) {
    m_controller.setSearchQuery(query.toUtf8().constData());
    syncModelsAndNotify();
}

void CascadesAppBridge::openCreateSheet() {
    m_controller.openCreateSheet();
    emit sheetsChanged();
}

bool CascadesAppBridge::openSelectedTaskDetail() {
    bool ok = m_controller.openSelectedTaskDetail();
    emit sheetsChanged();
    return ok;
}

bool CascadesAppBridge::openTaskDetail(const QString& taskId) {
    bool ok = m_controller.openTaskDetailById(taskId.toUtf8().constData());
    syncModelsAndNotify();
    return ok;
}

void CascadesAppBridge::openSettingsSheet() {
    m_controller.openSettingsSheet();
    emit sheetsChanged();
}

void CascadesAppBridge::closeSheets() {
    m_controller.closeSheets(currentEpochMs());
    syncModelsAndNotify();
}

bool CascadesAppBridge::saveApiToken(const QString& token) {
    bool ok = m_controller.setApiToken(token.toUtf8().constData(), currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::clearApiToken() {
    bool ok = m_controller.clearApiToken();
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::triggerSync() {
    bool ok = m_controller.triggerSyncNow(currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::triggerFullSync() {
    bool ok = m_controller.triggerFullSync(currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

bool CascadesAppBridge::handleShortcut(const QString& key) {
    bool ok = m_controller.handleKeyShortcut(key.toUtf8().constData(), currentEpochMs());
    syncModelsAndNotify();
    return ok;
}

void CascadesAppBridge::syncModelsAndNotify() {
    const std::vector<TaskViewItem>& visible = m_controller.getVisibleTasks();
    m_taskModel.setTasks(visible);
    m_qtTaskModel.setTasks(visible);

    m_projectModel.clear();
    const std::vector<std::string>& projects = m_controller.getAvailableProjects();
    for (size_t i = 0; i < projects.size(); ++i) {
        std::string disp = m_controller.getProjectDisplayName(projects[i]);
        m_projectModel.append(QString::fromUtf8(disp.c_str()));
    }

    m_coverTaskModel.clear();
    const ActiveFrameSnapshot& snap = m_controller.getActiveFrameSnapshot();
    for (size_t i = 0; i < snap.topTasks.size(); ++i) {
        m_coverTaskModel.append(CascadesTaskListModel::taskToVariantMap(
            snap.topTasks[i], static_cast<int>(i)));
    }

    emit filterChanged();
    emit selectionChanged();
    emit sheetsChanged();
    emit statusChanged();
    emit authChanged();
    emit countsChanged();
    emit coverChanged();
}

} // namespace todoist
