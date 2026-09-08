#include "owngrove/StorageNode.h"

#include <utility>

namespace owngrove {
    StorageNode::StorageNode(std::string node_id, StorageBackend& storage_backend)
    : node_id_(std::move(node_id))
    , storage_backend_(storage_backend)
    {
    }
    const std::string& StorageNode::id() const{
        return node_id_;
    }
    bool StorageNode::writeObject(const std::string& key, const std::string& data){
        if (!available_.load(std::memory_order_relaxed)) {
            return false;
        }
        return storage_backend_.writeObject(key, data);
    }
    ReadObjectResult StorageNode::readObject(const std::string& key) const{
        if (!available_.load(std::memory_order_relaxed)) {
            return ReadObjectResult{false, ""};
        }
        return storage_backend_.readObject(key);
    }
    bool StorageNode::deleteObject(const std::string& key){
        if (!available_.load(std::memory_order_relaxed)) {
            return false;
        }
        return storage_backend_.deleteObject(key);
    }
    bool StorageNode::existsObject(const std::string& key) const{
        if (!available_.load(std::memory_order_relaxed)) {
            return false;
        }
        return storage_backend_.existsObject(key);
    }
    ObjectSizeResult StorageNode::objectSize(const std::string& key) const{
        if (!available_.load(std::memory_order_relaxed)) {
            return ObjectSizeResult{false, 0};
        }
        return storage_backend_.objectSize(key);
    }
    ObjectStatResult StorageNode::statObject(const std::string& key) const{
        if (!available_.load(std::memory_order_relaxed)) {
            return ObjectStatResult{false, 0, std::filesystem::file_time_type::min()};
        }
        return storage_backend_.statObject(key);
    }
    std::vector<std::string> StorageNode::listKeys(const std::string& prefix) const{
        if (!available_.load(std::memory_order_relaxed)) {
            return {};
        }
        return storage_backend_.listKeys(prefix);
    }
    bool StorageNode::deletePrefix(const std::string& prefix){
        if (!available_.load(std::memory_order_relaxed)) {
            return false;
        }
        return storage_backend_.deletePrefix(prefix);
    }
    bool StorageNode::isAvailable() const{
        return available_.load(std::memory_order_relaxed);
    }
    void StorageNode::setAvailable(bool available){
        available_.store(available, std::memory_order_relaxed);
    }
}