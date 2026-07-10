#include <telemetry/telemetry.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <thread>

// Global counters to track mock function calls
static int mock_call_count = 0;

// Mock implementations for all telemetry functions
static telemetry_status_t
mock_assign_file_transfer_id(const char* mission_plan_id, const char* filepath, const char* file_transfer_id) {
    std::cout << "   [MOCK] AssignFileTransferID: mission=" << mission_plan_id << ", file=" << filepath
              << ", id=" << file_transfer_id << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_has_file_transfer_chunk(bool                                   has_chunk,
                                                       const char*                            file_transfer_id,
                                                       const telemetry_file_transfer_chunk_t* chunk) {
    std::cout << "   [MOCK] HasFileTransferChunk: has_chunk=" << has_chunk << ", id=" << file_transfer_id;
    if (chunk) {
        std::cout << ", chunk_id=" << chunk->id << ", size=" << chunk->size;
    }
    std::cout << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_active_mission_plan(const char* mission_plan_id) {
    std::cout << "   [MOCK] LogActiveMissionPlan: " << mission_plan_id << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_track(const char* track_id, const telemetry_lla_t* position, uint16_t error) {
    std::cout << "   [MOCK] LogTrack: id=" << track_id << ", pos=(" << position->lat << ", " << position->lon << ", "
              << position->alt << "), error=" << error << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_waypoint(const char* device_id, const telemetry_waypoint_t* waypoint) {
    std::cout << "   [MOCK] LogWaypoint: device=" << device_id << ", pos=(" << waypoint->position.lat << ", "
              << waypoint->position.lon << ", " << waypoint->position.alt << ")" << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t
mock_log_route(const char* device_id, const telemetry_waypoint_t* waypoints, uint32_t num_waypoints) {
    std::cout << "   [MOCK] LogRoute: device=" << device_id << ", waypoints=" << num_waypoints << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_tagged_entity(const char*            tag_id,
                                                 const char*            device_id,
                                                 const telemetry_lla_t* device_position,
                                                 time_t                 timestamp) {
    std::cout << "   [MOCK] LogTaggedEntity: tag=" << tag_id << ", device=" << device_id << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t
mock_log_attributed_deviation(const char* source_node, const char* device_id, telemetry_deviation_type_t deviation) {
    std::cout << "   [MOCK] LogAttributedDeviation: source=" << source_node << ", device=" << device_id
              << ", deviation=" << deviation << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t
mock_log_attributed_threat(const char* source_node, const char* device_id, telemetry_threat_type_t threat) {
    std::cout << "   [MOCK] LogAttributedThreat: source=" << source_node << ", device=" << device_id
              << ", threat=" << threat << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_unattributed_threat(const char* source_node, telemetry_threat_type_t threat) {
    std::cout << "   [MOCK] LogUnattributedThreat: source=" << source_node << ", threat=" << threat << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_new_user(const char* user_id) {
    std::cout << "   [MOCK] LogNewUser: " << user_id << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_new_login(const char* user_id) {
    std::cout << "   [MOCK] LogNewLogin: " << user_id << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_file_added(const char* filepath, const telemetry_file_info_t* file_info) {
    std::cout << "   [MOCK] LogFileAdded: path=" << filepath << ", size=" << file_info->file_size
              << ", type=" << file_info->file_type << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_kernel_module_unloaded(const char* filepath) {
    std::cout << "   [MOCK] LogKernelModuleUnloaded: " << filepath << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_kernel_module_loaded(const char* filepath) {
    std::cout << "   [MOCK] LogKernelModuleLoaded: " << filepath << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t
mock_log_offline_device(const char* device_id, const telemetry_lla_t* device_position, time_t timestamp) {
    std::cout << "   [MOCK] LogOfflineDevice: device=" << device_id << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_memory_dump(void) {
    std::cout << "   [MOCK] LogMemoryDump" << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_port_scan(const telemetry_port_scan_info_t* port_scan_info) {
    std::cout << "   [MOCK] LogPortScan: source_ip=" << port_scan_info->source_ip << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_privilege_escalation(void) {
    std::cout << "   [MOCK] LogPrivilegeEscalation" << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t
mock_log_excess_resource_utilization(const telemetry_resource_utilization_info_t* resource_info) {
    std::cout << "   [MOCK] LogExcessResourceUtilization: cpu=" << resource_info->cpu_usage
              << "%, mem=" << resource_info->mem_usage << "%" << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_arbitrary_data(const char* data) {
    std::cout << "   [MOCK] LogArbitraryData: " << data << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_ioc_update(const char* ioc_info) {
    std::cout << "   [MOCK] LogIOCUpdate: " << ioc_info << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_risk_update(const telemetry_risk_info_t* risk_info) {
    std::cout << "   [MOCK] LogRiskUpdate: score=" << (int)risk_info->risk_score
              << ", rec_action=" << (int)risk_info->rec_action << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_boot_stage_event(telemetry_boot_stage_t boot_stage, telemetry_task_status_t status) {
    std::cout << "   [MOCK] LogBootStageEvent: stage=" << boot_stage << ", status=" << status << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_access_tee(telemetry_tee_type_t tee_type, bool accessible) {
    std::cout << "   [MOCK] LogAccessTEE: type=" << tee_type << ", accessible=" << accessible << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_see_namespace(bool isolated, const telemetry_ns_info_t* ns_info) {
    std::cout << "   [MOCK] LogSEENamespace: isolated=" << isolated << ", pid=" << ns_info->pid
              << ", name=" << ns_info->name << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

static telemetry_status_t mock_log_invoke_tee(const telemetry_uuid_t* uuid) {
    std::cout << "   [MOCK] LogInvokeTEE: uuid=" << std::hex << uuid->time_low << std::dec << std::endl;
    mock_call_count++;
    return TELEMETRY_SUCCESS;
}

// Helper function to call all telemetry functions
void call_all_telemetry_functions(const char* phase) {
    std::cout << "\n" << phase << std::endl;
    std::cout << std::string(strlen(phase), '=') << std::endl << std::endl;

    telemetry_status_t status;

    // 1. AssignFileTransferID
    std::cout << "1. AssignFileTransferID..." << std::endl;
    status = telemetryAssignFileTransferID("mission-001", "/data/file.txt", "transfer-123");
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 2. HasFileTransferChunk
    std::cout << "2. HasFileTransferChunk..." << std::endl;
    telemetry_file_transfer_chunk_t chunk = {"chunk-001", 1024};
    status                                = telemetryHasFileTransferChunk(true, "transfer-123", &chunk);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 3. LogActiveMissionPlan
    std::cout << "3. LogActiveMissionPlan..." << std::endl;
    status = telemetryLogActiveMissionPlan("mission-001");
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 4. LogTrack
    std::cout << "4. LogTrack..." << std::endl;
    telemetry_lla_t position = {34.0522, -118.2437, 100.0};
    status                   = telemetryLogTrack("target-001", &position, 5);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 5. LogWaypoint
    std::cout << "5. LogWaypoint..." << std::endl;
    telemetry_waypoint_t waypoint = {position, std::time(nullptr)};
    status                        = telemetryLogWaypoint("device-001", &waypoint);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 6. LogRoute
    std::cout << "6. LogRoute..." << std::endl;
    telemetry_waypoint_t waypoints[3] = {{{34.0522, -118.2437, 100.0}, std::time(nullptr)},
                                         {{34.0622, -118.2537, 110.0}, std::time(nullptr) + 60},
                                         {{34.0722, -118.2637, 120.0}, std::time(nullptr) + 120}};
    status                            = telemetryLogRoute("device-001", waypoints, 3);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 7. LogTaggedEntity
    std::cout << "7. LogTaggedEntity..." << std::endl;
    status = telemetryLogTaggedEntity("tag-001", "device-001", &position, std::time(nullptr));
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 8. LogAttributedDeviation
    std::cout << "8. LogAttributedDeviation..." << std::endl;
    status = telemetryLogAttributedDeviation("node-001", "device-001", TELEMETRY_ROUTE_CHANGE);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 9. LogAttributedThreat
    std::cout << "9. LogAttributedThreat..." << std::endl;
    status = telemetryLogAttributedThreat("node-001", "device-001", TELEMETRY_USB_INSERTION);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 10. LogUnattributedThreat
    std::cout << "10. LogUnattributedThreat..." << std::endl;
    status = telemetryLogUnattributedThreat("node-001", TELEMETRY_GEOFENCE_ESCAPE);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 11. LogNewUser
    std::cout << "11. LogNewUser..." << std::endl;
    status = telemetryLogNewUser("user-001");
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 12. LogNewLogin
    std::cout << "12. LogNewLogin..." << std::endl;
    status = telemetryLogNewLogin("user-001");
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 13. LogFileAdded
    std::cout << "13. LogFileAdded..." << std::endl;
    telemetry_file_info_t file_info = {1024, std::time(nullptr), "text/plain", "user-001", "rw-r--r--"};
    status                          = telemetryLogFileAdded("/tmp/test.txt", &file_info);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 14. LogKernelModuleUnloaded
    std::cout << "14. LogKernelModuleUnloaded..." << std::endl;
    status = telemetryLogKernelModuleUnloaded("/lib/modules/test.ko");
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 15. LogKernelModuleLoaded
    std::cout << "15. LogKernelModuleLoaded..." << std::endl;
    status = telemetryLogKernelModuleLoaded("/lib/modules/test.ko");
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 16. LogOfflineDevice
    std::cout << "16. LogOfflineDevice..." << std::endl;
    status = telemetryLogOfflineDevice("device-002", &position, std::time(nullptr));
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 17. LogMemoryDump
    std::cout << "17. LogMemoryDump..." << std::endl;
    status = telemetryLogMemoryDump();
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 18. LogPortScan
    std::cout << "18. LogPortScan..." << std::endl;
    telemetry_port_scan_info_t port_scan_info = {"192.168.1.100", std::time(nullptr)};
    status                                    = telemetryLogPortScan(&port_scan_info);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 19. LogPrivilegeEscalation
    std::cout << "19. LogPrivilegeEscalation..." << std::endl;
    status = telemetryLogPrivilegeEscalation();
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 20. LogExcessResourceUtilization
    std::cout << "20. LogExcessResourceUtilization..." << std::endl;
    telemetry_resource_utilization_info_t resource_info = {85.5, 92.3, std::time(nullptr)};
    status                                              = telemetryLogExcessResourceUtilization(&resource_info);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 21. LogArbitraryData
    std::cout << "21. LogArbitraryData..." << std::endl;
    const char* json_data =
        R"({"event": "custom_event", "severity": "high", "details": {"component": "sensor", "value": 42}})";
    status = telemetryLogArbitraryData(json_data);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 22. LogIOCUpdate
    std::cout << "22. LogIOCUpdate..." << std::endl;
    status = telemetryLogIOCUpdate("IOC: Anomalous network traffic detected");
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 23. LogRiskUpdate
    std::cout << "23. LogRiskUpdate..." << std::endl;
    telemetry_risk_info_t risk_info = {75,
                                       2,
                                       {TELEMETRY_DETECTION_RISK,
                                        TELEMETRY_LOCALITY_RISK,
                                        TELEMETRY_TRANSPARENCY_RISK,
                                        TELEMETRY_TARGETING_RISK,
                                        TELEMETRY_CONFIDENCE_RISK,
                                        TELEMETRY_ATTRIBUTION_RISK}};
    status                          = telemetryLogRiskUpdate(&risk_info);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 24. LogBootStageEvent
    std::cout << "24. LogBootStageEvent..." << std::endl;
    status = telemetryLogBootStageEvent(TELEMETRY_STAGE0, TELEMETRY_IN_PROGRESS);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 25. LogAccessTEE
    std::cout << "25. LogAccessTEE..." << std::endl;
    status = telemetryLogAccessTEE(TELEMETRY_OPTEE, true);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 26. LogSEENamespace
    std::cout << "26. LogSEENamespace..." << std::endl;
    telemetry_ns_info_t ns_info = {12345, 0x00020000, "secure_ns"};
    status                      = telemetryLogSEENamespace(true, &ns_info);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;

    // 27. LogInvokeTEE
    std::cout << "27. LogInvokeTEE..." << std::endl;
    telemetry_uuid_t uuid = {0x12345678, 0x1234, 0x5678, {0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89}};
    status                = telemetryLogInvokeTEE(&uuid);
    std::cout << "   Status: " << (status == TELEMETRY_SUCCESS ? "SUCCESS" : "ERROR") << std::endl << std::endl;
}

void assign_all_mock_implementations() {
    std::cout << "\nAssigning Mock Implementations" << std::endl;
    std::cout << "==============================" << std::endl << std::endl;

    telemetrySetAssignFileTransferIdImpl(mock_assign_file_transfer_id);
    telemetrySetHasFileTransferChunkImpl(mock_has_file_transfer_chunk);
    telemetrySetLogActiveMissionPlanImpl(mock_log_active_mission_plan);
    telemetrySetLogTrackImpl(mock_log_track);
    telemetrySetLogWaypointImpl(mock_log_waypoint);
    telemetrySetLogRouteImpl(mock_log_route);
    telemetrySetLogTaggedEntityImpl(mock_log_tagged_entity);
    telemetrySetLogAttributedDeviationImpl(mock_log_attributed_deviation);
    telemetrySetLogAttributedThreatImpl(mock_log_attributed_threat);
    telemetrySetLogUnattributedThreatImpl(mock_log_unattributed_threat);
    telemetrySetLogNewUserImpl(mock_log_new_user);
    telemetrySetLogNewLoginImpl(mock_log_new_login);
    telemetrySetLogFileAddedImpl(mock_log_file_added);
    telemetrySetLogKernelModuleUnloadedImpl(mock_log_kernel_module_unloaded);
    telemetrySetLogKernelModuleLoadedImpl(mock_log_kernel_module_loaded);
    telemetrySetLogOfflineDeviceImpl(mock_log_offline_device);
    telemetrySetLogMemoryDumpImpl(mock_log_memory_dump);
    telemetrySetLogPortScanImpl(mock_log_port_scan);
    telemetrySetLogPrivilegeEscalationImpl(mock_log_privilege_escalation);
    telemetrySetLogExcessResourceUtilizationImpl(mock_log_excess_resource_utilization);
    telemetrySetLogArbitraryDataImpl(mock_log_arbitrary_data);
    telemetrySetLogIOCUpdateImpl(mock_log_ioc_update);
    telemetrySetLogRiskUpdateImpl(mock_log_risk_update);
    telemetrySetLogBootStageEventImpl(mock_log_boot_stage_event);
    telemetrySetLogAccessTEEImpl(mock_log_access_tee);
    telemetrySetLogSEENamespaceImpl(mock_log_see_namespace);
    telemetrySetLogInvokeTEEImpl(mock_log_invoke_tee);

    std::cout << "✓ All mock implementations assigned successfully!" << std::endl;
}

int main() {
    std::cout << "Comprehensive Standalone Telemetry API Test" << std::endl;
    std::cout << "==========================================" << std::endl << std::endl;

    telemetryInit();

    // Phase 1: Call all functions with default implementations
    call_all_telemetry_functions("Phase 1: Testing Default Implementations");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Phase 2: Assign mock implementations
    assign_all_mock_implementations();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Phase 3: Call all functions with mock implementations
    mock_call_count = 0;
    call_all_telemetry_functions("Phase 2: Testing Mock Implementations");

    std::cout << "\nMock Statistics" << std::endl;
    std::cout << "===============" << std::endl;
    std::cout << "Total mock function calls: " << mock_call_count << std::endl;
    std::cout << "Expected calls: 27" << std::endl;
    std::cout << "Status: "
              << (mock_call_count == 27 ? "✓ PASS - All mocks called correctly" : "✗ FAIL - Mock count mismatch")
              << std::endl
              << std::endl;

    // Phase 4: Reset to default implementations
    std::cout << "Resetting to Default Implementations" << std::endl;
    std::cout << "====================================" << std::endl;
    telemetryResetAllImpl();
    std::cout << "✓ All implementations reset to default" << std::endl << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    telemetryCleanup();

    std::cout << "\n✓ Comprehensive telemetry API test completed successfully!" << std::endl;

    return 0;
}
