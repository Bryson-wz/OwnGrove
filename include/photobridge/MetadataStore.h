#pragma once
#include "photobridge/FileMetadata.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace photobridge {
    class MetadataStore {
        public:
            explicit MetadataStore(std::filesystem::path metadata_path);
            bool appendFile(const FileMetadata& metadata) const;
            std::vector<FileMetadata> listFiles() const;
            std::string readAll() const;
            std::uint64_t countRecords() const;
        private:
            std::filesystem::path metadata_path_;
        };

} // namespace photobridge
