#include "photobridge/HttpServer.h"

#include <iostream>

int main()
{
    std::cout << "PhotoBridge starting..." << std::endl;

    photobridge::HttpServer server;
    if (!server.start("0.0.0.0", 8080)) {
        std::cerr << "Failed to start HTTP server" << std::endl;
        return 1;
    }

    return 0;
}
