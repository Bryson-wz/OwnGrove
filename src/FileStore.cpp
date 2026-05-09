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

} // namespace photobridge
