#ifndef TODOIST_TEMP_ID_RECONCILER_HPP
#define TODOIST_TEMP_ID_RECONCILER_HPP

#include "todoist/db_manager.hpp"
#include <map>
#include <string>

namespace todoist {

class TempIdReconciler {
public:
    explicit TempIdReconciler(DatabaseManager& dbManager);

    // Atomically remap temporary client IDs to permanent server IDs
    // across tasks table and all pending/in-flight sync commands
    bool reconcile(const std::map<std::string, std::string>& tempIdMapping);

private:
    DatabaseManager& m_db;

    TempIdReconciler(const TempIdReconciler&);
    TempIdReconciler& operator=(const TempIdReconciler&);
};

} // namespace todoist

#endif // TODOIST_TEMP_ID_RECONCILER_HPP
