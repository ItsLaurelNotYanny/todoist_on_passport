#include "todoist/sync/sync_response_parser.hpp"
#include "picojson.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <sstream>

namespace todoist {

using std::isdigit;
using std::isspace;

namespace {

static bool isLeapYear(int year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static int getDaysInMonth(int year, int month) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) {
        return isLeapYear(year) ? 29 : 28;
    }
    return (month >= 1 && month <= 12) ? days[month - 1] : 0;
}

} // anonymous namespace

SyncResponseParser::SyncResponseParser() {
}

bool SyncResponseParser::parseIso8601ToEpochMs(const std::string& isoStr, int64_t& outEpochMs) {
    outEpochMs = 0; // Default to 0 sentinel
    if (isoStr.length() < 19 || isoStr.length() > 35) {
        return false;
    }

    for (int i = 0; i < 4; ++i) {
        if (!isdigit(static_cast<unsigned char>(isoStr[i]))) return false;
    }
    if (isoStr[4] != '-') return false;
    if (!isdigit(static_cast<unsigned char>(isoStr[5])) || !isdigit(static_cast<unsigned char>(isoStr[6]))) return false;
    if (isoStr[7] != '-') return false;
    if (!isdigit(static_cast<unsigned char>(isoStr[8])) || !isdigit(static_cast<unsigned char>(isoStr[9]))) return false;

    char sep = isoStr[10];
    if (sep != 'T' && sep != ' ' && sep != 't') return false;

    if (!isdigit(static_cast<unsigned char>(isoStr[11])) || !isdigit(static_cast<unsigned char>(isoStr[12]))) return false;
    if (isoStr[13] != ':') return false;
    if (!isdigit(static_cast<unsigned char>(isoStr[14])) || !isdigit(static_cast<unsigned char>(isoStr[15]))) return false;
    if (isoStr[16] != ':') return false;
    if (!isdigit(static_cast<unsigned char>(isoStr[17])) || !isdigit(static_cast<unsigned char>(isoStr[18]))) return false;

    int year = std::atoi(isoStr.substr(0, 4).c_str());
    int month = std::atoi(isoStr.substr(5, 2).c_str());
    int day = std::atoi(isoStr.substr(8, 2).c_str());
    int hour = std::atoi(isoStr.substr(11, 2).c_str());
    int min = std::atoi(isoStr.substr(14, 2).c_str());
    int sec = std::atoi(isoStr.substr(17, 2).c_str());

    if (year < 1970 || year > 2200 || month < 1 || month > 12) {
        return false;
    }
    if (day < 1 || day > getDaysInMonth(year, month)) {
        return false; // Rejects invalid calendar days like Feb 31
    }
    if (hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
        return false;
    }

    size_t idx = 19;
    int fracMs = 0;
    if (idx < isoStr.length() && isoStr[idx] == '.') {
        idx++;
        size_t startFrac = idx;
        int mult = 100;
        while (idx < isoStr.length() && isdigit(static_cast<unsigned char>(isoStr[idx]))) {
            if (mult > 0) {
                fracMs += (isoStr[idx] - '0') * mult;
                mult /= 10;
            }
            idx++;
        }
        if (idx == startFrac) {
            // Period without fractional digits is invalid (e.g. 2099-01-01T00:00:00.Z)
            return false;
        }
    }

    int tzOffsetSeconds = 0;
    if (idx >= isoStr.length()) {
        return false;
    }

    char tzChar = isoStr[idx];
    if (tzChar == 'Z' || tzChar == 'z') {
        idx++;
    } else if (tzChar == '+' || tzChar == '-') {
        idx++;
        if (idx + 5 > isoStr.length()) return false;
        if (!isdigit(static_cast<unsigned char>(isoStr[idx])) ||
            !isdigit(static_cast<unsigned char>(isoStr[idx + 1])) ||
            isoStr[idx + 2] != ':' ||
            !isdigit(static_cast<unsigned char>(isoStr[idx + 3])) ||
            !isdigit(static_cast<unsigned char>(isoStr[idx + 4]))) {
            return false;
        }
        int tzHour = (isoStr[idx] - '0') * 10 + (isoStr[idx + 1] - '0');
        int tzMin = (isoStr[idx + 3] - '0') * 10 + (isoStr[idx + 4] - '0');
        if (tzHour < 0 || tzHour > 23 || tzMin < 0 || tzMin > 59) {
            return false;
        }
        tzOffsetSeconds = tzHour * 3600 + tzMin * 60;
        if (tzChar == '-') {
            tzOffsetSeconds = -tzOffsetSeconds;
        }
        idx += 5;
    } else {
        return false;
    }

    if (idx != isoStr.length()) {
        return false; // Trailing garbage
    }

    // Mathematical Julian Day conversion for UTC days since 1970-01-01
    int a = (14 - month) / 12;
    int y = year + 4800 - a;
    int m = month + 12 * a - 3;
    int64_t julianDay = day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
    int64_t daysSinceEpoch = julianDay - 2440588; // 2440588 is 1970-01-01
    int64_t epochSeconds = daysSinceEpoch * 86400 + hour * 3600 + min * 60 + sec;

    epochSeconds -= tzOffsetSeconds;
    outEpochMs = epochSeconds * 1000 + fracMs;
    return true;
}

