#include "photobridge/HttpServer.h"

#include "photobridge/FileStore.h"

#include "httplib.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

namespace photobridge {

bool HttpServer::start(const char* host, int port)
{
    httplib::Server server;
    FileStore file_store("data/uploads");

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });

    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream index_file("web/index.html");
        if (!index_file.is_open()) {
            res.status = 404;
            res.set_content("index.html not found", "text/plain");
            return;
        }

        std::stringstream index_buffer;
        index_buffer << index_file.rdbuf();
        res.set_content(index_buffer.str(), "text/html");
    });

    server.Get("/api/files", [&file_store](const httplib::Request&, httplib::Response& res) {
        std::ostringstream json;
        json << "[";

        bool first = true;
        for (const auto& file : file_store.listFiles()) {
            if (!first) {
                json << ",";
            }

            json << "{";
            json << "\"name\":\"" << file.name << "\",";
            json << "\"size\":" << file.size;
            json << "}";

            first = false;
        }

        json << "]";
        res.set_content(json.str(), "application/json");
    });

    std::cout << "Listening on http://" << host << ":" << port << std::endl;
    return server.listen(host, port);
}

}
