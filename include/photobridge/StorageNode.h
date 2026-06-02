#pragma once

#include "photobridge/StorageBackend.h"

#include <string>
#include <vector>

namespace photobridge {
    class StorageNode {
    public:
        StorageNode(std::string node_id, StorageBackend& storage_backend);

        const std::string& id() const;

        bool writeObject(const std::string& key, const std::string& data);
        ReadObjectResult readObject(const std::string& key) const;
        bool deleteObject(const std::string& key);
        bool existsObject(const std::string& key) const;
        ObjectSizeResult objectSize(const std::string& key) const;
        ObjectStatResult statObject(const std::string& key) const;
        std::vector<std::string> listKeys(const std::string& prefix) const;
        bool deletePrefix(const std::string& prefix);

        bool isAvailable() const;
        void setAvailable(bool available);
    private:
        std::string node_id_;
        StorageBackend& storage_backend_;
        bool available_ = true;
    };
}