#pragma once
#include "owngrove/FileMetadata.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace owngrove {
    enum class MetadataIssueType {
        MissingFile,
        OrphanFile,
    };
    struct MetadataIssue {
        MetadataIssueType type;
        std::string filename;
    };
    struct MetadataReplayResult{
        std::vector<FileMetadata> records;
        std::uint64_t skipped_records = 0;
        std::uint64_t total_records = 0;
    };

    class MetadataStore {
        public:
            explicit MetadataStore(std::filesystem::path metadata_path);
            bool appendFile(const FileMetadata& metadata) const;
            std::vector<FileMetadata> listFiles() const;
            std::string readAll() const;
            std::uint64_t countRecords() const;
            std::vector<MetadataIssue> auditAgainstUploads(const std::filesystem::path& upload_dir) const;
            bool appendStatusChange(
                const std::string& filename,
                const std::string& op,
                const std::string& status,
                const std::string& timestamp
            ) const;
            std::vector<FileMetadata> listLatestRecords() const;
            bool compact() const;
        private:
            std::filesystem::path metadata_path_;
            MetadataReplayResult replayMetadata() const;
            mutable std::mutex mutex_;
        };

} // namespace owngrove
