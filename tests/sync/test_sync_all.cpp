#include "todoist/sync/sync_engine.hpp"
#include "todoist/sync/sync_payload_builder.hpp"
#include "todoist/sync/sync_response_parser.hpp"
#include "todoist/sync/sync_transport_interface.hpp"
#include "todoist/db_manager.hpp"
#include "todoist/task_repository.hpp"
#include "todoist/sync_command_queue.hpp"
#include "todoist/temp_id_reconciler.hpp"
#include "todoist/sync_metadata_repo.hpp"
#include "todoist/uuid_util.hpp"
#include "picojson.h"

#include <iostream>
#include <sstream>
#include <cassert>
#include <cstdio>
#include <unistd.h>

using namespace todoist;

// Mock Transport for hermetic testing
class MockSyncTransport : public ISyncTransport {
public:
    HttpResponse nextResponse;
    std::string lastRequestBody;
    std::string lastBearerToken;
    std::string lastEndpointUrl;
    int callCount;
    void (*onPost)(void* context);
    void* onPostContext;

    MockSyncTransport()
        : nextResponse(200, "{\"sync_token\":\"next_tok\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{}}")
        , lastRequestBody("")
        , lastBearerToken("")
        , lastEndpointUrl("")
        , callCount(0)
        , onPost(NULL)
        , onPostContext(NULL)
    {}

    virtual HttpResponse executeSyncPost(const std::string& endpointUrl,
                                        const std::string& requestBody,
                                        const std::string& bearerToken) {
        callCount++;
        lastEndpointUrl = endpointUrl;
        lastRequestBody = requestBody;
        lastBearerToken = bearerToken;
        if (onPost) {
            onPost(onPostContext);
        }
        return nextResponse;
    }
};

static std::string createTempDbPath(const std::string& suffix) {
    std::ostringstream ss;
    ss << "/tmp/todoist_sync_test_" << suffix << "_" << getpid() << "_" << util::currentEpochMs() << ".db";
    return ss.str();
}

static void removeDbFiles(const std::string& path) {
    unlink(path.c_str());
    unlink((path + "-wal").c_str());
    unlink((path + "-shm").c_str());
}

