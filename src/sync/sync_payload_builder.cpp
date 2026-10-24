#include "todoist/sync/sync_payload_builder.hpp"
#include "picojson.h"
#include <sqlite3.h>
#include <set>
#include <sstream>

namespace todoist {

namespace {

std::string extractTaskIdFromCommand(const SyncCommand& cmd) {
    if (!cmd.temp_id.empty()) {
        return cmd.temp_id;
    }
    picojson::value v;
    std::string err = picojson::parse(v, cmd.args_json);
    if (err.empty() && v.is<picojson::object>()) {
        const picojson::object& obj = v.get<picojson::object>();
        picojson::object::const_iterator it = obj.find("id");
        if (it != obj.end() && it->second.is<std::string>()) {
            return it->second.get<std::string>();
        }
    }
    return "";
}

picojson::object buildWireArgs(const SyncCommand& cmd) {
    picojson::object wireArgs;
    picojson::value parsed;
    std::string err = picojson::parse(parsed, cmd.args_json);
    picojson::object inArgs;
    if (err.empty() && parsed.is<picojson::object>()) {
        inArgs = parsed.get<picojson::object>();
    }

    if (cmd.type == "item_add") {
        if (inArgs.find("content") != inArgs.end()) {
            wireArgs["content"] = inArgs["content"];
        }
        if (inArgs.find("description") != inArgs.end()) {
            wireArgs["description"] = inArgs["description"];
        }
        if (inArgs.find("priority") != inArgs.end()) {
            wireArgs["priority"] = inArgs["priority"];
        }
        if (inArgs.find("project_id") != inArgs.end()) {
            std::string pid = inArgs["project_id"].is<std::string>() ? inArgs["project_id"].get<std::string>() : "";
            // Omit project_id if it is the default inbox
            if (!pid.empty() && pid != "inbox") {
                wireArgs["project_id"] = picojson::value(pid);
            }
        }
        if (inArgs.find("due_date") != inArgs.end() && inArgs["due_date"].is<std::string>()) {
            std::string dueDate = inArgs["due_date"].get<std::string>();
            if (!dueDate.empty()) {
                picojson::object dueObj;
                dueObj["date"] = picojson::value(dueDate);
                wireArgs["due"] = picojson::value(dueObj);
            }
        }
    } else if (cmd.type == "item_update") {
        if (inArgs.find("id") != inArgs.end()) {
            wireArgs["id"] = inArgs["id"];
        }
        if (inArgs.find("content") != inArgs.end()) {
            wireArgs["content"] = inArgs["content"];
        }
        if (inArgs.find("description") != inArgs.end()) {
            wireArgs["description"] = inArgs["description"];
        }
        if (inArgs.find("priority") != inArgs.end()) {
            wireArgs["priority"] = inArgs["priority"];
        }
        if (inArgs.find("project_id") != inArgs.end()) {
            wireArgs["project_id"] = inArgs["project_id"];
        }
        if (inArgs.find("due_date") != inArgs.end()) {
            if (inArgs["due_date"].is<picojson::null>()) {
                wireArgs["due"] = picojson::value(); // null
            } else if (inArgs["due_date"].is<std::string>()) {
                std::string dueDate = inArgs["due_date"].get<std::string>();
                if (dueDate.empty()) {
                    wireArgs["due"] = picojson::value(); // clearing date
                } else {
                    picojson::object dueObj;
                    dueObj["date"] = picojson::value(dueDate);
                    wireArgs["due"] = picojson::value(dueObj);
                }
            }
        }
    } else if (cmd.type == "item_complete" || cmd.type == "item_close" ||
               cmd.type == "item_uncomplete" || cmd.type == "item_delete") {
        if (inArgs.find("id") != inArgs.end()) {
            wireArgs["id"] = inArgs["id"];
        }
    }

    return wireArgs;
}

} // anonymous namespace

SyncPayloadBuilder::SyncPayloadBuilder() {
}

static inline char toHexChar(unsigned char c) {
    static const char hexDigits[] = "0123456789ABCDEF";
    return hexDigits[c & 0x0F];
}

static inline int fromHexChar(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string SyncPayloadBuilder::urlEncode(const std::string& str) {
    std::ostringstream ss;
    for (size_t i = 0; i < str.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            ss << static_cast<char>(c);
        } else {
            ss << '%' << toHexChar(c >> 4) << toHexChar(c & 0x0F);
        }
    }
    return ss.str();
}

std::string SyncPayloadBuilder::urlDecode(const std::string& str) {
    std::ostringstream ss;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%') {
            if (i + 2 < str.length()) {
                int h1 = fromHexChar(str[i + 1]);
                int h2 = fromHexChar(str[i + 2]);
                if (h1 >= 0 && h2 >= 0) {
                    ss << static_cast<char>((h1 << 4) | h2);
                    i += 2;
                    continue;
                }
            }
            ss << '%';
        } else if (str[i] == '+') {
            ss << ' ';
        } else {
            ss << str[i];
        }
    }
    return ss.str();
}

