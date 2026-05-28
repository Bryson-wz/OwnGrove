#pragma once

#include "photobridge/StorageBackend.h"

#include <filesystem>
#include <string>
#include <cstdint>
#include <vector>


namespace photobridge {
    class LocalStorageBackend : public StorageBackend {
    public:
        explicit LocalStorageBackend(std::filesystem::path root_dir);

        bool writeObject(const std::string& key, const std::string& data) override;
        ReadObjectResult readObject(const std::string& key) const override;
        bool deleteObject(const std::string& key) override;
        bool existsObject(const std::string& key) const override;
        ObjectSizeResult objectSize(const std::string& key) const override;
        ObjectStatResult statObject(const std::string& key) const override;
        std::vector<std::string> listKeys(const std::string& prefix) const override;
        bool deletePrefix(const std::string& prefix) override;
    private:
        std::filesystem::path resolveKey(const std::string& key) const;
        bool isSafeKey(const std::string& key) const;
        std::filesystem::path root_dir_;
    };
}