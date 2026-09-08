#include "owngrove/MetadataStore.h"

#include <fstream>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <system_error>

namespace{
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

    // 轻量解析当前项目自己生成的固定格式 JSONL，不作为通用 JSON parser。
    std::string extractStringField(const std::string& line,const std::string& key){
        const std::string pattern = "\""+key+"\":\"";
        const auto start = line.find(pattern);
        if(start == std::string::npos){
            return "";
        }
        const auto value_start = start + pattern.length();
        const auto value_end = line.find("\"", value_start);
        if(value_end == std::string::npos){
            return "";
        }
        return line.substr(value_start, value_end - value_start);
    }

    std::uint64_t extractUintField(const std::string& line,const std::string& key){
        const std::string pattern = "\""+key+"\":";
        const auto start = line.find(pattern);
        if(start == std::string::npos){
            return 0;
        }
        const auto value_start = start + pattern.length();
        const auto value_end = line.find_first_of(",}", value_start);
        const auto value = line.substr(value_start, value_end - value_start);
        return static_cast<std::uint64_t>(std::stoull(value));
    }

    bool writeMetadataRecord(std::ostream& file, const owngrove::FileMetadata& metadata){
        file << "{"
        << "\"schema_version\":" << metadata.schemaVersion << ","
        << "\"op\":\"" << escapeJson(metadata.op) << "\","
        << "\"filename\":\"" << escapeJson(metadata.filename) << "\","
        << "\"content_type\":\"" << escapeJson(metadata.contentType) << "\","
        << "\"size\":" << metadata.size << ","
        << "\"uploaded_at\":\"" << escapeJson(metadata.uploadedAt) << "\","
        << "\"status\":\"" << escapeJson(metadata.status) << "\""
        << "}\n";

        return file.good();
    }
}
namespace owngrove {
    bool MetadataStore::appendStatusChange(
        const std::string& filename,
        const std::string& op,
        const std::string& status,
        const std::string& timestamp
    ) const{
        return appendFile(FileMetadata{1, op, filename, "", 0, timestamp, status});
    }

    MetadataStore::MetadataStore(std::filesystem::path metadata_path)
    : metadata_path_(std::move(metadata_path))
    {

    }
    bool MetadataStore::appendFile(const FileMetadata& metadata) const{
        std::lock_guard<std::mutex> lock(mutex_);
        std::error_code ec;
        std::filesystem::create_directories(metadata_path_.parent_path(), ec);
        std::ofstream file(metadata_path_,std::ios::app);
        if(!file.is_open()){
            return false;
        }
        return writeMetadataRecord(file, metadata);
    }
    std::string MetadataStore::readAll() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ifstream file(metadata_path_);
        if(!file.is_open())
        {
            return "";
        }
        std::ostringstream buffer;
        buffer<<file.rdbuf();
        return buffer.str();
    }
    std::uint64_t MetadataStore::countRecords() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ifstream file(metadata_path_);
        if(!file.is_open())
        {
            return 0;
        }
        std::uint64_t count = 0;
        std::string line;
        while(std::getline(file, line))
        {
            if(!line.empty())
            {
                count++;
            }
        }
        return count;
    }

    std::vector<FileMetadata> MetadataStore::listFiles() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<FileMetadata> files;
        const auto replay_result = replayMetadata();
    
        for (const auto& metadata : replay_result.records) {
            if (metadata.status == "completed") {
                files.push_back(metadata);
            }
        }
        return files;
    }
    std::vector<MetadataIssue> MetadataStore::auditAgainstUploads(const std::filesystem::path& upload_dir) const
    {
        std::vector<MetadataIssue> issues;
        std::unordered_set<std::string> metadata_filenames;

        for (const auto& file : listFiles()) {
            metadata_filenames.insert(file.filename);

            const auto upload_path = upload_dir / file.filename;
            if (!std::filesystem::exists(upload_path)) {
                issues.push_back(MetadataIssue{MetadataIssueType::MissingFile, file.filename});
            }
        }

        if (!std::filesystem::exists(upload_dir)) {
            return issues;
        }

        for (const auto& entry : std::filesystem::directory_iterator(upload_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const auto filename = entry.path().filename().string();
            if (!metadata_filenames.contains(filename)) {
                issues.push_back(MetadataIssue{MetadataIssueType::OrphanFile, filename});
            }
        }

        return issues;
    }
    std::vector<FileMetadata> MetadataStore::listLatestRecords() const{
        std::lock_guard<std::mutex> lock(mutex_);
        return  replayMetadata().records;
    }
    bool MetadataStore::compact() const{
        std::lock_guard<std::mutex> lock(mutex_);
        const auto replay_result = replayMetadata();
        if (replay_result.skipped_records > 0) {
            return false;
        }
        const auto& records = replay_result.records;

        std::error_code ec;
        std::filesystem::create_directories(metadata_path_.parent_path(), ec);
        auto tmp_path = metadata_path_;
        tmp_path += ".tmp";
        auto backup_path = metadata_path_;
        backup_path += ".bak";

        {
            std::ofstream file(tmp_path, std::ios::trunc);
            if(!file.is_open()){
                return false;
            }
            for(const auto& metadata : records){
                if(!writeMetadataRecord(file, metadata)){
                    return false;
                }
            }
        }

        const bool had_original = std::filesystem::exists(metadata_path_);
        if(had_original){
            std::filesystem::remove(backup_path, ec);
            ec.clear();
            std::filesystem::rename(metadata_path_, backup_path, ec);
            if(ec){
                std::filesystem::remove(tmp_path, ec);
                return false;
            }
        }

        ec.clear();
        std::filesystem::rename(tmp_path, metadata_path_, ec);
        if(ec){
            std::filesystem::remove(tmp_path, ec);
            if(had_original){
                std::error_code restore_ec;
                std::filesystem::rename(backup_path, metadata_path_, restore_ec);
            }
            return false;
        }
        if(had_original){
            std::filesystem::remove(backup_path, ec);
        }
        return true;
    }
    MetadataReplayResult MetadataStore::replayMetadata() const{
        MetadataReplayResult result;
        std::ifstream file(metadata_path_);
        if(!file.is_open())
        {
            return result;
        }
        std::unordered_map<std::string, FileMetadata> latest;
        std::string line;
        while(std::getline(file, line))
        {
            if(line.empty()){
                continue;
            }
            result.total_records++;
            try{
                const auto schema_version = extractUintField(line, "schema_version");
                if(schema_version != 1){
                    result.skipped_records++;
                    continue;
                }
                const auto filename = extractStringField(line, "filename");
                if(filename.empty()){
                    result.skipped_records++;
                    continue;
                }
                FileMetadata metadata{
                    static_cast<int>(schema_version),
                    extractStringField(line, "op"),
                    filename,
                    extractStringField(line, "content_type"),
                    extractUintField(line, "size"),
                    extractStringField(line, "uploaded_at"),
                    extractStringField(line, "status")
                };
                latest[filename] = metadata;
            } catch (...) {
                result.skipped_records++;
                continue;
            }
        }
        for(const auto& [filename, metadata]:latest){
            result.records.push_back(metadata);
        }
        return result;
    }
} // namespace owngrove