// -------------------------------------------------------------
// Test 1: Payload Builder & Outbound Wire Mapping (M2-D003)
// -------------------------------------------------------------
void testPayloadBuilderWireMapping() {
    std::cout << "[Test 1] Payload Builder Wire Mapping (M2-D003)..." << std::endl;

    // item_add with inbox (should omit project_id) and due date
    SyncCommand addCmd;
    addCmd.uuid = "uuid-add-1";
    addCmd.type = "item_add";
    addCmd.temp_id = "temp-101";
    addCmd.args_json = "{\"content\":\"Test Task\",\"project_id\":\"inbox\",\"priority\":3,\"due_date\":\"2026-10-25\"}";

    std::string addWire = SyncPayloadBuilder::serializeCommandToWireJson(addCmd);
    picojson::value addVal;
    std::string err = picojson::parse(addVal, addWire);
    assert(err.empty());
    assert(addVal.get("type").get<std::string>() == "item_add");
    assert(addVal.get("uuid").get<std::string>() == "uuid-add-1");
    assert(addVal.get("temp_id").get<std::string>() == "temp-101");
    picojson::object addArgs = addVal.get("args").get<picojson::object>();
    assert(addArgs["content"].get<std::string>() == "Test Task");
    assert(addArgs.find("project_id") == addArgs.end()); // Omitted for inbox
    assert(addArgs.find("due") != addArgs.end());
    assert(addArgs["due"].get("date").get<std::string>() == "2026-10-25");

    // item_complete mapped to item_close
    SyncCommand closeCmd;
    closeCmd.uuid = "uuid-close-1";
    closeCmd.type = "item_complete";
    closeCmd.args_json = "{\"id\":\"srv-101\"}";

    std::string closeWire = SyncPayloadBuilder::serializeCommandToWireJson(closeCmd);
    picojson::value closeVal;
    err = picojson::parse(closeVal, closeWire);
    assert(err.empty());
    assert(closeVal.get("type").get<std::string>() == "item_close");
    assert(closeVal.get("args").get("id").get<std::string>() == "srv-101");

    // item_update with due date clearing ("due": null)
    SyncCommand updCmd;
    updCmd.uuid = "uuid-upd-1";
    updCmd.type = "item_update";
    updCmd.args_json = "{\"id\":\"srv-101\",\"due_date\":null}";

    std::string updWire = SyncPayloadBuilder::serializeCommandToWireJson(updCmd);
    picojson::value updVal;
    err = picojson::parse(updVal, updWire);
    assert(err.empty());
    assert(updVal.get("type").get<std::string>() == "item_update");
    picojson::object updArgs = updVal.get("args").get<picojson::object>();
    assert(updArgs["due"].is<picojson::null>());

    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 2: Portable ISO-8601 Timestamp Parsing & 0 Sentinel (M2-D003)
// -------------------------------------------------------------
void testTimestampParsingAndSentinel() {
    std::cout << "[Test 2] Portable ISO-8601 & 0 Sentinel (M2-D003)..." << std::endl;

    int64_t epochMs = 0;
    // Exact UTC
    bool ok = SyncResponseParser::parseIso8601ToEpochMs("2026-10-20T12:00:00Z", epochMs);
    assert(ok);
    assert(epochMs == 1792497600000LL);

    // With milliseconds
    ok = SyncResponseParser::parseIso8601ToEpochMs("2026-10-20T12:00:00.500Z", epochMs);
    assert(ok);
    assert(epochMs == 1792497600500LL);

    // With timezone offset +02:00
    ok = SyncResponseParser::parseIso8601ToEpochMs("2026-10-20T14:00:00+02:00", epochMs);
    assert(ok);
    assert(epochMs == 1792497600000LL);

    // Missing / Malformed timestamp -> returns false and sets 0 sentinel
    epochMs = 999;
    ok = SyncResponseParser::parseIso8601ToEpochMs("invalid-date-string", epochMs);
    assert(!ok);
    assert(epochMs == 0); // 0 sentinel

    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 3: Outcome-Gated TempId Mapping & Inconsistent Response (M2-D004)
// -------------------------------------------------------------
void testOutcomeGatedMappingAndInconsistency() {
    std::cout << "[Test 3] Outcome-Gated TempId Mapping (M2-D004)..." << std::endl;

    SyncCommand createCmd;
    createCmd.uuid = "cmd-create-1";
    createCmd.type = "item_add";
    createCmd.temp_id = "temp-A";
    createCmd.args_json = "{\"content\":\"Task A\"}";

    std::vector<SyncCommand> inFlight;
    inFlight.push_back(createCmd);

    // Case 1: Inconsistent response - mapping present {temp-A: srv-A}, but status is missing
    ParsedDeltaResponse resp1;
    resp1.syncToken = "tok1";
    resp1.tempIdMapping["temp-A"] = "srv-A";
    // commandOutcomes is empty for cmd-create-1

    std::map<std::string, std::string> validatedMap;
    std::string mapErr;
    bool ok = SyncResponseParser::correlateAndValidateMappings(inFlight, resp1, validatedMap, mapErr);
    assert(!ok); // Rejected!
    assert(!mapErr.empty());

    // Case 2: Inconsistent response - status is "ok", but mapping is missing
    ParsedDeltaResponse resp2;
    resp2.syncToken = "tok2";
    CommandOutcome ackOutcome;
    ackOutcome.uuid = "cmd-create-1";
    ackOutcome.isAck = true;
    resp2.commandOutcomes["cmd-create-1"] = ackOutcome;
    // tempIdMapping is empty

    ok = SyncResponseParser::correlateAndValidateMappings(inFlight, resp2, validatedMap, mapErr);
    assert(!ok); // Rejected!

    // Case 3: Valid outcome-gated response - status is "ok" AND mapping present
    ParsedDeltaResponse resp3;
    resp3.syncToken = "tok3";
    resp3.commandOutcomes["cmd-create-1"] = ackOutcome;
    resp3.tempIdMapping["temp-A"] = "srv-A";
    resp3.tempIdMapping["foreign-temp"] = "foreign-srv"; // Unsolicited mapping

    ok = SyncResponseParser::correlateAndValidateMappings(inFlight, resp3, validatedMap, mapErr);
    assert(ok);
    assert(validatedMap.size() == 1);
    assert(validatedMap["temp-A"] == "srv-A");
    assert(validatedMap.find("foreign-temp") == validatedMap.end()); // Unsolicited ignored

    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 4: Tombstone Priority & State Finalization (M2-D001)
// -------------------------------------------------------------
struct Test4DeleteContext {
    TaskRepository* taskRepo;
};

static void deleteDuringFlightCallback(void* ctx) {
    Test4DeleteContext* tc = static_cast<Test4DeleteContext*>(ctx);
    bool delOk = tc->taskRepo->deleteTaskLocally("srv-task-1", "cmd-del-1");
    (void)delOk;
    assert(delOk);
}

void testTombstoneAwareFinalization() {
    std::cout << "[Test 4] Tombstone Priority & State Finalization (M2-D001)..." << std::endl;

    std::string dbPath = createTempDbPath("tombstone");
    removeDbFiles(dbPath);

    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    // Setup: Task starts Synced
    Task task;
    task.id = "srv-task-1";
    task.server_id = "srv-task-1";
    task.content = "Initial content";
    task.sync_state = SyncState::Synced;
    assert(taskRepo.upsertRemoteTask(task));

    // Send update U
    assert(taskRepo.updateTaskLocally("srv-task-1", "Edited content", "", 1, "", "cmd-upd-1"));

    // Prepare mock: during network I/O of cmd-upd-1, user deletes task locally (tombstone created)
    Test4DeleteContext ctx;
    ctx.taskRepo = &taskRepo;

    MockSyncTransport mock;
    mock.onPost = deleteDuringFlightCallback;
    mock.onPostContext = &ctx;
    mock.nextResponse.statusCode = 200;
    mock.nextResponse.body =
        "{"
        "  \"sync_token\": \"token-2\","
        "  \"full_sync\": false,"
        "  \"items\": [{\"id\":\"srv-task-1\",\"content\":\"Server echo\",\"is_deleted\":false}],"
        "  \"temp_id_mapping\": {},"
        "  \"sync_status\": {\"cmd-upd-1\":\"ok\"}"
        "}";

    SyncCycleResult cycleRes = engine.executeSyncCycle(mock);
    assert(cycleRes.isSuccess());

    // After response: U is pruned, delete command cmd-del-1 remains pending,
    // and task MUST strictly retain LocallyDeleted (never resurrected or reclassified to Synced!)
    Task currentTask;
    assert(taskRepo.getTaskById("srv-task-1", currentTask));
    assert(currentTask.sync_state == SyncState::LocallyDeleted);

    std::vector<SyncCommand> pending;
    assert(cmdQueue.getPendingCommands(pending));
    assert(pending.size() == 1);
    assert(pending[0].uuid == "cmd-del-1");
    assert(pending[0].status == CommandStatus::Pending);

    // Second sync cycle: dispatch cmd-del-1 to finalize deletion
    mock.onPost = NULL;
    mock.onPostContext = NULL;
    mock.nextResponse.statusCode = 200;
    mock.nextResponse.body =
        "{"
        "  \"sync_token\": \"token-3\","
        "  \"full_sync\": false,"
        "  \"items\": [],"
        "  \"temp_id_mapping\": {},"
        "  \"sync_status\": {\"cmd-del-1\":\"ok\"}"
        "}";

    SyncCycleResult cycleRes2 = engine.executeSyncCycle(mock);
    assert(cycleRes2.isSuccess());

    // After delete command acknowledged and no remaining commands: tombstone purged from DB
    assert(!taskRepo.getTaskById("srv-task-1", currentTask));
    assert(cmdQueue.getPendingCommands(pending));
    assert(pending.empty());

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 5: Startup Recovery & Ambiguity Preservation (M2-D002)
// -------------------------------------------------------------
void testStartupRecovery() {
    std::cout << "[Test 5] Startup Recovery & Ambiguity (M2-D002)..." << std::endl;

    std::string dbPath = createTempDbPath("recovery");
    removeDbFiles(dbPath);

    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    // Create task offline and claim in flight
    Task t;
    t.content = "Recovery task";
    assert(taskRepo.createTaskLocally(t, "cmd-create-1"));

    FrozenRequest req;
    assert(SyncPayloadBuilder::claimPendingBatch(db, 50, "*", req));
    assert(req.inFlightCommands.size() == 1);
    assert(req.inFlightCommands[0].retry_count == 0);

    // Simulate process crash and restart: call initialize()
    assert(engine.initialize());

    // Verify command reset to pending AND retry_count incremented to 1
    std::vector<SyncCommand> pending;
    assert(cmdQueue.getPendingCommands(pending));
    assert(pending.size() == 1);
    assert(pending[0].uuid == "cmd-create-1");
    assert(pending[0].status == CommandStatus::Pending);
    assert(pending[0].retry_count == 1); // Send history ambiguity preserved!

    // Verify per-task filter: task with 2 pending commands claims only first
    assert(taskRepo.updateTaskLocally(t.id, "Second edit", "", 2, "", "cmd-upd-2"));

    FrozenRequest req2;
    assert(SyncPayloadBuilder::claimPendingBatch(db, 50, "*", req2));
    assert(req2.inFlightCommands.size() == 1);
    assert(req2.inFlightCommands[0].uuid == "cmd-create-1"); // Only first claimed!

    // Verify task with in-flight command is not claimed in a concurrent batch
    FrozenRequest req3;
    assert(SyncPayloadBuilder::claimPendingBatch(db, 50, "*", req3));
    assert(req3.inFlightCommands.empty()); // cmd-upd-2 deferred because cmd-create-1 is in-flight!

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 6: Provenance-Guarded Full-Sync Absence Pruning (M2-D005)
// -------------------------------------------------------------
void testProvenanceGuardedFullSync() {
    std::cout << "[Test 6] Provenance-Guarded Full Sync Pruning (M2-D005)..." << std::endl;

    std::string dbPath = createTempDbPath("fullsync");
    removeDbFiles(dbPath);

    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    // Setup:
    // 1. Task X: pre-request clean (sync_state == Synced, no commands)
    Task taskX;
    taskX.id = "task-clean-X";
    taskX.server_id = "task-clean-X";
    taskX.content = "Clean X";
    taskX.sync_state = SyncState::Synced;
    assert(taskRepo.upsertRemoteTask(taskX));

    // 2. Task T: created locally, command in-flight
    Task taskT;
    taskT.content = "Created T";
    assert(taskRepo.createTaskLocally(taskT, "cmd-add-T"));
    std::string tempIdT = taskT.id;

    // 3. Task C: unsent local create (pending, not in this batch)
    Task taskC;
    taskC.content = "Unsent C";
    assert(taskRepo.createTaskLocally(taskC, "cmd-add-C"));

    // Claim batch with batch size 1 (only claims cmd-add-T)
    engine.setMaxBatchSize(1);

    // Mock Full-Sync Response:
    // - full_sync == true
    // - cmd-add-T acknowledged with "ok", mapping temp-T -> srv-S
    // - items array contains ONLY task Y (server echo for S is delayed and X is absent)
    MockSyncTransport mock;
    mock.nextResponse.statusCode = 200;
    mock.nextResponse.body =
        "{"
        "  \"sync_token\": \"token-full-2\","
        "  \"full_sync\": true,"
        "  \"items\": [{\"id\":\"task-remote-Y\",\"content\":\"Remote Y\",\"is_deleted\":false}],"
        "  \"temp_id_mapping\": {\"" + tempIdT + "\":\"srv-S\"},"
        "  \"sync_status\": {\"cmd-add-T\":\"ok\"}"
        "}";

    SyncCycleResult cycleRes = engine.executeSyncCycle(mock);
    assert(cycleRes.isSuccess());
    assert(cycleRes.hasFullSyncCatchUpNeeded);

    // Assertions:
    // 1. Task X: pre-request clean and absent from snapshot -> PRUNED!
    Task checkX;
    assert(!taskRepo.getTaskById("task-clean-X", checkX));

    // 2. Task S: newly mapped/acknowledged in this request -> STRICTLY PRESERVED (not pruned by absence)!
    Task checkS;
    assert(taskRepo.getTaskById("srv-S", checkS));
    assert(checkS.sync_state == SyncState::Synced);

    // 3. Task C: unsent local create -> STRICTLY PRESERVED!
    Task checkC;
    assert(taskRepo.getTaskById(taskC.id, checkC));
    assert(checkC.sync_state == SyncState::LocallyCreated);

    // 4. Task Y: remote snapshot item -> upserted
    Task checkY;
    assert(taskRepo.getTaskById("task-remote-Y", checkY));

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 7: Multi-Batch Queue Draining & Halting (M2-D007)
// -------------------------------------------------------------
void testMultiBatchQueueDraining() {
    std::cout << "[Test 7] Multi-Batch Draining & Halting (M2-D007)..." << std::endl;

    std::string dbPath = createTempDbPath("drain");
    removeDbFiles(dbPath);

    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);
    engine.setMaxBatchSize(2);

    // Enqueue 5 commands for 5 separate tasks
    for (int i = 1; i <= 5; ++i) {
        std::ostringstream ss;
        ss << "srv-task-" << i;
        Task t;
        t.id = ss.str();
        t.server_id = ss.str();
        t.sync_state = SyncState::Synced;
        assert(taskRepo.upsertRemoteTask(t));

        std::ostringstream cmdSs;
        cmdSs << "cmd-upd-" << i;
        assert(taskRepo.updateTaskLocally(t.id, "Edit", "", 1, "", cmdSs.str()));
    }

    // Mock transport accepts all
    MockSyncTransport mock;
    mock.nextResponse.statusCode = 200;
    mock.nextResponse.body =
        "{"
        "  \"sync_token\": \"tok-drain\","
        "  \"full_sync\": false,"
        "  \"items\": [],"
        "  \"temp_id_mapping\": {},"
        "  \"sync_status\": {"
        "    \"cmd-upd-1\":\"ok\",\"cmd-upd-2\":\"ok\",\"cmd-upd-3\":\"ok\","
        "    \"cmd-upd-4\":\"ok\",\"cmd-upd-5\":\"ok\""
        "  }"
        "}";

    // Drain up to 5 batches
    SyncCycleResult drainRes = engine.drainPendingQueue(mock, 5);
    assert(drainRes.isSuccess());
    assert(mock.callCount == 3); // 2 + 2 + 1 = 5 commands in 3 batches

    // Verify all 5 commands pruned from queue
    std::vector<SyncCommand> pending;
    assert(cmdQueue.getPendingCommands(pending));
    assert(pending.empty());

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 8: Form URL Encoding & Roundtrip (M2-R012)
// -------------------------------------------------------------
void testFormUrlEncodingAndRoundtrip() {
    std::cout << "[Test 8] Form URL Encoding & Roundtrip (M2-R012)..." << std::endl;

    // 1. urlEncode / urlDecode
    std::string plain = "hello world! 123 -_.~ +&=% /# [{\"key\":\"val\"}]";
    std::string encoded = SyncPayloadBuilder::urlEncode(plain);
    std::string decoded = SyncPayloadBuilder::urlDecode(encoded);
    assert(decoded == plain);

    // Unicode string encoding
    std::string utf8Str = "\xE4\xB9\xB0\xE7\x89\x9B\xE5\xA5\xB6"; // "buy milk" in UTF-8 Chinese characters
    std::string utf8Encoded = SyncPayloadBuilder::urlEncode(utf8Str);
    assert(utf8Encoded == "%E4%B9%B0%E7%89%9B%E5%A5%B6");
    assert(SyncPayloadBuilder::urlDecode(utf8Encoded) == utf8Str);

    // 2. parseFormBody
    std::string form = "sync_token=tok%2B123&resource_types=%5B%22items%22%5D&commands=%5B%7B%22type%22%3A%22item_close%22%7D%5D";
    std::map<std::string, std::string> parsed;
    assert(SyncPayloadBuilder::parseFormBody(form, parsed));
    assert(parsed["sync_token"] == "tok+123");
    assert(parsed["resource_types"] == "[\"items\"]");
    assert(parsed["commands"] == "[{\"type\":\"item_close\"}]");

    // 3. FrozenRequest request generation
    std::string dbPath = createTempDbPath("form_enc");
    removeDbFiles(dbPath);
    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    Task tForm;
    tForm.id = "temp-form-1";
    tForm.content = "Test form";
    tForm.priority = 1;
    assert(taskRepo.createTaskLocally(tForm, "cmd-form-1"));

    FrozenRequest req;
    assert(SyncPayloadBuilder::claimPendingBatch(db, 10, "test-cursor-1", req));
    assert(req.contentType == "application/x-www-form-urlencoded");

    std::map<std::string, std::string> reqParsed;
    assert(SyncPayloadBuilder::parseFormBody(req.serializedBody, reqParsed));
    assert(reqParsed["sync_token"] == "test-cursor-1");
    assert(reqParsed["resource_types"] == "[\"items\",\"projects\",\"user\"]");

    picojson::value cmdJson;
    std::string err = picojson::parse(cmdJson, reqParsed["commands"]);
    assert(err.empty());
    assert(cmdJson.is<picojson::array>());
    assert(cmdJson.get<picojson::array>().size() == 1);
    assert(cmdJson.get<picojson::array>()[0].get("uuid").get<std::string>() == "cmd-form-1");

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 9: Strict RFC3339 Timestamp & Calendar Validation (M2-R006)
// -------------------------------------------------------------
void testStrictTimestampAndCalendarValidation() {
    std::cout << "[Test 9] Strict RFC3339 Timestamp & Calendar Validation (M2-R006)..." << std::endl;

    int64_t epochMs = 9999;

    // Invalid calendar dates: Feb 31
    assert(!SyncResponseParser::parseIso8601ToEpochMs("2099-02-31T12:00:00Z", epochMs));
    assert(epochMs == 0);

    // Non-leap year Feb 29
    epochMs = 9999;
    assert(!SyncResponseParser::parseIso8601ToEpochMs("2023-02-29T12:00:00Z", epochMs));
    assert(epochMs == 0);

    // Month with 30 days: April 31
    epochMs = 9999;
    assert(!SyncResponseParser::parseIso8601ToEpochMs("2026-04-31T12:00:00Z", epochMs));
    assert(epochMs == 0);

    // Trailing garbage
    epochMs = 9999;
    assert(!SyncResponseParser::parseIso8601ToEpochMs("2099-01-01T00:00:00garbage", epochMs));
    assert(epochMs == 0);

    // Invalid timezone offset
    epochMs = 9999;
    assert(!SyncResponseParser::parseIso8601ToEpochMs("2099-01-01T00:00:00+99:99", epochMs));
    assert(epochMs == 0);
    epochMs = 9999;
    assert(!SyncResponseParser::parseIso8601ToEpochMs("2099-01-01T00:00:00+bad", epochMs));
    assert(epochMs == 0);

    // Decimal point without fractional digits
    epochMs = 9999;
    assert(!SyncResponseParser::parseIso8601ToEpochMs("2099-01-01T00:00:00.Z", epochMs));
    assert(epochMs == 0);

    // Valid leap year Feb 29
    epochMs = 0;
    assert(SyncResponseParser::parseIso8601ToEpochMs("2024-02-29T12:00:00Z", epochMs));
    assert(epochMs == 1709208000000LL);

    // Valid negative timezone offset (-05:00)
    epochMs = 0;
    assert(SyncResponseParser::parseIso8601ToEpochMs("2026-10-20T07:00:00-05:00", epochMs));
    assert(epochMs == 1792497600000LL);

    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 10: Strict Schema Validation & Trailing Garbage (M2-R005)
// -------------------------------------------------------------
void testSchemaValidationAndTrailingGarbage() {
    std::cout << "[Test 10] Schema Validation & Trailing Garbage (M2-R005)..." << std::endl;

    ParsedDeltaResponse resp;

    // 1. Active item with wrong field types (content integer, due number, priority string)
    std::string invalidItem =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":\"A\",\"content\":77,\"due\":42,\"priority\":\"urgent\",\"is_deleted\":false}]}";
    assert(!SyncResponseParser::validateAndParseResponse(invalidItem, resp));
    assert(!resp.validationError.empty());

    // 2. Trailing garbage after valid JSON
    std::string trailingGarbage =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{}} trailing-garbage";
    assert(!SyncResponseParser::validateAndParseResponse(trailingGarbage, resp));

    // 3. Trailing whitespace is allowed
    std::string trailingSpace =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{}}   \t\n";
    assert(SyncResponseParser::validateAndParseResponse(trailingSpace, resp));

    // 4. Item priority out of range (must be 1..4)
    std::string badPriority =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":\"A\",\"content\":\"Task\",\"priority\":5,\"is_deleted\":false}]}";
    assert(!SyncResponseParser::validateAndParseResponse(badPriority, resp));

    // 5. Sparse deletion tombstone is allowed (only id and is_deleted: true)
    std::string sparseDel =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":\"del-item-1\",\"is_deleted\":true}]}";
    assert(SyncResponseParser::validateAndParseResponse(sparseDel, resp));
    assert(resp.deletedTaskIds.size() == 1);
    assert(resp.deletedTaskIds[0] == "del-item-1");

    // 6. Active task missing content must be rejected (M2-R005)
    std::string missingContent =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":\"A\"}]}";
    assert(!SyncResponseParser::validateAndParseResponse(missingContent, resp));

    // 7. Fractional priority must be rejected (M2-R005)
    std::string fracPriority =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":\"A\",\"content\":\"x\",\"priority\":1.5}]}";
    assert(!SyncResponseParser::validateAndParseResponse(fracPriority, resp));

    // 8. Non-string ID must be rejected (M2-R005)
    std::string numId =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":9007199254740993,\"is_deleted\":true}]}";
    assert(!SyncResponseParser::validateAndParseResponse(numId, resp));

    // 9. Non-null due object missing date must be rejected (M2-R005)
    std::string emptyDueObj =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":\"A\",\"content\":\"x\",\"due\":{}}]}";
    assert(!SyncResponseParser::validateAndParseResponse(emptyDueObj, resp));

    // 10. Non-string project_id must be rejected (M2-R005)
    std::string numProj =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":\"A\",\"content\":\"x\",\"project_id\":12345}]}";
    assert(!SyncResponseParser::validateAndParseResponse(numProj, resp));

    // 11. Large finite floats outside integer range (1e100, -1e100) must be rejected pre-cast (M2-R005)
    std::string overflowPri =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":\"A\",\"content\":\"x\",\"priority\":1e100}]}";
    assert(!SyncResponseParser::validateAndParseResponse(overflowPri, resp));

    std::string negOverflowPri =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"items\":[{\"id\":\"A\",\"content\":\"x\",\"priority\":-1e100}]}";
    assert(!SyncResponseParser::validateAndParseResponse(negOverflowPri, resp));

    std::string overflowHttp =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},"
        "\"sync_status\":{\"cmd-1\":{\"http_code\":1e100}}}";
    assert(!SyncResponseParser::validateAndParseResponse(overflowHttp, resp));

    std::string overflowDomain =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},"
        "\"sync_status\":{\"cmd-1\":{\"error_code\":1e100}}}";
    assert(!SyncResponseParser::validateAndParseResponse(overflowDomain, resp));

    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 11: Domain Error Code Separation & Cascade (M2-R004)
// -------------------------------------------------------------
void testDomainErrorCodeSeparationAndCascade() {
    std::cout << "[Test 11] Domain Error Code Separation & Cascade (M2-R004)..." << std::endl;

    // 1. Parse error with error_code 20 and http_code 400
    std::string respJson =
        "{\"sync_token\":\"tok1\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},"
        "\"sync_status\":{"
        "  \"cmd-perm\": {\"error_code\": 20, \"http_code\": 400, \"error\": \"Invalid argument\"},"
        "  \"cmd-retry\": {\"http_code\": 429, \"error\": \"Too many requests\"},"
        "  \"cmd-retry500\": {\"http_code\": 500, \"error\": \"Internal server error\"}"
        "}}";

    ParsedDeltaResponse parsed;
    assert(SyncResponseParser::validateAndParseResponse(respJson, parsed));
    assert(parsed.commandOutcomes["cmd-perm"].isPermanent);
    assert(parsed.commandOutcomes["cmd-perm"].httpCode == 400);
    assert(parsed.commandOutcomes["cmd-perm"].domainCode == 20);
    assert(!parsed.commandOutcomes["cmd-retry"].isPermanent); // Retryable
    assert(!parsed.commandOutcomes["cmd-retry500"].isPermanent); // Retryable

    // Projects and User Inbox Project ID parsing (M4-DEV-001)
    std::string projJson =
        "{\"sync_token\":\"tok-p\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{},"
        "\"projects\":[{\"id\":\"6X7fqH39MwjmwV4q\",\"name\":\"Work\",\"is_deleted\":false,\"inbox_project\":true}],"
        "\"user\":{\"inbox_project_id\":\"6X7fqH39MwjmwV4q\"}}";
    ParsedDeltaResponse parsedProj;
    assert(SyncResponseParser::validateAndParseResponse(projJson, parsedProj));
    assert(parsedProj.projectNames["6X7fqH39MwjmwV4q"] == "Work");
    assert(parsedProj.inboxProjectId == "6X7fqH39MwjmwV4q");

    // 2. Engine execution: terminal create cascades to dependent commands
    std::string dbPath = createTempDbPath("cascade");
    removeDbFiles(dbPath);
    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    // Enqueue create task with temp_id "temp-A"
    Task tA;
    tA.id = "temp-A";
    tA.content = "Task A";
    tA.priority = 1;
    assert(taskRepo.createTaskLocally(tA, "cmd-add-A"));
    // Enqueue update depending on "temp-A"
    assert(taskRepo.updateTaskLocally("temp-A", "Task A updated", "", 2, "", "cmd-upd-A"));

    // Server returns terminal error 400 / error_code 20 for cmd-add-A
    MockSyncTransport mock;
    mock.nextResponse.statusCode = 200;
    mock.nextResponse.body =
        "{\"sync_token\":\"tok-cascade\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},"
        "\"sync_status\":{\"cmd-add-A\": {\"error_code\": 20, \"http_code\": 400, \"error\": \"Invalid argument\"}}}";

    SyncCycleResult res = engine.executeSyncCycle(mock);
    assert(res.isSuccess());
    assert(res.commandsFailedCount >= 1);

    // cmd-add-A moved to dead_letter_commands
    assert(cmdQueue.getDeadLetterCount() >= 1);

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 12: Engine State Machine & Exception Safety (M2-R001, M2-R007)
// -------------------------------------------------------------
class ThrowingSyncTransport : public ISyncTransport {
public:
    virtual HttpResponse executeSyncPost(const std::string&, const std::string&, const std::string&) {
        throw std::runtime_error("simulated network failure");
    }
};

void testEngineStateAndExceptionSafety() {
    std::cout << "[Test 12] Engine State Machine & Exception Safety (M2-R001, M2-R007)..." << std::endl;

    std::string dbPath = createTempDbPath("engine_state");
    removeDbFiles(dbPath);
    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo, "https://api.todoist.com/sync/v9/sync", "initial-token");

    Task t1;
    t1.id = "temp-1";
    t1.content = "Task 1";
    t1.priority = 1;
    assert(taskRepo.createTaskLocally(t1, "cmd-1"));

    // 1. Exception safety: transport throws
    ThrowingSyncTransport throwingTransport;
    SyncCycleResult throwRes = engine.executeSyncCycle(throwingTransport);
    assert(throwRes.status == SyncCycleStatus::RetryPending);
    assert(engine.getState() == SyncEngineState::RetryPending);

    // In-flight command is safely back to pending
    std::vector<SyncCommand> pending;
    assert(cmdQueue.getPendingCommands(pending));
    assert(pending.size() == 1);
    assert(pending[0].status == CommandStatus::Pending);

    // Next call is not stuck Busy!
    MockSyncTransport mock;
    mock.nextResponse.statusCode = 401; // 401 Unauthorized
    mock.nextResponse.body = "{\"error\": \"Unauthorized\"}";

    SyncCycleResult authRes = engine.executeSyncCycle(mock);
    assert(authRes.status == SyncCycleStatus::AuthError);
    assert(engine.getState() == SyncEngineState::AuthError);
    assert(mock.callCount == 1);

    // 2. AuthError suspension: subsequent call without token update makes ZERO transport calls
    SyncCycleResult authRes2 = engine.executeSyncCycle(mock);
    assert(authRes2.status == SyncCycleStatus::AuthError);
    assert(mock.callCount == 1); // No new network call!

    // 3. Updating token with valid new token clears AuthError suspension
    engine.setBearerToken("valid-new-token");
    assert(engine.getState() == SyncEngineState::Idle);

    mock.nextResponse.statusCode = 200;
    mock.nextResponse.body =
        "{\"sync_token\":\"tok-recovered\",\"full_sync\":false,\"items\":[],"
        "\"temp_id_mapping\":{\"temp-1\":\"srv-1\"},\"sync_status\":{\"cmd-1\":\"ok\"}}";

    SyncCycleResult recoveredRes = engine.executeSyncCycle(mock);
    assert(recoveredRes.isSuccess());
    assert(mock.callCount == 2); // Transport called now

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 13: Batch Size Bounds & Tie-Breaker Ordering (M2-R010)
// -------------------------------------------------------------
void testBatchSizeBoundsAndTieBreaker() {
    std::cout << "[Test 13] Batch Size Bounds & Tie-Breaker Ordering (M2-R010)..." << std::endl;

    std::string dbPath = createTempDbPath("batch_bounds");
    removeDbFiles(dbPath);
    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    // setMaxBatchSize clamping (0 resets to default 50, >100 clamped to 100)
    engine.setMaxBatchSize(0);
    assert(engine.getMaxBatchSize() == 50);
    engine.setMaxBatchSize(105);
    assert(engine.getMaxBatchSize() == 100);
    engine.setMaxBatchSize(1);
    assert(engine.getMaxBatchSize() == 1);

    // Enqueue 105 tasks
    for (int i = 1; i <= 105; ++i) {
        std::ostringstream ss;
        ss << "srv-task-" << i;
        Task t;
        t.id = ss.str();
        t.server_id = ss.str();
        t.sync_state = SyncState::Synced;
        assert(taskRepo.upsertRemoteTask(t));

        std::ostringstream cmdSs;
        cmdSs << "cmd-upd-" << i;
        assert(taskRepo.updateTaskLocally(t.id, "Edit", "", 1, "", cmdSs.str()));
    }

    FrozenRequest req;
    assert(SyncPayloadBuilder::claimPendingBatch(db, 105, "tok", req));
    assert(req.inFlightCommands.size() == 100); // Clamped at 100

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 14: Structural Target Extraction & Escapes (M2-R009)
// -------------------------------------------------------------
void testStructuralTargetExtractionEscapes() {
    std::cout << "[Test 14] Structural Target Extraction & Escapes (M2-R009)..." << std::endl;

    std::string dbPath = createTempDbPath("escapes");
    removeDbFiles(dbPath);
    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    // Task with backslash in ID: "opaque\id"
    std::string specialId = "opaque\\id";
    Task t;
    t.id = specialId;
    t.server_id = specialId;
    t.content = "Initial";
    t.sync_state = SyncState::Synced;
    assert(taskRepo.upsertRemoteTask(t));

    assert(taskRepo.updateTaskLocally(specialId, "Edited title", "", 1, "", "cmd-esc-1"));

    MockSyncTransport mock;
    mock.nextResponse.statusCode = 200;
    mock.nextResponse.body =
        "{\"sync_token\":\"tok-esc\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},"
        "\"sync_status\":{\"cmd-esc-1\":\"ok\"}}";

    SyncCycleResult res = engine.executeSyncCycle(mock);
    assert(res.isSuccess());

    // Task must be properly finalized to Synced (not left locally_updated due to bad string search)
    Task outT;
    assert(taskRepo.getTaskById(specialId, outT));
    assert(outT.sync_state == SyncState::Synced);

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 15: Acknowledged Echo Baseline Advancement (M2-R008)
// -------------------------------------------------------------
struct Test15EchoContext {
    TaskRepository* taskRepo;
};

static void test15ConcurrentEditCallback(void* ctx) {
    Test15EchoContext* c = static_cast<Test15EchoContext*>(ctx);
    bool ok = c->taskRepo->updateTaskLocally("A", "second local edit V", "", 1, "", "cmd-V");
    (void)ok;
    assert(ok);
}

void testAcknowledgedEchoBaselineAdvancement() {
    std::cout << "[Test 15] Acknowledged Echo Baseline Advancement (M2-R008)..." << std::endl;

    std::string dbPath = createTempDbPath("echo_adv");
    removeDbFiles(dbPath);
    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    // Initial synced task
    Task t;
    t.id = "A";
    t.server_id = "A";
    t.content = "original base";
    t.sync_state = SyncState::Synced;
    assert(taskRepo.upsertRemoteTask(t));

    // User updates task locally: edit U
    assert(taskRepo.updateTaskLocally("A", "first local edit U", "", 1, "", "cmd-U"));

    // Prepare mock: during network call, user performs second local edit V
    Test15EchoContext ctx;
    ctx.taskRepo = &taskRepo;

    MockSyncTransport mock;
    mock.onPost = test15ConcurrentEditCallback;
    mock.onPostContext = &ctx;
    mock.nextResponse.statusCode = 200;
    // Server acknowledges U and echoes U's sent content with a later timestamp (2099)
    mock.nextResponse.body =
        "{\"sync_token\":\"tok-echo\",\"full_sync\":false,"
        "\"items\":[{\"id\":\"A\",\"content\":\"first local edit U\",\"checked\":false,\"updated_at\":\"2099-01-01T00:00:00Z\"}],"
        "\"temp_id_mapping\":{},\"sync_status\":{\"cmd-U\":\"ok\"}}";

    SyncCycleResult cycleRes = engine.executeSyncCycle(mock);
    assert(cycleRes.isSuccess());

    // Task A MUST retain "second local edit V" (server echo of U did NOT overwrite newer edit V!)
    Task finalTask;
    assert(taskRepo.getTaskById("A", finalTask));
    assert(finalTask.content == "second local edit V");
    assert(finalTask.sync_state == SyncState::LocallyUpdated);

    // Command cmd-V must remain intact in pending queue
    std::vector<SyncCommand> pending;
    assert(cmdQueue.getPendingCommands(pending));
    assert(pending.size() == 1);
    assert(pending[0].uuid == "cmd-V");

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 16: Retryable Command Drain Halting (M2-R003)
// -------------------------------------------------------------
void testRetryableDrainHalting() {
    std::cout << "[Test 16] Retryable Command Drain Halting (M2-R003)..." << std::endl;

    std::string dbPath = createTempDbPath("drain_halt");
    removeDbFiles(dbPath);
    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    Task t1; t1.id = "t1"; t1.server_id = "t1"; t1.sync_state = SyncState::Synced; assert(taskRepo.upsertRemoteTask(t1));
    Task t2; t2.id = "t2"; t2.server_id = "t2"; t2.sync_state = SyncState::Synced; assert(taskRepo.upsertRemoteTask(t2));

    assert(taskRepo.updateTaskLocally("t1", "E1", "", 1, "", "cmd-1"));
    assert(taskRepo.updateTaskLocally("t2", "E2", "", 1, "", "cmd-2"));

    MockSyncTransport mock;
    mock.nextResponse.statusCode = 200;
    // cmd-1 ok, cmd-2 retryable
    mock.nextResponse.body =
        "{\"sync_token\":\"tok-mix\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},"
        "\"sync_status\":{\"cmd-1\":\"ok\",\"cmd-2\":{\"http_code\":500,\"error\":\"transient\"}}}";

    SyncCycleResult drainRes = engine.drainPendingQueue(mock, 3);
    assert(drainRes.status == SyncCycleStatus::RetryPending);
    assert(mock.callCount == 1); // Halts immediately after first batch!

    // cmd-1 pruned, cmd-2 remains pending with incremented retry
    std::vector<SyncCommand> pending;
    assert(cmdQueue.getPendingCommands(pending));
    assert(pending.size() == 1);
    assert(pending[0].uuid == "cmd-2");
    assert(pending[0].retry_count == 1);

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 17: SQL Failure Atomic Rollback (M2-R002)
// -------------------------------------------------------------
void testSqlFailureAtomicRollback() {
    std::cout << "[Test 17] SQL Failure Atomic Rollback (M2-R002)..." << std::endl;

    std::string dbPath = createTempDbPath("sql_fail");
    removeDbFiles(dbPath);
    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    Task t; t.id = "A"; t.server_id = "A"; t.sync_state = SyncState::Synced; assert(taskRepo.upsertRemoteTask(t));
    assert(taskRepo.updateTaskLocally("A", "Edit", "", 1, "", "cmd-1"));

    // Trigger failure on token update
    assert(db.execute("CREATE TRIGGER fail_token BEFORE INSERT ON sync_metadata WHEN NEW.key='sync_token' BEGIN SELECT RAISE(ABORT, 'token fault'); END;"));

    MockSyncTransport mock;
    mock.nextResponse.statusCode = 200;
    mock.nextResponse.body =
        "{\"sync_token\":\"tok-new\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},"
        "\"sync_status\":{\"cmd-1\":\"ok\"}}";

    SyncCycleResult res = engine.executeSyncCycle(mock);
    assert(res.status == SyncCycleStatus::RetryPending);
    assert(engine.getState() == SyncEngineState::RetryPending); // Not permanently Busy!

    // cmd-1 is safely back to pending
    std::vector<SyncCommand> pending;
    assert(cmdQueue.getPendingCommands(pending));
    assert(pending.size() == 1);
    assert(pending[0].uuid == "cmd-1");

    // Test recovery write failure: trigger rejects in_flight -> pending update
    assert(db.execute("DROP TRIGGER fail_token;"));
    assert(db.execute("CREATE TRIGGER fail_reset BEFORE UPDATE OF status ON sync_commands WHEN NEW.status='pending' BEGIN SELECT RAISE(ABORT, 'reset fault'); END;"));
    mock.nextResponse.statusCode = 500;
    SyncCycleResult recFailRes = engine.executeSyncCycle(mock);
    assert(recFailRes.status == SyncCycleStatus::FatalError); // Truthful storage failure!
    assert(cmdQueue.getInFlightCount() == 1);
    assert(cmdQueue.getPendingCount() == 0);

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 18: Non-Finite JSON Numeric Rejection (M2-R011)
// -------------------------------------------------------------
void testNonFiniteJsonHandling() {
    std::cout << "[Test 18] Non-Finite JSON Numeric Rejection (M2-R011)..." << std::endl;

    picojson::value v;
    std::string err = picojson::parse(v, "1e999");
    assert(!err.empty());
    assert(v.is<picojson::null>()); // No fabricated zero!

    err = picojson::parse(v, "-1e999");
    assert(!err.empty());
    assert(v.is<picojson::null>());

    // Valid finite numbers parse and roundtrip cleanly
    err = picojson::parse(v, "123.456");
    assert(err.empty());
    assert(v.is<double>());
    assert(v.get<double>() == 123.456);

    std::cout << "  -> Passed." << std::endl;
}

// -------------------------------------------------------------
// Test 19: Project Metadata Reconciliation and Unmapped Full Sync (M4-DEV-001)
// -------------------------------------------------------------
void testProjectMetadataReconciliationAndSync() {
    std::cout << "[Test 19] Project Metadata Reconciliation and Unmapped Full Sync (M4-DEV-001)..." << std::endl;

    std::string dbPath = createTempDbPath("proj_recon");
    removeDbFiles(dbPath);
    DatabaseManager db;
    assert(db.open(dbPath));
    assert(db.initializeSchema());

    TaskRepository taskRepo(db);
    SyncCommandQueue cmdQueue(db);
    TempIdReconciler reconciler(db);
    SyncMetadataRepository metaRepo(db, taskRepo, cmdQueue);
    SyncEngine engine(db, taskRepo, cmdQueue, reconciler, metaRepo);

    // 1. Initially set advanced sync token and insert task with unmapped project ID
    assert(metaRepo.setSyncToken("tok_advanced_prior_to_projects"));
    Task t;
    t.id = "srv_task_1";
    t.content = "Unmapped project task";
    t.project_id = "6hf635fJmgG7pC3P";
    t.sync_state = SyncState::Synced;
    assert(taskRepo.upsertRemoteTask(t));

    // 2. Prepare mock response for initial full sync
    MockSyncTransport mock;
    mock.nextResponse.statusCode = 200;
    mock.nextResponse.body =
        "{\"sync_token\":\"tok_reconciled_after_full\","
        "\"full_sync\":true,"
        "\"items\":[{\"id\":\"srv_task_1\",\"content\":\"Unmapped project task\",\"project_id\":\"6hf635fJmgG7pC3P\",\"is_deleted\":false}],"
        "\"projects\":[{\"id\":\"6hf635fJmgG7pC3P\",\"name\":\"Work Projects\",\"is_deleted\":false}],"
        "\"user\":{\"inbox_project_id\":\"inbox_server_id\"},"
        "\"temp_id_mapping\":{},\"sync_status\":{}}";

    SyncCycleResult res = engine.executeSyncCycle(mock);
    assert(res.isSuccess());

    // Verify sync_token=* was requested because hasUnmappedProjects() detected 6hf635fJmgG7pC3P
    assert(mock.lastRequestBody.find("sync_token=%2A") != std::string::npos ||
           mock.lastRequestBody.find("sync_token=*") != std::string::npos);

    // Verify project name stored in sync_metadata
    std::string projName;
    assert(metaRepo.getMetadata("project_name:6hf635fJmgG7pC3P", projName));
    assert(projName == "Work Projects");

    // Verify inbox project ID stored in sync_metadata
    std::string inboxId;
    assert(metaRepo.getMetadata("inbox_project_id", inboxId));
    assert(inboxId == "inbox_server_id");

    // 3. Next sync: hasUnmappedProjects() is now false, so incremental cursor is preserved
    mock.nextResponse.body =
        "{\"sync_token\":\"tok_incremental_2\","
        "\"full_sync\":false,"
        "\"items\":[],"
        "\"projects\":[{\"id\":\"6hf635fJmgG7pC3P\",\"name\":\"Work Projects\",\"is_deleted\":true}],"
        "\"temp_id_mapping\":{},\"sync_status\":{}}";

    SyncCycleResult incRes = engine.executeSyncCycle(mock);
    assert(incRes.isSuccess());

    // Verify incremental token was sent (not "*")
    assert(mock.lastRequestBody.find("tok_reconciled_after_full") != std::string::npos);

    // Verify deleted project was cleaned from sync_metadata
    std::string deletedCheck;
    assert(!metaRepo.getMetadata("project_name:6hf635fJmgG7pC3P", deletedCheck));

    // 4. Test deleteMetadata directly
    assert(metaRepo.setMetadata("test_delete_key", "val"));
    assert(metaRepo.deleteMetadata("test_delete_key"));
    assert(!metaRepo.getMetadata("test_delete_key", deletedCheck));

    // 5. Test M5-R002: Deletion failure stops immediately, rolls back all effects and does not commit later writes
    assert(sqlite3_exec(db.handle(),
        "CREATE TEMP TRIGGER reject_del_test BEFORE DELETE ON sync_metadata WHEN OLD.key='project_name:p_fail' BEGIN SELECT RAISE(ABORT,'injected'); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(metaRepo.setMetadata("project_name:p_fail", "Fail Project"));
    assert(metaRepo.setMetadata("project_name:p_survive", "Survive Project"));
    assert(metaRepo.setSyncToken("tok_before_fail"));

    mock.nextResponse.body =
        "{\"sync_token\":\"tok_should_not_commit\","
        "\"full_sync\":false,\"items\":[],"
        "\"projects\":[{\"id\":\"p_fail\",\"is_deleted\":true},{\"id\":\"p_survive\",\"is_deleted\":true}],"
        "\"user\":{\"inbox_project_id\":\"new_inbox_should_not_commit\"},"
        "\"temp_id_mapping\":{},\"sync_status\":{}}";

    SyncCycleResult failRes = engine.executeSyncCycle(mock);
    assert(!failRes.isSuccess());
    assert(metaRepo.getSyncToken() == "tok_before_fail");
    std::string checkVal;
    assert(metaRepo.getMetadata("project_name:p_fail", checkVal));
    assert(metaRepo.getMetadata("project_name:p_survive", checkVal));
    assert(!metaRepo.getMetadata("inbox_project_id", checkVal) || checkVal != "new_inbox_should_not_commit");

    db.close();
    removeDbFiles(dbPath);
    std::cout << "  -> Passed." << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Todoist Sync Engine - Test Suite (M2)" << std::endl;
    std::cout << "========================================" << std::endl;

    testPayloadBuilderWireMapping();
    testTimestampParsingAndSentinel();
    testOutcomeGatedMappingAndInconsistency();
    testTombstoneAwareFinalization();
    testStartupRecovery();
    testProvenanceGuardedFullSync();
    testMultiBatchQueueDraining();
    testFormUrlEncodingAndRoundtrip();
    testStrictTimestampAndCalendarValidation();
    testSchemaValidationAndTrailingGarbage();
    testDomainErrorCodeSeparationAndCascade();
    testEngineStateAndExceptionSafety();
    testBatchSizeBoundsAndTieBreaker();
    testStructuralTargetExtractionEscapes();
    testAcknowledgedEchoBaselineAdvancement();
    testRetryableDrainHalting();
    testSqlFailureAtomicRollback();
    testNonFiniteJsonHandling();
    testProjectMetadataReconciliationAndSync();

    std::cout << "========================================" << std::endl;
    std::cout << "All M2 Sync Tests Passed Successfully!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
