#include "photobridge/LocalStorageBackend.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <utility>

namespace photobridge {
    LocalStorageBackend::LocalStorageBackend(std::filesystem::path root_dir)
        : root_dir_(std::move(root_dir))
    {
    }
    bool LocalStorageBackend::writeObject(const std::string& key, const std::string& data)
    {
        if(!isSafeKey(key)){
            return false;
        }
        const auto path = resolveKey(key);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        if(!file.is_open()){
            return false;
        }
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        return file.good();
    }
    ReadObjectResult LocalStorageBackend::readObject(const std::string& key) const{
        if(!isSafeKey(key)){
            return ReadObjectResult{false, ""};
        }
        const auto path = resolveKey(key);
        std::ifstream file(path, std::ios::binary);
        if(!file.is_open()){
            return ReadObjectResult{false, ""};
        }
        std::stringstream buffer; 
        buffer << file.rdbuf();
        return ReadObjectResult{true, buffer.str()};
    }
    bool LocalStorageBackend::deleteObject(const std::string& key) {
        if(!isSafeKey(key)){
            return false;
        }
        const auto path = resolveKey(key);
        return std::filesystem::remove(path);
    }
    bool LocalStorageBackend::existsObject(const std::string& key) const{
        if(!isSafeKey(key)){
            return false;
        }
        const auto path = resolveKey(key);
        return std::filesystem::exists(path);
    }
    ObjectSizeResult LocalStorageBackend::objectSize(const std::string& key) const{
        if(!isSafeKey(key)){
            return {};
        }
        const auto path = resolveKey(key);
        if(!std::filesystem::exists(path)){
            return {};
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if(ec){
            return {};
        }
        return ObjectSizeResult{true, size};
    }
    ObjectStatResult LocalStorageBackend::statObject(const std::string& key) const{
        if(!isSafeKey(key)){
            return {};
        }
        const auto path = resolveKey(key);
        if(!std::filesystem::exists(path)){
            return {};
        }
        std::error_code ec;
        const auto last_modified = std::filesystem::last_write_time(path, ec);
        if(ec){
            return {};
        }
        const auto size = std::filesystem::file_size(path, ec);
        if(ec){
            return {};
        }
        return ObjectStatResult{true, size, last_modified};
    }
    std::vector<std::string> LocalStorageBackend::listKeys(const std::string& prefix) const{
        if(!isSafeKey(prefix)){
            return {};
        }
        const auto path = resolveKey(prefix);
        std::vector<std::string> keys;
        if(!std::filesystem::exists(path)){
            return keys;
        }

        std::error_code ec;
        for(const auto& entry : std::filesystem::recursive_directory_iterator(path, ec)){
            if(ec){
                break;
            }
            if(!entry.is_regular_file()){
                continue;
            }

            const auto relative = std::filesystem::relative(entry.path(), root_dir_, ec);
            if(ec){
                continue;
            }
            keys.push_back(relative.generic_string());
        }
        return keys;
    }
    bool LocalStorageBackend::deletePrefix(const std::string& prefix) {
        if(!isSafeKey(prefix)){
            return false;
        }
        const auto path = resolveKey(prefix);
        return std::filesystem::remove_all(path);
    }
    bool LocalStorageBackend::isSafeKey(const std::string& key) const{
        return !key.empty() 
            && key.front() !='/' 
            && key.front() != '\\' 
            && key.find("..") == std::string::npos 
            && key.find("//") == std::string::npos 
            && key.find(":") == std::string::npos;
    }
    std::filesystem::path LocalStorageBackend::resolveKey(const std::string& key) const{
        return root_dir_ / key;
    }
}
