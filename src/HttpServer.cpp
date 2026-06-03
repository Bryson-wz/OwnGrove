#include "photobridge/HttpServer.h"
#include "photobridge/MetadataStore.h"
#include "photobridge/FileStore.h"
#include "photobridge/ChunkUploadStore.h"
#include "photobridge/LocalStorageBackend.h"
#include "photobridge/ReplicaStorageBackend.h"
#include "photobridge/StorageNode.h"

#include "httplib.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <vector>


namespace{
    bool isPerfLoggingEnabled() {
        static const bool enabled = [] {
            const char* flag = std::getenv("PHOTO_BRIDGE_PERF");
            return flag != nullptr && std::string(flag) == "1";
        }();
        return enabled;
    }

    class ScopedTimer {
        public:
            explicit ScopedTimer(std::string name)
                : name_(std::move(name)),
                  start_(std::chrono::steady_clock::now()) {}
        
            ~ScopedTimer() {
                if (!isPerfLoggingEnabled()) {
                    return;
                }
                const auto end = std::chrono::steady_clock::now();
                const auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();
        
                std::cout << "[perf] " << name_ << " elapsed_ms=" << elapsed_ms << "\n";
            }
        
        private:
            std::string name_;
            std::chrono::steady_clock::time_point start_;
        };
    
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
    httplib::Server server;
    const std::string expected_token = getTokenFromEnv();
    FileStore file_store("data/uploads");
    MetadataStore metadata_store("data/metadata/files.jsonl");

    LocalStorageBackend shard0("data/shards/shard_0");
    LocalStorageBackend shard1("data/shards/shard_1");
    LocalStorageBackend shard2("data/shards/shard_2");

    StorageNode node0("node-0", shard0);
    StorageNode node1("node-1", shard1);
    StorageNode node2("node-2", shard2);