bool SyncResponseParser::validateAndParseResponse(const std::string& jsonBody,
                                                 ParsedDeltaResponse& outResponse) {
    outResponse = ParsedDeltaResponse();

    picojson::value rootVal;
    std::string err;
    std::string::const_iterator it = jsonBody.begin();
    it = picojson::parse(rootVal, it, jsonBody.end(), &err);
    if (!err.empty() || !rootVal.is<picojson::object>()) {
        outResponse.validationError = "Malformed JSON or root is not an object";
        return false;
    }

    // Verify trailing characters after JSON object are only whitespace
    while (it != jsonBody.end()) {
        if (!isspace(static_cast<unsigned char>(*it))) {
            outResponse.validationError = "Trailing garbage after valid JSON root object";
            return false;
        }
        ++it;
    }

    const picojson::object& root = rootVal.get<picojson::object>();

    // 1. Validate sync_token
    picojson::object::const_iterator itToken = root.find("sync_token");
    if (itToken == root.end() || !itToken->second.is<std::string>() || itToken->second.get<std::string>().empty()) {
        outResponse.validationError = "Missing or empty string sync_token";
        return false;
    }
    outResponse.syncToken = itToken->second.get<std::string>();

    // 2. Validate full_sync
    picojson::object::const_iterator itFull = root.find("full_sync");
    if (itFull == root.end() || !itFull->second.is<bool>()) {
        outResponse.validationError = "Missing or non-boolean full_sync";
        return false;
    }
    outResponse.isFullSync = itFull->second.get<bool>();

    // 3. Validate items
    picojson::object::const_iterator itItems = root.find("items");
    if (itItems == root.end() || !itItems->second.is<picojson::array>()) {
        outResponse.validationError = "Missing or non-array items";
        return false;
    }

    // 4. Validate temp_id_mapping
    picojson::object::const_iterator itMap = root.find("temp_id_mapping");
    if (itMap == root.end() || !itMap->second.is<picojson::object>()) {
        outResponse.validationError = "Missing or non-object temp_id_mapping";
        return false;
    }

    // 5. Validate sync_status
    picojson::object::const_iterator itStatus = root.find("sync_status");
    if (itStatus == root.end() || !itStatus->second.is<picojson::object>()) {
        outResponse.validationError = "Missing or non-object sync_status";
        return false;
    }

    // Parse temp_id_mapping members
    const picojson::object& mapObj = itMap->second.get<picojson::object>();
    for (picojson::object::const_iterator itM = mapObj.begin(); itM != mapObj.end(); ++itM) {
        if (!itM->second.is<std::string>() || itM->second.get<std::string>().empty()) {
            outResponse.validationError = "Non-string or empty value in temp_id_mapping";
            return false;
        }
        outResponse.tempIdMapping[itM->first] = itM->second.get<std::string>();
    }

    // Parse sync_status members
    const picojson::object& statusObj = itStatus->second.get<picojson::object>();
    for (picojson::object::const_iterator itS = statusObj.begin(); itS != statusObj.end(); ++itS) {
        CommandOutcome outcome;
        outcome.uuid = itS->first;

        if (itS->second.is<std::string>()) {
            std::string s = itS->second.get<std::string>();
            if (s == "ok") {
                outcome.isAck = true;
            } else {
                outcome.isAck = false;
                outcome.error = s;
            }
        } else if (itS->second.is<picojson::object>()) {
            outcome.isAck = false;
            const picojson::object& errObj = itS->second.get<picojson::object>();
            picojson::object::const_iterator itErr = errObj.find("error");
            if (itErr != errObj.end() && itErr->second.is<std::string>()) {
                outcome.error = itErr->second.get<std::string>();
            }

            int httpCode = 0;
            picojson::object::const_iterator itHttp = errObj.find("http_code");
            if (itHttp != errObj.end()) {
                if (!itHttp->second.is<double>()) {
                    outResponse.validationError = "Invalid type for http_code (must be number)";
                    return false;
                }
                double dHttp = itHttp->second.get<double>();
                if (dHttp < 100.0 || dHttp > 599.0 || dHttp != static_cast<double>(static_cast<int>(dHttp))) {
                    outResponse.validationError = "http_code out of valid HTTP status range (100..599) or not integer";
                    return false;
                }
                httpCode = static_cast<int>(dHttp);
            }
            outcome.httpCode = httpCode;

            int domainCode = 0;
            picojson::object::const_iterator itDomain = errObj.find("error_code");
            if (itDomain != errObj.end()) {
                if (!itDomain->second.is<double>()) {
                    outResponse.validationError = "Invalid type for error_code (must be number)";
                    return false;
                }
                double dDomain = itDomain->second.get<double>();
                if (dDomain < 0.0 || dDomain > 2147483647.0 || dDomain != static_cast<double>(static_cast<int>(dDomain))) {
                    outResponse.validationError = "error_code out of integer range or not integer";
                    return false;
                }
                domainCode = static_cast<int>(dDomain);
            }
            outcome.domainCode = domainCode;

            // Separate domain error_code from HTTP status (M2-R004)
            // 429: Rate Limit (Transient)
            // 5xx: Server error (Transient)
            // 4xx: Client error (Permanent)
            if (httpCode == 429) {
                outcome.isPermanent = false;
            } else if (httpCode >= 400 && httpCode < 500) {
                outcome.isPermanent = true;
            } else if (httpCode >= 500) {
                outcome.isPermanent = false;
            } else if (domainCode > 0) {
                // If http_code is missing, check standard Todoist client domain codes:
                // 10-49 are client/argument/state errors (Permanent)
                if (domainCode >= 10 && domainCode < 50) {
                    outcome.isPermanent = true;
                } else {
                    outcome.isPermanent = false;
                }
            } else {
                outcome.isPermanent = false;
            }
        } else {
            outResponse.validationError = "Invalid type for command outcome in sync_status";
            return false;
        }
        outResponse.commandOutcomes[outcome.uuid] = outcome;
    }

    // Parse items array
    const picojson::array& itemsArr = itItems->second.get<picojson::array>();
    for (size_t i = 0; i < itemsArr.size(); ++i) {
        if (!itemsArr[i].is<picojson::object>()) {
            outResponse.validationError = "Non-object element in items array";
            return false;
        }
        const picojson::object& item = itemsArr[i].get<picojson::object>();

        // ID is required and preserved as non-empty opaque string
        picojson::object::const_iterator itId = item.find("id");
        if (itId == item.end()) {
            outResponse.validationError = "Item missing id";
            return false;
        }
        if (!itId->second.is<std::string>()) {
            outResponse.validationError = "Item id must be a string";
            return false;
        }
        std::string itemId = itId->second.get<std::string>();
        if (itemId.empty()) {
            outResponse.validationError = "Empty item id";
            return false;
        }

        // Check is_deleted
        bool isDeleted = false;
        picojson::object::const_iterator itDel = item.find("is_deleted");
        if (itDel != item.end()) {
            if (itDel->second.is<bool>()) {
                isDeleted = itDel->second.get<bool>();
            } else if (itDel->second.is<double>()) {
                double d = itDel->second.get<double>();
                if (d != 0.0 && d != 1.0) {
                    outResponse.validationError = "Invalid numeric flag for is_deleted";
                    return false;
                }
                isDeleted = (d != 0.0);
            } else {
                outResponse.validationError = "Invalid type for is_deleted (must be boolean)";
                return false;
            }
        }

        if (isDeleted) {
            outResponse.deletedTaskIds.push_back(itemId);
            continue;
        }

        Task t;
        t.id = itemId;
        t.server_id = itemId;
        t.sync_state = SyncState::Synced;

        // Active task requires string content
        picojson::object::const_iterator itContent = item.find("content");
        if (itContent == item.end() || !itContent->second.is<std::string>()) {
            outResponse.validationError = "Active item missing or invalid content (must be string)";
            return false;
        }
        t.content = itContent->second.get<std::string>();

        picojson::object::const_iterator itDesc = item.find("description");
        if (itDesc != item.end()) {
            if (!itDesc->second.is<std::string>()) {
                outResponse.validationError = "Invalid type for item description (must be string)";
                return false;
            }
            t.description = itDesc->second.get<std::string>();
        }

        picojson::object::const_iterator itChecked = item.find("checked");
        if (itChecked == item.end()) {
            itChecked = item.find("is_completed");
        }
        if (itChecked != item.end()) {
            if (itChecked->second.is<bool>()) {
                t.is_completed = itChecked->second.get<bool>();
            } else if (itChecked->second.is<double>()) {
                double d = itChecked->second.get<double>();
                if (d != 0.0 && d != 1.0) {
                    outResponse.validationError = "Invalid numeric flag for checked/is_completed";
                    return false;
                }
                t.is_completed = (d != 0.0);
            } else {
                outResponse.validationError = "Invalid type for checked/is_completed (must be boolean)";
                return false;
            }
        }

        picojson::object::const_iterator itPri = item.find("priority");
        if (itPri != item.end()) {
            if (!itPri->second.is<double>()) {
                outResponse.validationError = "Invalid type for item priority (must be number)";
                return false;
            }
            double dPri = itPri->second.get<double>();
            if (dPri < 1.0 || dPri > 4.0 || dPri != static_cast<double>(static_cast<int>(dPri))) {
                outResponse.validationError = "Item priority must be integer in range 1..4";
                return false;
            }
            t.priority = static_cast<int>(dPri);
        }

        picojson::object::const_iterator itProj = item.find("project_id");
        if (itProj != item.end()) {
            if (!itProj->second.is<std::string>()) {
                outResponse.validationError = "Invalid type for project_id (must be string)";
                return false;
            }
            t.project_id = itProj->second.get<std::string>();
        }
        if (t.project_id.empty()) {
            t.project_id = "inbox";
        }

        // due date
        picojson::object::const_iterator itDue = item.find("due");
        if (itDue != item.end()) {
            if (itDue->second.is<picojson::null>()) {
                t.due_date = "";
            } else if (itDue->second.is<picojson::object>()) {
                const picojson::object& dueObj = itDue->second.get<picojson::object>();
                picojson::object::const_iterator itDueDate = dueObj.find("date");
                if (itDueDate == dueObj.end() || !itDueDate->second.is<std::string>() || itDueDate->second.get<std::string>().empty()) {
                    outResponse.validationError = "Non-null due object must contain non-empty string 'date'";
                    return false;
                }
                t.due_date = itDueDate->second.get<std::string>();
            } else if (itDue->second.is<std::string>()) {
                if (itDue->second.get<std::string>().empty()) {
                    outResponse.validationError = "Empty due string is invalid";
                    return false;
                }
                t.due_date = itDue->second.get<std::string>();
            } else {
                outResponse.validationError = "Invalid type for due (must be object, string, or null)";
                return false;
            }
        }

        // updated_at
        picojson::object::const_iterator itUpd = item.find("updated_at");
        if (itUpd != item.end()) {
            if (!itUpd->second.is<std::string>()) {
                outResponse.validationError = "Invalid type for updated_at (must be string)";
                return false;
            }
            int64_t epochMs = 0;
            if (parseIso8601ToEpochMs(itUpd->second.get<std::string>(), epochMs)) {
                t.local_updated_at = epochMs;
            } else {
                t.local_updated_at = 0; // 0 sentinel on malformed
            }
        } else {
            t.local_updated_at = 0; // 0 sentinel on missing
        }

        outResponse.upsertedTasks.push_back(t);
    }

    // Parse optional projects array for project names and inbox detection
    picojson::object::const_iterator itProjects = root.find("projects");
    if (itProjects != root.end() && itProjects->second.is<picojson::array>()) {
        outResponse.hasProjectsField = true;
        const picojson::array& projArr = itProjects->second.get<picojson::array>();
        for (size_t i = 0; i < projArr.size(); ++i) {
            if (projArr[i].is<picojson::object>()) {
                const picojson::object& pObj = projArr[i].get<picojson::object>();
                picojson::object::const_iterator itPId = pObj.find("id");
                picojson::object::const_iterator itPName = pObj.find("name");
                picojson::object::const_iterator itPDel = pObj.find("is_deleted");
                picojson::object::const_iterator itPArch = pObj.find("is_archived");
                bool isDeleted = (itPDel != pObj.end() && itPDel->second.is<bool>() && itPDel->second.get<bool>());
                bool isArchived = (itPArch != pObj.end() && itPArch->second.is<bool>() && itPArch->second.get<bool>());
                std::string pId;
                if (itPId != pObj.end()) {
                    if (itPId->second.is<std::string>()) {
                        pId = itPId->second.get<std::string>();
                    } else if (itPId->second.is<double>()) {
                        std::ostringstream oss;
                        oss << static_cast<int64_t>(itPId->second.get<double>());
                        pId = oss.str();
                    }
                }
                if (!pId.empty()) {
                    if (isDeleted || isArchived) {
                        outResponse.deletedProjectIds.push_back(pId);
                    } else if (itPName != pObj.end() && itPName->second.is<std::string>()) {
                        outResponse.projectNames[pId] = itPName->second.get<std::string>();
                    }
                }
                picojson::object::const_iterator itInbox = pObj.find("inbox_project");
                if (itInbox != pObj.end() && itInbox->second.is<bool>() && itInbox->second.get<bool>()) {
                    outResponse.inboxProjectId = pId;
                }
            }
        }
    }

    // Parse optional user object for inbox_project_id
    picojson::object::const_iterator itUser = root.find("user");
    if (itUser != root.end() && itUser->second.is<picojson::object>()) {
        const picojson::object& uObj = itUser->second.get<picojson::object>();
        picojson::object::const_iterator itInbox = uObj.find("inbox_project_id");
        if (itInbox != uObj.end()) {
            if (itInbox->second.is<std::string>()) {
                outResponse.inboxProjectId = itInbox->second.get<std::string>();
            } else if (itInbox->second.is<double>()) {
                std::ostringstream oss;
                oss << static_cast<int64_t>(itInbox->second.get<double>());
                outResponse.inboxProjectId = oss.str();
            }
        }
    }

    outResponse.isValid = true;
    return true;
}

