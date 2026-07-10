#include "anduril/util/Logger.h"
#include "bratislavaApiShim.hpp"
#include "bratislavaPlugin.hpp"
#include "telemetry/magic_enum.hpp"

#include <BratislavaSocketImpl.h>
#include <spdlog/fmt/chrono.h>

#include <chrono>
#include <csignal>
#include <map>
#ifdef __cplusplus
extern "C" {
#endif
#include "sensor_api.h"
#ifdef __cplusplus
}
#endif

using namespace yellowstone::demoapp;

int main(int argc, char* argv[]) {
    std::cout << "Launching demo app: ";
    for (int i = 0; i < argc; i++) {
        std::cout << argv[i] << " ";
    }
    std::cout << std::endl;

    ELoggerLevel logger_level = INFO;

    if (argc > 1) {
        const std::string log_level_str = argv[1];
        if (std::string("ERROR") == log_level_str) {
            logger_level = ERROR;
        } else if (std::string("WARNING") == log_level_str) {
            logger_level = WARNING;
        } else if (std::string("INFO") == log_level_str) {
            logger_level = INFO;
        } else if (std::string("DEBUG") == log_level_str) {
            logger_level = DEBUG;
        } else {
            logger_level = INFO;
            std::cout << "Warning: Invalid argument for log level: " + log_level_str
                      << ". Using log level: " << logger_level << std::endl;
        }
    }

    if (argc > 2) {
        std::cout << "Warning: Extra arguments ignored. Using log level: " << logger_level << std::endl;
    }

    // Create an instrument API
    const auto                now     = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::string         logFile = fmt::format("/tmp/ta3/logs/demo-app-{:%Y%m%dT%H%M%SZ}.log", fmt::gmtime(now));
    constexpr ServerConfig    serverConfig = {"192.168.1.1", 50052};
    const LoggerConfig        loggerConfig = {logger_level, LOG_FILE, logFile.c_str()};
    const InstrumentAPIConfig config{serverConfig, loggerConfig};

    InstrumentAPI* api = createInstrumentAPI(config);
    anduril::util::GetLogger()->flush_on(anduril::util::GetSpdLogLevel(logger_level));
    SimOsShim os_shim = SimOsShim{api, loggerConfig.level};

    TelemetryAPI* telem = api->telemetry;
    SetPluginInstance(new BratislavaPlugin{"/filestore", &os_shim, telem});

    registerLinkCallback(api, INSTRUMENT_COMMS);
    registerSensorCallbacks(api);
    scanForDevices(api, std::chrono::milliseconds(30000));

    // Block indefinitely / until killed by user. All work happens in the background
    // driven by the above callbacks.

    sigset_t signals = {
        SIGTERM,
        SIGINT,
    };
    siginfo_t result;
    sigwaitinfo(&signals, &result);
    LOG_INFO("Signal received. Exiting. code={}", result.si_code);

    LOG_INFO("Tearing down Bratislava plugin.");
    stopScanningForDevices();
    DeletePluginInstance();

    LOG_INFO("Tearing down Bratislava API.");
    destroyInstrumentAPI(api);

    return 0;
}
