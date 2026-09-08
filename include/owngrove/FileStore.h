#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace owngrove {

struct FileInfo {
    std::string name;
    std::uintmax_t size;
};

enum class SaveResult {
    Success,
    InvalidFilename,
    FileTooLarge,
    FileExists,
    SaveFailed,
};
enum class DeleteResult {
    Success,
    InvalidFilename,
    NotFound,
    DeleteFailed
};
class FileStore {
public:
    explicit FileStore(std::filesystem::path upload_dir);
    std::filesystem::path getFilePath(const std::string& filename) const;
    std::vector<FileInfo> listFiles() const;
    SaveResult saveFile(const std::string& filename, const std::string& content) const;
    DeleteResult deleteFile(const std::string& filename) const;
private:
    bool isSafeFilename(const std::string& filename) const;
    std::filesystem::path upload_dir_;
};

} // namespace owngrove
