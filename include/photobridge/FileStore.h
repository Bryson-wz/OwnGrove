#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace photobridge {

struct FileInfo {
    std::string name;
    std::uintmax_t size;
};

class FileStore {
public:
    explicit FileStore(std::filesystem::path upload_dir);

    std::vector<FileInfo> listFiles() const;

private:
    std::filesystem::path upload_dir_;
};

} // namespace photobridge
