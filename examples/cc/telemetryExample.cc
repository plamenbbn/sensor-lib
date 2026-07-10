extern "C" {
#include "sensor_api.h"
}

#include <anduril/util/ErrnoToString.h>

#include <uuid/uuid.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

/**
 * @file telemetryExample.cc
 * @brief A simple example demonstrating usage of the telemetry endpoint of the
 * C API.
 * @details
 *
 * Expectations:
 * 1. Get the name of the source node (hostname).
 * 2. Create a Bluetooth device representing a target node.
 * 3. Create a Bluetooth device representing a following device.
 * 4. Log the active mission plan of discovered objective.yaml files.
 * 5. If a file transfer objective exists, assign a file transfer ID to its file.
 * 6. If a file transfer objective exists, demonstrate chunk logging.
 * 7. Log a track of the target device.
 * 8. Log a route change of the target device from the prescribed mission.
 * 9. Log a change in the active mission plan.
 * 10. Log a geofence escape threat for this node.
 * 11. Log a USB insertion threat for this node.
 * 12. Log the detection of a device that is following this node.
 * 13. Log a waypoint for the target device.
 * 14. Log a route for the target device.
 * 15. Log a file addition.
 * 16. Log a kernel module unload and load.
 * 17. Log an offline device.
 * 18. Log a memory dump.
 * 19. Log a port scan.
 * 20. Log a privilege escalation.
 * 21. Log excess resource utilization.
 * 22. Log an IOC update.
 * 23. Log a risk update.
 * 24. Log bootstrap stage events.
 * 25. Log TEE access.
 * 26. Log SEE namespace establishment.
 * 27. Log a TEE invocation.
 */

// Function to simulate location updates
C_LLA updateLocation(C_LLA current_location, double distance) {
    // Simple example: move north by 'distance' meters
    // 1 degree of latitude is approximately 111,139 meters
    current_location.lat += (distance / 111139.0);
    return current_location;
}

instrument_api_status_t getPosition(InstrumentAPI* apiHandle, C_PositionInfo& gps_info) {
    std::cout << "Getting device location using GPS" << std::endl;

    // Make a get position action
    constexpr InstrumentActions action = INSTRUMENT_GPS_GET_POSITION;
    std::vector<C_PositionInfo> positionInfo(1);
    uint32_t                    outputLen{};

    const auto result = apiHandle->instrumentAction(
        action, nullptr, 0, static_cast<InstrumentOutputType>(&positionInfo.front()), &outputLen);

    if (result != INSTRUMENT_API_SUCCESS) {
        std::cout << "Error: " << result << std::endl;
        return result;
    }
    if (!outputLen) {
        std::cout << "Empty response! " << std::endl;
    } else {
        std::cout << "Acquired GPS position data: " << std::endl;
        std::cout << "    Timestamp: " << positionInfo[0].timestamp << std::endl;
        std::cout << "    Position: " << positionInfo[0].position.lat << ", " << positionInfo[0].position.lon << ", "
                  << positionInfo[0].position.alt << std::endl;
    }

    gps_info = positionInfo[0];
    return result;
}

std::vector<YAML::Node> readYaml(const std::filesystem::path& directory_path, const std::string& filename_regex_str) {
    std::vector<YAML::Node> matching_nodes;
    std::regex              filename_regex(filename_regex_str);
    if (!std::filesystem::exists(directory_path)) {
        std::cerr << "Directory does not exist: " << directory_path << std::endl;
        return matching_nodes;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory_path)) {
        if (entry.is_regular_file()) {
            const std::string filename = entry.path().filename().string();
            if (std::regex_match(filename, filename_regex)) {
                try {
                    matching_nodes.push_back(YAML::LoadFile(entry.path().string()));
                    std::cout << "Parsed YAML file: " << filename << std::endl;
                } catch (const YAML::ParserException& e) {
                    std::cerr << "Error parsing YAML file " << entry.path() << ": " << e.what() << std::endl;
                }
            }
        }
    }
    return matching_nodes;
}

std::string generateUUID4() {
    uuid_t uuid;
    char   uuid_str[37];

    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, uuid_str);

    return std::string(uuid_str);
}

