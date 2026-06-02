#include "photobridge/ShardedStorageBackend.h"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace {

    std::uint64_t fnv1a64(std::string_view key) {
        std::uint64_t hash = 14695981039346656037ull;
        for (unsigned char ch : key) {
            hash ^= ch;
            hash *= 1099511628211ull;
        }
        return hash;
    }

}

namespace photobridge {

    ShardedStorageBackend::ShardedStorageBackend(std::vector<StorageBackend*> shards)
        : shards_(std::move(shards))
    {
        if (shards_.empty()) {
            throw std::invalid_argument("ShardedStorageBackend requires at least one shard");
        }

        for (const auto* shard : shards_) {
            if (shard == nullptr) {
                throw std::invalid_argument("ShardedStorageBackend shard must not be null");
            }
        }
    }

    std::size_t ShardedStorageBackend::shardIndexForKey(const std::string& key) const {
        return static_cast<std::size_t>(fnv1a64(key) % shards_.size());
    }

    StorageBackend& ShardedStorageBackend::shardForKey(const std::string& key) {
        return *shards_[shardIndexForKey(key)];
    }

    const StorageBackend& ShardedStorageBackend::shardForKey(const std::string& key) const {
        return *shards_[shardIndexForKey(key)];
    }

    bool ShardedStorageBackend::writeObject(const std::string& key, const std::string& data) {
        auto& shard = shardForKey(key);

        if (!shard.writeObject(key, data)) {
            shard.deleteObject(key);
            return false;
        }

        return true;
    }

    ReadObjectResult ShardedStorageBackend::readObject(const std::string& key) const {
        return shardForKey(key).readObject(key);
    }

    bool ShardedStorageBackend::deleteObject(const std::string& key) {
        return shardForKey(key).deleteObject(key);
    }

    bool ShardedStorageBackend::existsObject(const std::string& key) const {
        return shardForKey(key).existsObject(key);
    }

    ObjectSizeResult ShardedStorageBackend::objectSize(const std::string& key) const {
        return shardForKey(key).objectSize(key);
    }

    ObjectStatResult ShardedStorageBackend::statObject(const std::string& key) const {
        return shardForKey(key).statObject(key);
    }

    std::vector<std::string> ShardedStorageBackend::listKeys(const std::string& prefix) const {
        std::vector<std::string> keys;

        for (const auto* shard : shards_) {
            const auto shard_keys = shard->listKeys(prefix);
            keys.insert(keys.end(), shard_keys.begin(), shard_keys.end());
        }

        return keys;
    }

    bool ShardedStorageBackend::deletePrefix(const std::string& prefix) {
        bool ok = true;

        for (auto* shard : shards_) {
            ok = shard->deletePrefix(prefix) && ok;
        }

        return ok;
    }

}