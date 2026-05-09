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
    std::filesystem::path getFilePath(const std::string& filename) const;
    std::vector<FileInfo> listFiles() const;

private:
    bool isSafeFilename(const std::string& filename) const;
    std::filesystem::path upload_dir_;
};

} // namespace photobridge
