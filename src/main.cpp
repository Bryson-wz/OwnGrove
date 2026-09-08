#include "owngrove/AppConfig.h"
#include "owngrove/HttpServer.h"

#include <iostream>

int main(int argc, char* argv[])
{
    int port = owngrove::kDefaultPort;
    if (argc > 1) {
        if (!owngrove::parsePort(argv[1], port)) {
            std::cerr << "Invalid port '" << argv[1] << "'. Expected an integer in 1-65535."
                      << std::endl;
            return 1;
        }
    } else if (const auto env_port = owngrove::readCompatEnv("OWNGROVE_PORT", "PHOTO_BRIDGE_PORT")) {
        if (!owngrove::parsePort(env_port->c_str(), port)) {
            std::cerr << "Invalid port '" << *env_port << "'. Expected an integer in 1-65535."
                      << std::endl;
            return 1;
        }
    }

    std::cout << "OwnGrove Hub starting..." << std::endl;

    owngrove::HttpServer server;
    if (!server.start("0.0.0.0", port)) {
        std::cerr << "Failed to start HTTP server" << std::endl;
        return 1;
    }

    return 0;
}
