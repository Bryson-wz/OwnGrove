#include "photobridge/MetadataStore.h"

#include <fstream>
#include <sstream>
#include <utility>

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
}
namespace photobridge {
    MetadataStore::MetadataStore(std::filesystem::path metadata_path)
    : metadata_path_(std::move(metadata_path))
    {

    }
    bool MetadataStore::appendFile(const FileMetadata& metadata) const{
        std::filesystem::create_directories(metadata_path_.parent_path());
        std::ofstream file(metadata_path_,std::ios::app);
        if(!file.is_open()){
            return false;
        }
        file << "{"
        << "\"filename\":\"" << escapeJson(metadata.filename) << "\","
        << "\"content_type\":\"" << escapeJson(metadata.contentType) << "\","
        << "\"size\":" << metadata.size << ","
        << "\"uploaded_at\":\"" << escapeJson(metadata.uploadedAt) << "\""
        << "}\n";

        return file.good();
    }
    std::string MetadataStore::readAll() const
    {
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
        std::ifstream file(metadata_path_);
        if (!file.is_open()) {
            return {};
        }

        std::vector<FileMetadata> files;
        std::string line;

        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }

            try {
                files.push_back(FileMetadata{
                    extractStringField(line, "filename"),
                    extractStringField(line, "content_type"),
                    extractUintField(line, "size"),
                    extractStringField(line, "uploaded_at")
                });
            } catch (...) {
                continue;
            }
        }

        return files;
    }

} // namespace photobridge
