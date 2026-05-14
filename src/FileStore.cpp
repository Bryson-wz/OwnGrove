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
    SaveResult FileStore::saveFile(const std::string& filename, const std::string& content) const{
        namespace fs = std::filesystem;

        if(!isSafeFilename(filename)){
            return SaveResult::InvalidFilename;
        }
        fs::create_directories(upload_dir_);
        const auto filepath = getFilePath(filename);

        if(fs::exists(filepath)){
            return SaveResult::FileExists;
        }
        if(content.size() > 1024 * 1024 * 500){ //500MB
            return SaveResult::FileTooLarge;
        }
        std::ofstream file(filepath, std::ios::binary);
        if(!file.is_open()){
            return SaveResult::SaveFailed;
        }
        file.write(content.data(),static_cast<std::streamsize>(content.size()));
        return file.good() ? SaveResult::Success : SaveResult::SaveFailed;
    }

    bool FileStore::isSafeFilename(const std::string& filename) const{
        return !filename.empty()
            && filename.find("..") == std::string::npos
            && filename.find_first_of("/\\") == std::string::npos;
        }
    std::filesystem::path FileStore::getFilePath(const std::string& filename) const{
        if(!isSafeFilename(filename)){
            return {};
        }
        return upload_dir_ / filename;
        }
    DeleteResult FileStore::deleteFile(const std::string& filename) const
    {
        if (!isSafeFilename(filename)) {
            return DeleteResult::InvalidFilename;
        }
    
        const auto path = getFilePath(filename);
        if (!std::filesystem::exists(path)) {
            return DeleteResult::NotFound;
        }
    
        std::error_code ec;
        const bool removed = std::filesystem::remove(path, ec);
        if (ec || !removed) {
            return DeleteResult::DeleteFailed;
        }
        
        return DeleteResult::Success;
    }
        
} // namespace photobridge
