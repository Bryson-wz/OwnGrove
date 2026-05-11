#include "photobridge/MetadataStore.h"

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


} // namespace photobridge
