#ifndef TODOIST_SYNC_PAYLOAD_BUILDER_HPP
#define TODOIST_SYNC_PAYLOAD_BUILDER_HPP

#include "todoist/sync/sync_types.hpp"
#include "todoist/db_manager.hpp"
#include <string>
#include <vector>

namespace todoist {

class SyncPayloadBuilder {
public:
    SyncPayloadBuilder();

    // Serializes a single stored command into Todoist API v1 wire format
    static std::string serializeCommandToWireJson(const SyncCommand& cmd);

    // Form URL-encoding and decoding helpers for application/x-www-form-urlencoded
    static std::string urlEncode(const std::string& str);
    static std::string urlDecode(const std::string& str);
    static bool parseFormBody(const std::string& body, std::map<std::string, std::string>& outFields);

    // Builds the full form-urlencoded request body for /api/v1/sync
    // Format: sync_token=<token>&resource_types=<json>&commands=<json>
    static std::string buildSyncRequestBody(const std::string& syncToken,
                                           const std::vector<SyncCommand>& commands,
                                           const std::vector<std::string>& resourceTypes);

    // Atomically selects pending commands, applies per-task FIFO serialization,
    // marks them in_flight in SQLite, and constructs the immutable FrozenRequest.
    // Transaction commits BEFORE any network I/O.
    static bool claimPendingBatch(DatabaseManager& db,
                                  size_t maxBatchSize,
                                  const std::string& syncToken,
                                  FrozenRequest& outRequest);

    // Deterministically reconstructs wire arguments and payload for a given batch of commands
    static std::string reconstructWirePayload(const std::string& syncToken,
                                             const std::vector<SyncCommand>& commands);
};

} // namespace todoist

#endif // TODOIST_SYNC_PAYLOAD_BUILDER_HPP
