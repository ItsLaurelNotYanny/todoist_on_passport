#ifndef TODOIST_CASCADES_TASK_LIST_MODEL_HPP
#define TODOIST_CASCADES_TASK_LIST_MODEL_HPP

#include "todoist/ui/ui_types.hpp"

#include <QtCore/QAbstractListModel>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <bb/cascades/DataModel>
#include <vector>

namespace todoist {

class CascadesTaskListModel : public bb::cascades::DataModel {
    Q_OBJECT
public:
    explicit CascadesTaskListModel(QObject* parent = 0);
    virtual ~CascadesTaskListModel();

    virtual int childCount(const QVariantList& indexPath);
    virtual bool hasChildren(const QVariantList& indexPath);
    virtual QString itemType(const QVariantList& indexPath);
    virtual QVariant data(const QVariantList& indexPath);

    void setTasks(const std::vector<TaskViewItem>& tasks);
    int size() const { return static_cast<int>(m_tasks.size()); }

    static QVariantMap taskToVariantMap(const TaskViewItem& item, int rowIndex);

private:
    std::vector<TaskViewItem> m_tasks;
};

class QtTaskListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum TaskRoles {
        IdRole = Qt::UserRole + 1,
        ServerIdRole,
        ContentRole,
        DescriptionRole,
        CompletedRole,
        PriorityRole,
        PriorityLabelRole,
        PriorityColorRole,
        DueDateRole,
        ProjectIdRole,
        SyncBadgeRole,
        SelectedRole
    };

    explicit QtTaskListModel(QObject* parent = 0);
    virtual ~QtTaskListModel();

    virtual int rowCount(const QModelIndex& parent = QModelIndex()) const;
    virtual QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const;

    void setTasks(const std::vector<TaskViewItem>& tasks);

private:
    std::vector<TaskViewItem> m_tasks;
};

} // namespace todoist

#endif // TODOIST_CASCADES_TASK_LIST_MODEL_HPP
