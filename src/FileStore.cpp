#include "photobridge/FileStore.h"

#include <utility>

namespace photobridge {

FileStore::FileStore(std::filesystem::path upload_dir)
    : upload_dir_(std::move(upload_dir))
{
}

std::vector<FileInfo> FileStore::listFiles() const
{
    namespace fs = std::filesystem;

    fs::create_directories(upload_dir_);

    std::vector<FileInfo> files;

    for (const auto& entry : fs::directory_iterator(upload_dir_)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        files.push_back(FileInfo{
            entry.path().filename().string(),
            entry.file_size()
        });
    }

    return files;
    }
bool FileStore::saveFile(const std::string& filename, const std::string& content) const{
    namespace fs = std::filesystem;

    if(!isSafeFilename(filename)){
        return false;
    }
    fs::create_directories(upload_dir_);
    const auto filepath = getFilePath(filename);
    std::ofstream file(filepath, std::ios::binary);
    if(!file.is_open()){
        return false;
    }
    file.write(content.data(),static_cast<std::streamsize>(content.size()));
    return file.good();
}
bool FileStore::isSafeFilename(const std::string& filename) const{
    return !filename.empty() && filename.find_first_of("/\\") == std::string::npos;
    }
std::filesystem::path FileStore::getFilePath(const std::string& filename) const{
    if(!isSafeFilename(filename)){
        return {};
    }
    return upload_dir_ / filename;
    }
} // namespace photobridge
