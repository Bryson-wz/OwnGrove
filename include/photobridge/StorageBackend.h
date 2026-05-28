#pragma once

#include <string>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace photobridge {
    struct ReadObjectResult{
        bool ok = false;
        std::string data;
    };
    struct ObjectSizeResult{
        bool ok = false;
        std::uintmax_t size = 0;
    };
    struct ObjectStatResult{
        bool ok = false;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type last_modified;
    };
    class StorageBackend {
    public: 
        virtual ~StorageBackend() = default;
    
        virtual bool writeObject(const std::string& key, const std::string& data) = 0;
        virtual ReadObjectResult readObject(const std::string& key) const = 0;
        virtual bool deleteObject(const std::string& key) = 0;
        virtual bool existsObject(const std::string& key) const = 0;
        virtual ObjectSizeResult objectSize(const std::string& key) const = 0;
        virtual ObjectStatResult statObject(const std::string& key) const = 0;
        virtual std::vector<std::string> listKeys(const std::string& prefix) const = 0;
        virtual bool deletePrefix(const std::string& prefix) = 0;
    };
}