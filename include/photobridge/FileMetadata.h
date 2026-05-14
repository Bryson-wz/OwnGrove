#pragma once

#include <string>
#include <cstdint>
namespace photobridge {

struct FileMetadata {
    int schemaVersion;
    std::string op;
    std::string filename;
    std::string contentType;
    std::uintmax_t size;
    std::string uploadedAt;
    std::string status;
};

} // namespace photobridge
