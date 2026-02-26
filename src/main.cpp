#include <config.h>
#include <async_logger.h>
#include <metrics.h>
#include <daemon.h>
#include <iostream>
#include <string>
#include <exception>

int main(int argc, char* argv[]) {

    const std::string config_path = (argc > 1) ? argv[1] : "./config.ini";
 
    Metrics::measurement().reset();

    if (!Config::cfg().load(config_path)) {
        std::cerr << "Failed to load config\n";
        return 1;
    }
    
    if (!AsyncLogger::logger().start()) {
        std::cerr << "Failed to start async logger\n";
        return 1;
    }
    
    int code = 0;

    try {
        Daemon daemon;
        daemon.run();

    } catch (const std::exception& e) {
        std::cerr << "Exception from daemon: " << e.what() << "\n";
        code = 1;    
    } catch (...) {
        std::cerr << "Non-std exception from daemon\n";
        code = 1;
    }

    AsyncLogger::logger().shutdown();
    
    return code;
}