int main() {
    // Get the name of this node (hostname).
    char source_node[HOST_NAME_MAX];
    if (gethostname(source_node, HOST_NAME_MAX) != 0) {
        std::cerr << "Error getting hostname: " << anduril::util::errnoToString() << std::endl;
        return 1;
    }

    // Create a Bluetooth device representing a target node.
    BluetoothDeviceInfoBase target_device;
    strcpy(target_device.name, "Johnny's iPhone");
    strcpy(target_device.mac_address, "3A:D2:D3:AE:F0:8F");

    // Create a Bluetooth device representing a following device.
    BluetoothDeviceInfoBase following_device;
    strcpy(following_device.name, "Suzy's AirPods Pro");
    strcpy(following_device.mac_address, "5E:D8:A3:BC:F1:3F");

    // Create the API.
    std::cout << "Creating instrument API" << std::endl;
    constexpr ServerConfig        serverConfig = {"192.168.1.1", 50052};
    constexpr LoggerConfig        loggerConfig = {INFO};
    constexpr InstrumentAPIConfig config{serverConfig, loggerConfig};
    InstrumentAPI*                api = createInstrumentAPI(config);
    if (api == nullptr) {
        std::cout << "Failed to create API: " << INSTRUMENT_API_ERROR << std::endl;
        return 1;
    }
    std::cout << "Instrument API created" << std::endl;

    instrument_api_status_t result = INSTRUMENT_API_ERROR;

    std::string             filestore_dir = "/filestore";
    std::vector<YAML::Node> nodes         = readYaml(filestore_dir, "objective.*\\.yaml");
    for (const auto& node : nodes) {
        if (node["mission_plan_id"]) {
            std::string mission_plan_id = node["mission_plan_id"].as<std::string>();

            // Log the active mission plan.
            result = api->telemetry->logActiveMissionPlan(mission_plan_id.c_str());
            if (result != INSTRUMENT_API_SUCCESS) {
                return 1;
            }

            // If the objective is a file transfer, demonstrate file transfer ID assignment and chunk logging
            for (const auto& objective : node["objective"]) {
                std::string type = objective["type"].as<std::string>();
                if (type == "FILE_TRANSFER" || type == "HIGH_PRIORITY_EXFIL") {
                    std::string filename = objective["contents"].as<std::string>();
                    std::string filepath = (std::filesystem::path(filestore_dir) / filename).string();

                    if (!std::filesystem::exists(filepath)) {
                        std::cerr << "File does not exist: " << filepath << std::endl;
                        continue;
                    }

                    std::uintmax_t filesize    = std::filesystem::file_size(filepath);
                    std::string    transfer_id = generateUUID4();

                    std::cout << "Assigning transfer ID to " << filepath << std::endl;
                    result = api->telemetry->assignFileTransferID(mission_plan_id.c_str(),
                                                                  filepath.c_str(),
                                                                  transfer_id.c_str());
                    if (result != INSTRUMENT_API_SUCCESS) {
                        std::cout
                            << "Failed to assign file transfer ID. This occurs if the provided file is not "
                               "associated with a mission_plan_id, or the file transfer ID has already been assigned."
                            << std::endl;
                    }

                    // Let's try to assign the same ID twice, to demonstrate INSTRUMENT_API_ERROR return
                    std::cout << "Validating that duplicate file transfer ID assignment fails." << std::endl;
                    result = api->telemetry->assignFileTransferID(mission_plan_id.c_str(),
                                                                  filepath.c_str(),
                                                                  transfer_id.c_str());
                    if (result != INSTRUMENT_API_ERROR) {
                        std::cout << "Duplicate file transfer ID assignment unexpectedly succeeded." << std::endl;
                    }

                    // Let's try to assign a file not associated with the mission plan
                    std::cout
                        << "Validating that assigning a transfer ID to an invalid `file hash + mission plan id` fails."
                        << std::endl;
                    std::string unassociated_filepath =
                        (std::filesystem::path(filestore_dir) / "unassociated_test.txt").string();
                    std::ofstream(unassociated_filepath) << "Test content";
                    std::string unassociated_uuid = generateUUID4();
                    result                        = api->telemetry->assignFileTransferID(mission_plan_id.c_str(),
                                                                  unassociated_filepath.c_str(),
                                                                  unassociated_uuid.c_str());
                    if (result != INSTRUMENT_API_ERROR) {
                        std::cout << "Transfer ID assignment to an unassociated file unexpectedly succeeded."
                                  << std::endl;
                    }
                    std::filesystem::remove(unassociated_filepath);

                    // Now let's demonstrate chunk logging
                    std::cout << "Logging chunks for " << filepath << std::endl;
                    uint32_t chunk_id = 0;
                    for (uint32_t offset = 0; offset < filesize; offset += 900) {
                        uint32_t chunk_size =
                            std::min(static_cast<uint32_t>(900), static_cast<uint32_t>(filesize - offset));
                        std::string       chunk_id_str = std::to_string(chunk_id);
                        FileTransferChunk chunk        = {chunk_id_str.c_str(), chunk_size};
                        result = api->telemetry->hasFileTransferChunk(true, transfer_id.c_str(), &chunk);
                        if (result != INSTRUMENT_API_SUCCESS) {
                            return 1;
                        }
                        chunk_id++;
                    }
                }
            }
        }
    }

    // Initial location
    C_PositionInfo gps_info;
    result = getPosition(api, gps_info);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }
    C_LLA              location = gps_info.position;
    constexpr uint16_t error    = 0; // in meters

    // Log initial track
    result = api->telemetry->logTrack(target_device.mac_address, location, error);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Loop to update location over time and log changes
    constexpr int num_updates = 5;
    Waypoint      waypoints[num_updates];
    for (auto& waypoint : waypoints) {
        constexpr double distance_per_update = 50.0;

        location = updateLocation(location, distance_per_update);

        // Log track
        result = api->telemetry->logTrack(target_device.mac_address, location, error);
        if (result != INSTRUMENT_API_SUCCESS) {
            return 1;
        }

        waypoint.position  = location;
        waypoint.timestamp = time(nullptr);

        // Log waypoint
        result = api->telemetry->logWaypoint(target_device.mac_address, &waypoint);
        if (result != INSTRUMENT_API_SUCCESS) {
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Log the route
    result = api->telemetry->logRoute(target_device.mac_address, waypoints, num_updates);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a route change of the target device from the prescribed mission.
    result =
        api->telemetry->logAttributedDeviation(source_node, target_device.mac_address, DeviationType::ROUTE_CHANGE);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a change in the active mission plan.
    result = api->telemetry->logActiveMissionPlan("alert_deviation.plan.v1");
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a geofence escape threat for this node.
    result = api->telemetry->logUnattributedThreat(source_node, ThreatType::GEOFENCE_ESCAPE);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a USB insertion threat for this node.
    result = api->telemetry->logUnattributedThreat(source_node, ThreatType::USB_INSERTION);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log the detection of a device that is following this node.
    result =
        api->telemetry->logAttributedThreat(source_node, following_device.mac_address, ThreatType::FOLLOWER_DETECTED);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a file addition.
    const FileInfo file_info = {1024, time(nullptr), "txt", "user", "rw-r--r--"};
    result                   = api->telemetry->logFileAdded("/path/to/file.txt", &file_info);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a kernel module unload.
    result = api->telemetry->logKernelModuleUnloaded("/path/to/kernel_module.ko");
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a kernel module load.
    result = api->telemetry->logKernelModuleLoaded("/path/to/kernel_module.ko");
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log an offline device.
    result = api->telemetry->logOfflineDevice(following_device.mac_address, location, time(nullptr));
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a memory dump.
    result = api->telemetry->logMemoryDump();
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a port scan.
    PortScanInfo port_scan_info = {"192.168.1.100", time(nullptr)};
    result                      = api->telemetry->logPortScan(&port_scan_info);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a privilege escalation.
    result = api->telemetry->logPrivilegeEscalation();
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log excess resource utilization.
    ResourceUtilizationInfo resource_info = {75.0, 60.0, time(nullptr)};
    result                                = api->telemetry->logExcessResourceUtilization(&resource_info);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a tagged entity.
    result = api->telemetry->logTaggedEntity("tag123", target_device.mac_address, location, time(nullptr));
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    result = api->telemetry->logNewUser("user1");
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    result = api->telemetry->logNewLogin("user1");
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log an IOC update.
    result = api->telemetry->logIOCUpdate("IOC: Anomalous network traffic detected");
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a risk update.
    RiskInfo risk_info = {75,
                          2,
                          {RISK_TYPE_ENUM_DETECTION,
                           RISK_TYPE_ENUM_LOCALITY,
                           RISK_TYPE_ENUM_TRANSPARENCY,
                           RISK_TYPE_ENUM_TARGETING,
                           RISK_TYPE_ENUM_CONFIDENCE,
                           RISK_TYPE_ENUM_ATTRIBUTION}};
    result             = api->telemetry->logRiskUpdate(&risk_info);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log bootstrap stage events.
    result = api->telemetry->logBootStageEvent(BootStage::STAGE0, TaskStatus::IN_PROGRESS);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    result = api->telemetry->logBootStageEvent(BootStage::STAGE0, TaskStatus::COMPLETED_SUCCESS);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log TEE access.
    result = api->telemetry->logAccessTEE(TEEType::OPTEE, true);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log SEE namespace establishment.
    NSInfo ns_info = {12345, 0x00020000, const_cast<char*>("secure_ns")};
    result         = api->telemetry->logSEENamespace(true, ns_info);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    // Log a TEE invocation.
    UUID tee_uuid = {0x12345678, 0x1234, 0x5678, {0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89}};
    result        = api->telemetry->logInvokeTEE(tee_uuid);
    if (result != INSTRUMENT_API_SUCCESS) {
        return 1;
    }

    destroyInstrumentAPI(api);
    return 0;
}