    std::vector<StorageNode*> nodes = {&node0, &node1, &node2};
    ReplicaStorageBackend storage_backend(nodes, 2, 2);
    ChunkUploadStore chunk_upload_store("data/uploads_tmp", "data/uploads", storage_backend);

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
    server.Post("/api/uploads/init",[&chunk_upload_store, expected_token](const httplib::Request& req, httplib::Response& res){
        ScopedTimer timer("uploads.init");
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto filename = req.get_param_value("filename");
        const auto size_text = req.get_param_value("size");
        const auto chunk_size_text = req.get_param_value("chunk_size");
        std::uintmax_t total_size = 0;
        std::uintmax_t chunk_size = 0;
        try{
            total_size = static_cast<std::uintmax_t>(std::stoull(size_text));
            chunk_size = static_cast<std::uintmax_t>(std::stoull(chunk_size_text));
        }catch(...){
            res.status = 400;
            res.set_content("Invalid size or chunk size", "text/plain");
            return;
        }
        const auto init_result = chunk_upload_store.initSession(
            filename,
            total_size,
            chunk_size
        );
        switch(init_result.result){
            case InitResult::Success: {
                const auto& session = init_result.session;

                std::ostringstream json;
                json << "{"
                << "\"result\":\"success\","
                << "\"session_id\":\"" << session.session_id << "\","
                << "\"filename\":\"" << session.filename << "\","
                << "\"total_size\":" << session.total_size << ","
                << "\"chunk_size\":" << session.chunk_size << ","
                << "\"chunk_count\":" << session.chunk_count << ","
                << "\"status\":\"" << session.status << "\""
                << "}";
                
                res.status = 201;
                res.set_content(json.str(),"application/json");
                return;
            }
            case InitResult::InvalidFilename: {
                res.status = 400;
                res.set_content("Invalid filename", "text/plain");
                return;
            }
            case InitResult::InvalidSize: {
                res.status = 400;
                res.set_content("Invalid size", "text/plain");
                return;
            }
            case InitResult::FileExists: {
                res.status = 409;
                res.set_content("File already exists", "text/plain");
                return;
            }
            case InitResult::InitFailed: {
                res.status = 500;
                res.set_content("Failed to initialize upload session", "text/plain");
                return;
            }
        }
    });
    server.Post("/api/uploads/chunk",[&chunk_upload_store, expected_token](const httplib::Request& req, httplib::Response& res){
        ScopedTimer timer("uploads.chunk");
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto session_id = req.get_param_value("session_id");
        const auto index_text = req.get_param_value("index");
        const auto chunk = req.body;
        std::uintmax_t index;
        try{
            index = static_cast<std::uintmax_t>(std::stoull(index_text));
        }catch(...){
            res.status = 400;
            res.set_content("Invalid index", "text/plain");
            return;
        }
        const auto save_result = chunk_upload_store.saveChunk(session_id, index, chunk);
        switch(save_result){
            case SaveChunkResult::Success: {
                std::ostringstream json;
                json << "{\"result\":\"success\"}";
                res.status = 200;
                res.set_content(json.str(), "application/json");
                return;
            }
            case SaveChunkResult::InvalidSession: {
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Invalid session\"}";
                res.status = 400;
                res.set_content(json.str(), "application/json");
                return;
            }
            case SaveChunkResult::InvalidIndex: {
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Invalid index\"}";
                res.status = 400;
                res.set_content(json.str(), "application/json");
                return;
            }
            case SaveChunkResult::EmptyChunk: {
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Empty chunk\"}";
                res.status = 400;
                res.set_content(json.str(), "application/json");
                return;
            }
            case SaveChunkResult::SaveFailed: {
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Failed to save chunk\"}";
                res.status = 500;
                res.set_content(json.str(), "application/json");
                return;
            }
            case SaveChunkResult::InvalidSize: {
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Invalid chunk size\"}";
                res.status = 400;
                res.set_content(json.str(), "application/json");
                return;
            }
            case SaveChunkResult::Conflict: {
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Chunk conflict\"}";
                res.status = 409;
                res.set_content(json.str(), "application/json");
                return;
            }
            
        }
    });
    server.Post("/api/uploads/abort",[&chunk_upload_store, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto session_id = req.get_param_value("session_id");
        const auto abort_result = chunk_upload_store.abortUpload(session_id);
        switch(abort_result){
            case AbortUploadResult::Success: {
                res.status = 200;
                std::ostringstream json;
                json << "{\"result\":\"success\"}";
                res.set_content(json.str(), "application/json");
                return;
            }
            case AbortUploadResult::InvalidSession: {
                res.status = 400;
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Invalid session\"}";
                res.set_content(json.str(), "application/json");
                return;
            }
            case AbortUploadResult::NotFound: {
                res.status = 404;
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Session not found\"}";
                res.set_content(json.str(), "application/json");
                return;
            }
            case AbortUploadResult::AbortFailed: {
                res.status = 500;
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Failed to abort upload\"}";
                res.set_content(json.str(), "application/json");
                return;
            }
        }
    });
    server.Post("/api/uploads/cleanup-expired",[&chunk_upload_store, expected_token](const httplib::Request& req, httplib::Response& res) {
            if (!isAuthorized(req, expected_token)) {
                res.status = 401;
                res.set_content("Unauthorized", "text/plain");
                return;
            }
    
            const auto max_age_text = req.get_param_value("max_age_seconds");
            std::uintmax_t max_age_seconds = 0;
    
            try {
                max_age_seconds = static_cast<std::uintmax_t>(std::stoull(max_age_text));
            } catch (...) {
                res.status = 400;
                res.set_content(
                    "{\"result\":\"error\",\"error\":\"Invalid max_age_seconds\"}",
                    "application/json"
                );
                return;
            }
    
            if (max_age_seconds == 0) {
                res.status = 400;
                res.set_content(
                    "{\"result\":\"error\",\"error\":\"Invalid max_age_seconds\"}",
                    "application/json"
                );
                return;
            }
    
            const auto result = chunk_upload_store.cleanupExpiredUploads(max_age_seconds);
    
            std::ostringstream json;
            json << "{"
                 << "\"result\":\"success\","
                 << "\"removed_count\":" << result.removed_count << ","
                 << "\"failed_count\":" << result.failed_count
                 << "}";
    
            res.status = 200;
            res.set_content(json.str(), "application/json");
        }
    );
    server.Post("/api/uploads/complete",[&chunk_upload_store, expected_token, &metadata_store](const httplib::Request& req, httplib::Response& res){
        ScopedTimer timer("uploads.complete");
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto session_id = req.get_param_value("session_id");
        const auto complete_result = chunk_upload_store.completeUpload(session_id);
        switch(complete_result.result){
            case CompleteUploadResult::Success: {
                if (!metadata_store.appendFile(FileMetadata{
                    1,
                    "chunk-upload-complete",
                    complete_result.filename,
                    "",
                    complete_result.size,
                    currentTimestamp(),
                    "completed"
                })) {
                    res.status = 500;
                    res.set_content(
                        "{\"result\":\"error\",\"error\":\"File merged but metadata write failed\"}",
                        "application/json"
                    );
                    return;
                }
                chunk_upload_store.cleanupSession(session_id);
                std::ostringstream json;
                json << "{"
                     << "\"result\":\"success\","
                     << "\"filename\":\"" << complete_result.filename << "\","
                     << "\"size\":" << complete_result.size
                     << "}";
                res.status = 200;
                res.set_content(json.str(), "application/json");
                return;
            }
            case CompleteUploadResult::InvalidSession: {
                res.status = 400;
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Invalid session\"}";
                res.set_content(json.str(), "application/json");
                return;
            }
            case CompleteUploadResult::SaveFailed: {
                res.status = 500;
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Failed to complete upload\"}";
                res.set_content(json.str(), "application/json");
                return;
            }
            case CompleteUploadResult::MissingChunk: {
                res.status = 409;
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Missing chunk\"}";
                res.set_content(json.str(), "application/json");
                return;
            }
            case CompleteUploadResult::MergeFailed: {
                res.status = 500;
                std::ostringstream json;
                json << "{\"result\":\"error\",\"error\":\"Failed to merge chunks\"}";
                res.set_content(json.str(), "application/json");
                return;
            }
        }
    });
    server.Get("/api/uploads/status",[&chunk_upload_store, expected_token](const httplib::Request& req, httplib::Response& res){
        ScopedTimer timer("uploads.status");
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }

        const auto session_id = req.get_param_value("session_id");
        const auto status = chunk_upload_store.getUploadStatus(session_id);
        const auto& session = status.session;
        if(status.result != UploadStatusResult::Success){
            res.status = 400;
            std::ostringstream json;
            json << "{\"result\":\"error\",\"error\":\"Failed to get status\"}";
            res.set_content(json.str(), "application/json");
            return;
        }
        std::ostringstream json;
        json << "{"
             << "\"result\":\"success\","
             << "\"session_id\":\"" << escapeJson(session.session_id) << "\","
             << "\"filename\":\"" << escapeJson(session.filename) << "\","
             << "\"total_size\":" << session.total_size << ","
             << "\"chunk_size\":" << session.chunk_size << ","
             << "\"chunk_count\":" << session.chunk_count << ","
             << "\"status\":\"" << escapeJson(session.status) << "\","
             << "\"uploaded_count\":" << status.uploaded_indexes.size() << ","
             << "\"missing_count\":" << status.missing_indexes.size() << ",";

        json << "\"uploaded_indexes\":[";
        {
            bool first = true;
            for(const auto& index : status.uploaded_indexes){
                if(!first){
                    json << ",";
                }
                json << index;
                first = false;
            }
            json << "],";
            json << "\"missing_indexes\":[";
            first = true;
            for(const auto& index : status.missing_indexes){
                if(!first){
                    json << ",";
                }
                json << index;
                first = false;
            }
        }
        json << "]";
        json << "}";

        res.status = 200;
        res.set_content(json.str(), "application/json");
        return;
    });
    server.Get("/api/storage/nodes",[&storage_backend, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        std::ostringstream json;
        json << "[";
        bool first = true;
        for(const auto& node : storage_backend.nodes()){
            if(!first){
                json << ",";
            }
            json << "{\"id\":\"" << node->id()
                 << "\",\"available\":" << (node->isAvailable() ? "true" : "false")
                 << "}";
            first = false;
        }
        json << "]";
        res.set_content(json.str(), "application/json");
        return;
    });
    server.Post("/api/storage/node/availability",[&storage_backend, expected_token](const httplib::Request& req, httplib::Response& res){
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto node_id = req.get_param_value("node_id");
        const auto available = req.get_param_value("available");
        if(node_id.empty() || available.empty()){
            res.status = 400;
            res.set_content("Node ID and availability are required", "text/plain");
            return;
        }
        bool new_available = false;
        if(available == "true"){
            new_available = true;
        }else if(available == "false"){
            new_available = false;
        }else{
            res.status = 400;
            res.set_content("Invalid availability", "text/plain");
            return;
        }
        if(!storage_backend.setNodeAvailability(node_id, new_available)){
            res.status = 404;
            res.set_content("Node not found", "text/plain");
            return;
        }
        res.status = 200;
        std::ostringstream json;
        json << "{\"result\":\"success\"}";
        res.set_content(json.str(), "application/json");
        return;
    });
    server.Get("/api/storage/placement", [&storage_backend, expected_token](const httplib::Request& req, httplib::Response& res) {
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
    
        const auto key = req.get_param_value("key");
        if (key.empty()) {
            res.status = 400;
            res.set_content("Key is required", "text/plain");
            return;
        }
    
        const auto node_ids = storage_backend.replicaNodeIdsForKey(key);
    
        std::ostringstream json;
        json << "{\"key\":\"" << escapeJson(key) << "\",\"nodes\":[";
        for (std::size_t i = 0; i < node_ids.size(); ++i) {
            if (i > 0) {
                json << ",";
            }
            json << "\"" << escapeJson(node_ids[i]) << "\"";
        }
        json << "]}";
    
        res.status = 200;
        res.set_content(json.str(), "application/json");
    });
    server.Get("/api/storage/replicas/audit", [&storage_backend, expected_token](const httplib::Request& req, httplib::Response& res) {
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
        const auto key = req.get_param_value("key");
        if (key.empty()) {
            res.status = 400;
            res.set_content("Key is required", "text/plain");
            return;
        }
        const auto items = storage_backend.auditReplicasForKey(key);
        std::ostringstream json;
        json << "{\"key\":\"" << escapeJson(key) << "\",\"replicas\":[";
        for(std::size_t i = 0; i < items.size(); ++i){
            if(i > 0){
                json << ",";
            }
            json << "{"
            << "\"node_id\":\"" << escapeJson(items[i].node_id) << "\","
            << "\"available\":" << (items[i].available ? "true" : "false") << ","
            << "\"exists\":" << (items[i].exists ? "true" : "false") << ","
            << "\"size\":" << items[i].size
            << "}";
        }
        json << "]}";
        res.set_content(json.str(), "application/json");
        return;
    });
    server.Post("/api/storage/replicas/repair", [&storage_backend, expected_token](const httplib::Request& req, httplib::Response& res) {
        if (!isAuthorized(req, expected_token)) {
            res.status = 401;
            res.set_content("Unauthorized", "text/plain");
            return;
        }
    
        const auto key = req.get_param_value("key");
        if (key.empty()) {
            res.status = 400;
            res.set_content("Key is required", "text/plain");
            return;
        }
    
        const auto result = storage_backend.repairReplicasForKey(key);
    
        std::ostringstream json;
        json << "{"
             << "\"result\":\"success\","
             << "\"repaired\":" << (result.repaired ? "true" : "false") << ","
             << "\"repaired_count\":" << result.repaired_count << ","
             << "\"source_node_id\":\"" << escapeJson(result.source_node_id) << "\""
             << "}";
    
        res.status = 200;
        res.set_content(json.str(), "application/json");
    });
    std::cout << "Listening on http://" << host << ":" << port << std::endl;
    return server.listen(host, port);
}

}
