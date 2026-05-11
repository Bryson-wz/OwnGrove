#pragma once
#include "photobridge/FileMetadata.h"

#include <filesystem>
#include <optional>
#include <vector>
#include <fstream>

namespace photobridge {
    class MetadataStore {
        public:
            explicit MetadataStore(std::filesystem::path metadata_path);
            bool appendFile(const FileMetadata& metadata) const;

        private:
            std::filesystem::path metadata_path_;
        };

} // namespace photobridge
