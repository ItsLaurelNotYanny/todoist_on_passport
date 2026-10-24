#include "todoist/ui/cascades_task_list_model.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QHash>

namespace todoist {

CascadesTaskListModel::CascadesTaskListModel(QObject* parent)
    : bb::cascades::DataModel(parent)
{
}

CascadesTaskListModel::~CascadesTaskListModel() {
}

QVariantMap CascadesTaskListModel::taskToVariantMap(const TaskViewItem& item, int rowIndex) {
    QVariantMap map;
    map.insert("id", QString::fromUtf8(item.id.c_str()));
    map.insert("serverId", QString::fromUtf8(item.serverId.c_str()));
    map.insert("content", QString::fromUtf8(item.content.c_str()));
    map.insert("description", QString::fromUtf8(item.description.c_str()));
    map.insert("isCompleted", item.isCompleted);
    map.insert("priority", item.priority);
    map.insert("priorityLabel", QString::fromUtf8(item.priorityLabel().c_str()));
    map.insert("priorityColor", QString::fromUtf8(item.priorityColorHex().c_str()));
    map.insert("dueDate", QString::fromUtf8(item.dueDate.c_str()));
    map.insert("projectId", QString::fromUtf8(item.projectId.c_str()));
    map.insert("projectDisplayName", QString::fromUtf8(item.projectDisplayName.c_str()));
    map.insert("syncBadge", QString::fromUtf8(item.syncBadgeText.c_str()));
    map.insert("isSelected", item.isSelected);
    map.insert("index", rowIndex);
    return map;
}

int CascadesTaskListModel::childCount(const QVariantList& indexPath) {
    if (indexPath.isEmpty()) {
        return static_cast<int>(m_tasks.size());
    }
    return 0;
}

bool CascadesTaskListModel::hasChildren(const QVariantList& indexPath) {
    if (indexPath.isEmpty()) {
        return !m_tasks.empty();
    }
    return false;
}

QString CascadesTaskListModel::itemType(const QVariantList& indexPath) {
    (void)indexPath;
    return QString("task");
}

QVariant CascadesTaskListModel::data(const QVariantList& indexPath) {
    if (indexPath.size() != 1) {
        return QVariant();
    }
    int row = indexPath.at(0).toInt();
    if (row < 0 || row >= static_cast<int>(m_tasks.size())) {
        return QVariant();
    }
    return taskToVariantMap(m_tasks[static_cast<size_t>(row)], row);
}

void CascadesTaskListModel::setTasks(const std::vector<TaskViewItem>& tasks) {
    bool sameStructure = (m_tasks.size() == tasks.size());
    if (sameStructure) {
        for (size_t i = 0; i < tasks.size(); ++i) {
            if (m_tasks[i].id != tasks[i].id) {
                sameStructure = false;
                break;
            }
        }
    }
    m_tasks = tasks;
    if (sameStructure) {
        emit itemsChanged(bb::cascades::DataModelChangeType::Update);
    } else {
        emit itemsChanged(bb::cascades::DataModelChangeType::Init);
    }
}

QtTaskListModel::QtTaskListModel(QObject* parent)
    : QAbstractListModel(parent)
{
    QHash<int, QByteArray> roles;
    roles[IdRole] = "id";
    roles[ServerIdRole] = "serverId";
    roles[ContentRole] = "content";
    roles[DescriptionRole] = "description";
    roles[CompletedRole] = "isCompleted";
    roles[PriorityRole] = "priority";
    roles[PriorityLabelRole] = "priorityLabel";
    roles[PriorityColorRole] = "priorityColor";
    roles[DueDateRole] = "dueDate";
    roles[ProjectIdRole] = "projectId";
    roles[SyncBadgeRole] = "syncBadge";
    roles[SelectedRole] = "isSelected";
    setRoleNames(roles);
}

QtTaskListModel::~QtTaskListModel() {
}

int QtTaskListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_tasks.size());
}

QVariant QtTaskListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_tasks.size())) {
        return QVariant();
    }
    const TaskViewItem& item = m_tasks[static_cast<size_t>(index.row())];
    switch (role) {
        case Qt::DisplayRole:
        case ContentRole:
            return QString::fromUtf8(item.content.c_str());
        case IdRole:
            return QString::fromUtf8(item.id.c_str());
        case ServerIdRole:
            return QString::fromUtf8(item.serverId.c_str());
        case DescriptionRole:
            return QString::fromUtf8(item.description.c_str());
        case CompletedRole:
            return item.isCompleted;
        case PriorityRole:
            return item.priority;
        case PriorityLabelRole:
            return QString::fromUtf8(item.priorityLabel().c_str());
        case PriorityColorRole:
            return QString::fromUtf8(item.priorityColorHex().c_str());
        case DueDateRole:
            return QString::fromUtf8(item.dueDate.c_str());
        case ProjectIdRole:
            return QString::fromUtf8(item.projectId.c_str());
        case SyncBadgeRole:
            return QString::fromUtf8(item.syncBadgeText.c_str());
        case SelectedRole:
            return item.isSelected;
        default:
            return QVariant();
    }
}

void QtTaskListModel::setTasks(const std::vector<TaskViewItem>& tasks) {
    beginResetModel();
    m_tasks = tasks;
    endResetModel();
}

} // namespace todoist
