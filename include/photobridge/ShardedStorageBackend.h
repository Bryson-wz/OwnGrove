#pragma once

#include "photobridge/StorageBackend.h"

#include <filesystem>
#include <string>
#include <cstdint>
#include <vector>
#include <utility>
#include <string_view>

namespace photobridge {
    class ShardedStorageBackend : public StorageBackend {
    public:
        explicit ShardedStorageBackend(std::vector<StorageBackend*> shards);

        bool writeObject(const std::string& key, const std::string& data) override;
        ReadObjectResult readObject(const std::string& key) const override;
        bool deleteObject(const std::string& key) override;
        bool existsObject(const std::string& key) const override;
        ObjectSizeResult objectSize(const std::string& key) const override;
        ObjectStatResult statObject(const std::string& key) const override;
        std::vector<std::string> listKeys(const std::string& prefix) const override;
        bool deletePrefix(const std::string& prefix) override;

    private:
        std::size_t shardIndexForKey(const std::string& key) const;
        StorageBackend& shardForKey(const std::string& key);
        const StorageBackend& shardForKey(const std::string& key) const;

        std::vector<StorageBackend*> shards_;
    };
}