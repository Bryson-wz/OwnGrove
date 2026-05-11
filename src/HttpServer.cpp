#include "photobridge/HttpServer.h"
#include "photobridge/MetadataStore.h"
#include "photobridge/FileStore.h"

#include "httplib.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>


namespace{
    std::string getTokenFromEnv(){
        const char* token = std::getenv("PHOTO_BRIDGE_TOKEN");
        return token ? token : "";
    }
    bool isAuthorized(const httplib::Request& req, const std::string& expected_token){
        if(expected_token.empty()){
            return false;
        }
        if(!req.has_param("token")){
            return false;
        }
        return req.get_param_value("token") == expected_token;
        }
    std::string currentTimestamp(){
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);

        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }
}
namespace photobridge {

bool HttpServer::start(const char* host, int port)
{
    // 创建HTTP服务器实例
    httplib::Server server;
    const std::string expected_token = getTokenFromEnv();
    // 创建文件存储实例
    FileStore file_store("data/uploads");

    // 创建元数据存储实例
    MetadataStore metadata_store("data/metadata/files.jsonl");
    // 创建健康检查路由
    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });
    // 创建主页路由
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
    // 创建文件列表路由
    server.Get("/api/files", [&file_store, expected_token](const httplib::Request& req, httplib::Response& res) {
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
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
    // 创建文件下载路由
    server.Get(R"(/api/files/(.+)/download)",[&file_store, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
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
    server.Post("/api/upload", [&file_store, &metadata_store, expected_token](const httplib::Request& req, httplib::Response& res) {
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
    
        const std::string filename = req.get_param_value("filename");
        if (filename.empty()) {
            res.status = 400;
            res.set_content("Filename is required", "text/plain");
            return;
        }
    
        if (req.body.empty()) {
            res.status = 400;
            res.set_content("File body is empty", "text/plain");
            return;
        }
    
        switch (file_store.saveFile(filename, req.body)) {
            case SaveResult::Success:
                if (!metadata_store.appendFile(FileMetadata{
                    filename,
                    req.get_header_value("Content-Type"),
                    req.body.size(),
                    currentTimestamp()
                })) {
                    res.status = 500;
                    res.set_content("File saved but metadata write failed", "text/plain");
                    return;
                }

                res.status = 201;
                res.set_content("File saved successfully", "text/plain");
                return;
            case SaveResult::InvalidFilename:
                res.status = 400;
                res.set_content("Invalid filename", "text/plain");
                return;
            case SaveResult::FileExists:
                res.status = 409;
                res.set_content("File already exists", "text/plain");
                return;
            case SaveResult::SaveFailed:
                res.status = 500;
                res.set_content("Failed to save file", "text/plain");
                return;
            case SaveResult::FileTooLarge:
                res.status = 413;
                res.set_content("Payload Too Large", "text/plain");
                return;
        }
    });
    //multipart/form-data
    server.Post("/api/upload-form",[&file_store, &metadata_store, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        if(!req.form.has_file("file")){
            res.status = 400;
            res.set_content("File is required", "text/plain");
            return;
        }
        const auto& file = req.form.get_file("file");

        switch (file_store.saveFile(file.filename, file.content)) {
            case SaveResult::Success:
                if (!metadata_store.appendFile(FileMetadata{
                    file.filename,
                    file.content_type,
                    file.content.size(),
                    currentTimestamp()
                })) {
                    res.status = 500;
                    res.set_content("File saved but metadata write failed", "text/plain");
                    return;
                }

                res.status = 201;
                res.set_content("File saved successfully", "text/plain");
                return;
            case SaveResult::InvalidFilename:
                res.status = 400;
                res.set_content("Invalid filename", "text/plain");
                return;
            case SaveResult::FileExists:
                res.status = 409;
                res.set_content("File already exists", "text/plain");
                return;
            case SaveResult::SaveFailed:
                res.status = 500;
                res.set_content("Failed to save file", "text/plain");
                return;
            case SaveResult::FileTooLarge:
                res.status = 413;
                res.set_content("Payload Too Large", "text/plain");
                return;
        }
    });
    std::cout << "Listening on http://" << host << ":" << port << std::endl;
    return server.listen(host, port);
}

}