bool SyncResponseParser::correlateAndValidateMappings(const std::vector<SyncCommand>& inFlightCommands,
                                                     const ParsedDeltaResponse& response,
                                                     std::map<std::string, std::string>& outValidatedMapping,
                                                     std::string& outErrorMessage) {
    outValidatedMapping.clear();
    outErrorMessage.clear();

    for (size_t i = 0; i < inFlightCommands.size(); ++i) {
        const SyncCommand& cmd = inFlightCommands[i];
        if (cmd.type == "item_add" && !cmd.temp_id.empty()) {
            std::map<std::string, CommandOutcome>::const_iterator itOutcome =
                response.commandOutcomes.find(cmd.uuid);
            bool isAck = (itOutcome != response.commandOutcomes.end() && itOutcome->second.isAck);

            std::map<std::string, std::string>::const_iterator itMap =
                response.tempIdMapping.find(cmd.temp_id);
            bool hasMapping = (itMap != response.tempIdMapping.end() && !itMap->second.empty());

            if (isAck && !hasMapping) {
                outErrorMessage = "Acknowledged item_add missing required temp_id mapping for: " + cmd.temp_id;
                return false;
            }

            if (!isAck && hasMapping) {
                outErrorMessage = "Inconsistent response: mapping provided for unacknowledged item_add: " + cmd.temp_id;
                return false;
            }

            if (isAck && hasMapping) {
                outValidatedMapping[cmd.temp_id] = itMap->second;
            }
        }
    }

    return true;
}

} // namespace todoist
