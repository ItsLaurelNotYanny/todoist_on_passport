#ifndef TODOIST_SYNC_RESPONSE_PARSER_HPP
#define TODOIST_SYNC_RESPONSE_PARSER_HPP

#include "todoist/sync/sync_types.hpp"
#include <string>
#include <vector>
#include <map>

namespace todoist {

class SyncResponseParser {
public:
    SyncResponseParser();

    // Portable ISO-8601 / RFC3339 UTC timestamp parser to epoch milliseconds.
    // On failure/absence, sets outEpochMs = 0 (epoch 0 sentinel) and returns false.
    static bool parseIso8601ToEpochMs(const std::string& isoStr, int64_t& outEpochMs);

    // Performs strict typed pre-validation and memory parsing of the sync response.
    // Checks that root envelope, sync_token (non-empty string), full_sync (bool),
    // items (array), temp_id_mapping (object), and sync_status (object) are well-typed.
    static bool validateAndParseResponse(const std::string& jsonBody,
                                        ParsedDeltaResponse& outResponse);

    // Outcome-Gated Mapping Correlation (M2-D004):
    // Correlates mappings strictly with in-flight batch outcomes.
    // Returns false if response is inconsistent (e.g. mapping present for unacknowledged/error add,
    // or acknowledged add missing its temp_id mapping).
    static bool correlateAndValidateMappings(const std::vector<SyncCommand>& inFlightCommands,
                                            const ParsedDeltaResponse& response,
                                            std::map<std::string, std::string>& outValidatedMapping,
                                            std::string& outErrorMessage);
};

} // namespace todoist

#endif // TODOIST_SYNC_RESPONSE_PARSER_HPP
