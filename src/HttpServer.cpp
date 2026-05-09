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
    server.Get(R"(/api/files/(.+)/download)",[&file_store](const httplib::Request& req, httplib::Response& res){
        const std::string filename = req.matches[1];
        const auto filepath = file_store.getFilePath(filename);
        if(filepath.empty()){
            res.status = 400;
            res.set_content("Invalid filename", "text/plain");
            return;
        }
        std::ifstream file(filepath, std::ios::binary);
        if(!file.is_open()){
            res.status = 500;
            res.set_content("Failed to open file", "text/plain");
            return;
        }
        std::ostringstream buffer;
        buffer<< file.rdbuf();
        res.set_header(
                "Content-Disposition",
                "attachment; filename=\"" + filename + "\""
        );
        res.set_content(buffer.str(),"application/octet-stream");
    });
    std::cout << "Listening on http://" << host << ":" << port << std::endl;
    return server.listen(host, port);
}

}