bool SyncPayloadBuilder::parseFormBody(const std::string& body, std::map<std::string, std::string>& outFields) {
    outFields.clear();
    size_t start = 0;
    while (start < body.length()) {
        size_t eq = body.find('=', start);
        if (eq == std::string::npos) {
            return false;
        }
        std::string rawKey = body.substr(start, eq - start);
        size_t amp = body.find('&', eq + 1);
        std::string rawVal;
        if (amp == std::string::npos) {
            rawVal = body.substr(eq + 1);
            start = body.length();
        } else {
            rawVal = body.substr(eq + 1, amp - (eq + 1));
            start = amp + 1;
        }
        outFields[urlDecode(rawKey)] = urlDecode(rawVal);
    }
    return true;
}

std::string SyncPayloadBuilder::serializeCommandToWireJson(const SyncCommand& cmd) {
    picojson::object cmdObj;

    // Standard wire command type mapping
    std::string wireType = cmd.type;
    if (wireType == "item_complete") {
        wireType = "item_close";
    }

    cmdObj["type"] = picojson::value(wireType);
    cmdObj["uuid"] = picojson::value(cmd.uuid);

    if (cmd.type == "item_add" && !cmd.temp_id.empty()) {
        cmdObj["temp_id"] = picojson::value(cmd.temp_id);
    }

    picojson::object argsObj = buildWireArgs(cmd);
    cmdObj["args"] = picojson::value(argsObj);

    return picojson::value(cmdObj).serialize();
}

std::string SyncPayloadBuilder::buildSyncRequestBody(const std::string& syncToken,
                                                    const std::vector<SyncCommand>& commands,
                                                    const std::vector<std::string>& resourceTypes) {
    std::string effectiveToken = syncToken.empty() ? "*" : syncToken;

    picojson::array resArray;
    for (size_t i = 0; i < resourceTypes.size(); ++i) {
        resArray.push_back(picojson::value(resourceTypes[i]));
    }
    std::string resTypesJson = picojson::value(resArray).serialize();

    picojson::array cmdArray;
    for (size_t i = 0; i < commands.size(); ++i) {
        picojson::value cmdVal;
        std::string serializedCmd = serializeCommandToWireJson(commands[i]);
        std::string err = picojson::parse(cmdVal, serializedCmd);
        if (err.empty()) {
            cmdArray.push_back(cmdVal);
        }
    }
    std::string commandsJson = picojson::value(cmdArray).serialize();

    // Form-urlencoded payload per Todoist Sync API v1 contract:
    // sync_token=<token>&resource_types=<json>&commands=<json>
    std::ostringstream ss;
    ss << "sync_token=" << urlEncode(effectiveToken)
       << "&resource_types=" << urlEncode(resTypesJson)
       << "&commands=" << urlEncode(commandsJson);

    return ss.str();
}

