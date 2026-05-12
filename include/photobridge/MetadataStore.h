#pragma once
#include "photobridge/FileMetadata.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace photobridge {
    enum class MetadataIssueType {
        MissingFile,
        OrphanFile,
    };
    struct MetadataIssue {
        MetadataIssueType type;
        std::string filename;
    };
    class MetadataStore {
        public:
            explicit MetadataStore(std::filesystem::path metadata_path);
            bool appendFile(const FileMetadata& metadata) const;
            std::vector<FileMetadata> listFiles() const;
            std::string readAll() const;
            std::uint64_t countRecords() const;
            std::vector<MetadataIssue> auditAgainstUploads(const std::filesystem::path& upload_dir) const;
        private:
            std::filesystem::path metadata_path_;
        };

} // namespace photobridge
