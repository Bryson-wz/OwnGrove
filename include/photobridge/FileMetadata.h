#pragma once

#include <string>
#include <cstdint>
namespace photobridge {

struct FileMetadata {
    std::string filename;
    std::string contentType;
    std::uintmax_t size;
    std::string uploadedAt;
};

} // namespace photobridge