bool SyncPayloadBuilder::claimPendingBatch(DatabaseManager& db,
                                          size_t maxBatchSize,
                                          const std::string& syncToken,
                                          FrozenRequest& outRequest) {
    // Clamp batch size to official server maximum [1, 100] (M2-R010)
    if (maxBatchSize == 0) {
        maxBatchSize = 50;
    } else if (maxBatchSize > 100) {
        maxBatchSize = 100;
    }

    DatabaseManager::ScopedTransaction tx(db);
    if (!tx.isActive()) {
        return false;
    }

    // 1. Gather all tasks that currently have an in-flight command in SQLite
    // to strictly preserve per-task serialization (at most 1 in-flight command per task)
    std::set<std::string> seenTasksInBatch;
    sqlite3_stmt* flightStmt = NULL;
    const char* flightSql =
        "SELECT uuid, type, temp_id, args_json, status, created_at, retry_count, last_error "
        "FROM sync_commands WHERE status = 'in_flight';";

    if (sqlite3_prepare_v2(db.handle(), flightSql, -1, &flightStmt, NULL) != SQLITE_OK) {
        return false;
    }
    int flightRc = SQLITE_OK;
    while ((flightRc = sqlite3_step(flightStmt)) == SQLITE_ROW) {
        SyncCommand cmd;
        cmd.uuid = reinterpret_cast<const char*>(sqlite3_column_text(flightStmt, 0));
        cmd.type = reinterpret_cast<const char*>(sqlite3_column_text(flightStmt, 1));
        const unsigned char* tempId = sqlite3_column_text(flightStmt, 2);
        cmd.temp_id = tempId ? reinterpret_cast<const char*>(tempId) : "";
        cmd.args_json = reinterpret_cast<const char*>(sqlite3_column_text(flightStmt, 3));
        std::string taskId = extractTaskIdFromCommand(cmd);
        if (!taskId.empty()) {
            seenTasksInBatch.insert(taskId);
        }
    }
    sqlite3_finalize(flightStmt);
    if (flightRc != SQLITE_DONE) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "SELECT uuid, type, temp_id, args_json, status, created_at, retry_count, last_error "
        "FROM sync_commands WHERE status = 'pending' ORDER BY created_at ASC, rowid ASC;";

    if (sqlite3_prepare_v2(db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    std::vector<SyncCommand> allPending;
    int stepRc = SQLITE_OK;
    while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
        SyncCommand cmd;
        cmd.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        cmd.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const unsigned char* tempId = sqlite3_column_text(stmt, 2);
        cmd.temp_id = tempId ? reinterpret_cast<const char*>(tempId) : "";
        cmd.args_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        cmd.status = stringToCommandStatus(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        cmd.created_at = sqlite3_column_int64(stmt, 5);
        cmd.retry_count = sqlite3_column_int(stmt, 6);
        const unsigned char* err = sqlite3_column_text(stmt, 7);
        cmd.last_error = err ? reinterpret_cast<const char*>(err) : "";
        allPending.push_back(cmd);
    }
    sqlite3_finalize(stmt);
    if (stepRc != SQLITE_DONE) {
        return false;
    }

    // Apply per-task serialization filter: at most 1 in-flight command per task
    std::vector<SyncCommand> selected;

    for (size_t i = 0; i < allPending.size() && selected.size() < maxBatchSize; ++i) {
        const SyncCommand& cmd = allPending[i];
        std::string taskId = extractTaskIdFromCommand(cmd);
        if (!taskId.empty()) {
            if (seenTasksInBatch.find(taskId) != seenTasksInBatch.end()) {
                // Task already has a command in-flight or claimed in this batch; defer subsequent command
                continue;
            }
            seenTasksInBatch.insert(taskId);
        }
        selected.push_back(cmd);
    }

    // Mark selected commands in_flight
    if (!selected.empty()) {
        sqlite3_stmt* updateStmt = NULL;
        const char* updateSql = "UPDATE sync_commands SET status = 'in_flight' WHERE uuid = ?;";
        if (sqlite3_prepare_v2(db.handle(), updateSql, -1, &updateStmt, NULL) != SQLITE_OK) {
            return false;
        }

        for (size_t i = 0; i < selected.size(); ++i) {
            sqlite3_bind_text(updateStmt, 1, selected[i].uuid.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(updateStmt) != SQLITE_DONE) {
                sqlite3_finalize(updateStmt);
                return false;
            }
            sqlite3_reset(updateStmt);
            selected[i].status = CommandStatus::InFlight;
        }
        sqlite3_finalize(updateStmt);
    }

    // Generate FrozenRequest
    outRequest.inFlightCommands = selected;
    outRequest.syncToken = syncToken.empty() ? "*" : syncToken;
    outRequest.resourceTypes.clear();
    outRequest.resourceTypes.push_back("items");
    outRequest.resourceTypes.push_back("projects");
    outRequest.resourceTypes.push_back("user");
    outRequest.serializedBody = buildSyncRequestBody(outRequest.syncToken, selected, outRequest.resourceTypes);
    outRequest.serializedJsonBody = outRequest.serializedBody;
    outRequest.contentType = "application/x-www-form-urlencoded";

    return tx.commit();
}

std::string SyncPayloadBuilder::reconstructWirePayload(const std::string& syncToken,
                                                     const std::vector<SyncCommand>& commands) {
    std::vector<std::string> resTypes;
    resTypes.push_back("items");
    resTypes.push_back("projects");
    resTypes.push_back("user");
    return buildSyncRequestBody(syncToken, commands, resTypes);
}

} // namespace todoist
