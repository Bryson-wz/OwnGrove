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
    const char* metadataIssueTypeToString(photobridge::MetadataIssueType type)
    {
        switch (type) {
            case photobridge::MetadataIssueType::MissingFile:
                return "MissingFile";
            case photobridge::MetadataIssueType::OrphanFile:
                return "OrphanFile";
        }

        return "Unknown";
    }

    std::string escapeJson(const std::string& text){
        std::string escaped;
        for (char ch : text) {
            switch (ch) {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    escaped += ch;
                    break;
            }
        }
        return escaped;
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
    server.Get("/api/files", [&file_store, &metadata_store, expected_token](const httplib::Request& req, httplib::Response& res) {
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        std::ostringstream json;
        json << "[";

        bool first = true;
        for (const auto& file : metadata_store.listFiles()) {
            if (!first) {
                json << ",";
            }

            json << "{";
            json << "\"name\":\"" << file.filename << "\",";
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
                    1,
                    "upload",
                    filename,
                    req.get_header_value("Content-Type"),
                    req.body.size(),
                    currentTimestamp(),
                    "completed"
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
                    1,
                    "upload",
                    file.filename,
                    file.content_type,
                    file.content.size(),
                    currentTimestamp(),
                    "completed"
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
    server.Get("/api/metadata", [&metadata_store, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const std::string metadata = metadata_store.readAll();
        res.set_content(metadata, "application/json");
    });
    server.Get("/api/metadata/count", [&metadata_store, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto count = metadata_store.countRecords();
        std::ostringstream json;
        json << "{";
        json << "\"count\":" << count;
        json << "}";
        res.set_content(json.str(), "application/json");
    });

    server.Get("/api/metadata/audit", [&metadata_store, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto issues = metadata_store.auditAgainstUploads("data/uploads");
        std::ostringstream json;
        json << "[";
        bool first = true;
        for(const auto& issue:issues){
            if(!first){
                json << ",";
            }
            json << "{";
            json << "\"type\":\"" << metadataIssueTypeToString(issue.type) << "\",";
            json << "\"filename\":\"" << escapeJson(issue.filename) << "\"";
            json << "}";
            first = false;
        }
        json << "]";
        res.set_content(json.str(), "application/json");
    });
    server.Post("/api/metadata/mark-missing", [&metadata_store, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto filename = req.get_param_value("filename");
        if(filename.empty()||
            filename.find("..") != std::string::npos ||
            filename.find('/') != std::string::npos ||
            filename.find('\\') != std::string::npos){
            res.status = 400;
            res.set_content("Invalid filename", "text/plain");
            return;
        }
        if(!metadata_store.appendStatusChange(filename, "mark-missing", "missing", currentTimestamp())){
            res.status = 500;
            res.set_content("Failed to mark file as missing", "text/plain");
            return;
        }
        res.status = 200;
        res.set_content("File marked as missing", "text/plain");
        return;
    });
    server.Post("/api/metadata/repair",[&metadata_store,expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        int repaired_count = 0;
        const auto issues = metadata_store.auditAgainstUploads("data/uploads");
        for(const auto &issue:issues){
            if(issue.type != MetadataIssueType::MissingFile){
                continue;
            }
            if(!metadata_store.appendStatusChange(issue.filename, "repair-missing", "missing", currentTimestamp())){
                res.status = 500;
                res.set_content("Failed to repair file", "text/plain");
                return;
            }
            repaired_count++;
        }
        const auto remaining_issues = metadata_store.auditAgainstUploads("data/uploads");
        std::ostringstream json;
        json << "{";
        json << "\"repaired_missing\":" << repaired_count<<",";
        json << "\"remaining_issues\":" << remaining_issues.size();
        json << "}";
        res.status = 200;
        res.set_content(json.str(), "application/json");
    });
    server.Post("/api/metadata/repair-missing", [&metadata_store, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
    
        const auto filename = req.get_param_value("filename");
        if (filename.empty() ||
            filename.find("..") != std::string::npos ||
            filename.find('/') != std::string::npos ||
            filename.find('\\') != std::string::npos) {
            res.status = 400;
            res.set_content("Invalid filename", "text/plain");
            return;
        }
    
        if (!metadata_store.appendStatusChange(filename, "repair-missing", "missing", currentTimestamp())) {
            res.status = 500;
            res.set_content("Failed to repair missing file", "text/plain");
            return;
        }
    
        res.status = 200;
        res.set_content("Missing file repaired", "text/plain");
        return;
    });
    
    server.Post("/api/files/delete", [&metadata_store, expected_token, &file_store](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto filename = req.get_param_value("filename");
        if(filename.empty()||
            filename.find("..") != std::string::npos ||
            filename.find('/') != std::string::npos ||
            filename.find('\\') != std::string::npos){
            res.status = 400;
            res.set_content("Invalid filename", "text/plain");
            return;
        }
        switch(file_store.deleteFile(filename)){
            case DeleteResult::Success:
                res.status = 200;
                break;
            case DeleteResult::InvalidFilename:
                res.status = 400;
                res.set_content("Invalid filename", "text/plain");
                return;
            case DeleteResult::NotFound:
                res.status = 404;
                res.set_content("File not found", "text/plain");
                return;
            case DeleteResult::DeleteFailed:
                res.status = 500;
                res.set_content("Failed to delete file", "text/plain");
                return;
        }
        if(!metadata_store.appendStatusChange(filename, "delete", "deleted", currentTimestamp())){
            res.status = 500;
            res.set_content("Failed to mark file as deleted", "text/plain");
            return;
        }
        res.status = 200;
        res.set_content("File deleted", "text/plain");
        return;
    });
    server.Post("/api/metadata/compact", [&metadata_store, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        if(!metadata_store.compact()){
            res.status = 500;
            res.set_content("Failed to compact metadata", "text/plain");
            return;
        }
        res.status = 200;
        res.set_content("Metadata compacted", "text/plain");
        return;
    });
    std::cout << "Listening on http://" << host << ":" << port << std::endl;
    return server.listen(host, port);
}

}
