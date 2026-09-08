#include "owngrove/ReplicaStorageBackend.h"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <set>
#include <string_view>

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
namespace owngrove {
    ReplicaStorageBackend::ReplicaStorageBackend(std::vector<StorageNode*> nodes, std::size_t replica_count, std::size_t write_quorum)
    {
        if(nodes.empty()){
            throw std::invalid_argument("ReplicaStorageBackend requires at least one node");
        }
        for(const auto* node :nodes){
            if(node == nullptr){
                throw std::invalid_argument("ReplicaStorageBackend node must not be null");
            }
        }
        if(replica_count <= 0 || replica_count > nodes.size()){
            throw std::invalid_argument("ReplicaStorageBackend replica count must be greater than 0 and less than or equal to the number of nodes");
        }
        if(write_quorum <= 0 || write_quorum > replica_count){
            throw std::invalid_argument("ReplicaStorageBackend write quorum must be greater than 0 and less than or equal to the replica count");
        }
        nodes_ = std::move(nodes);
        replica_count_ = replica_count;
        write_quorum_ = write_quorum;
    }
    std::size_t ReplicaStorageBackend::primaryIndexForKey(const std::string& key) const{
        return static_cast<std::size_t>(fnv1a64(key) % nodes_.size());
    }
    std::vector<StorageNode*> ReplicaStorageBackend::replicaSetForKey(const std::string& key){
        std::vector<StorageNode*> replicas;
        const auto primary = primaryIndexForKey(key);
        for(std::size_t i = 0; i < replica_count_; ++i){
            const auto index = (primary + i) % nodes_.size();
            replicas.push_back(nodes_[index]);
        }
        return replicas;
    }
    std::vector<const StorageNode*> ReplicaStorageBackend::replicaSetForKey(const std::string& key) const{
        std::vector<const StorageNode*> replicas;
        const auto primary = primaryIndexForKey(key);
        for(std::size_t i = 0; i < replica_count_; ++i){
            const auto index = (primary + i) % nodes_.size();
            replicas.push_back(nodes_[index]);
        }
        return replicas;
    }
    bool ReplicaStorageBackend::writeObject(const std::string& key, const std::string& data){ 
        std::vector<StorageNode*> written;
        std::size_t success_count = 0;
        for(auto* node : replicaSetForKey(key)){
            if(node->writeObject(key, data)){
                success_count++;
                written.push_back(node);
            }
        }
        if(success_count >= write_quorum_){
            return true;
        }
        for(auto* node :written){
            node->deleteObject(key);
        }
        return false;
    }
    bool ReplicaStorageBackend::existsObject(const std::string& key) const{
        for(const auto* node :replicaSetForKey(key)){
            if(node->existsObject(key)){
                return true;
            }
        }
        return false;
    }
    ReadObjectResult ReplicaStorageBackend::readObject(const std::string& key) const{
        for(const auto* node :replicaSetForKey(key)){
            const auto result = node->readObject(key);
            if(result.ok){
                return result;
            }
        }
        return ReadObjectResult{false, ""};
    }
    ObjectStatResult ReplicaStorageBackend::statObject(const std::string& key) const{
        for(const auto* node :replicaSetForKey(key)){
            const auto result = node->statObject(key);
            if(result.ok){
                return result;
            }
        }
        return ObjectStatResult{false, 0, std::filesystem::file_time_type::min()};
    }
    ObjectSizeResult ReplicaStorageBackend::objectSize(const std::string& key) const{
        for(const auto* node :replicaSetForKey(key)){
            const auto size = node->objectSize(key);
            if(size.ok){
                return size;
            }
        }
        return {};
    }
    bool ReplicaStorageBackend::deleteObject(const std::string& key){
        bool ok = true;
        for(auto* node : replicaSetForKey(key)){
            if(node->existsObject(key) && !node->deleteObject(key)){
                ok = false;
            }
        }
        return ok;
    }
    std::vector<std::string> ReplicaStorageBackend::listKeys(const std::string& prefix) const{
        std::set<std::string> unique;
        for(const auto* node : nodes_){
            const auto node_keys = node->listKeys(prefix);
            for(const auto& key : node_keys){
                unique.insert(key);
            }
        }
        return std::vector<std::string>(unique.begin(), unique.end());
    }
    bool ReplicaStorageBackend::deletePrefix(const std::string& prefix){
        bool ok = true;
        for(auto* node : nodes_){
            ok = node->deletePrefix(prefix) && ok;
        }
        return ok;
    }
    std::vector<const StorageNode*> ReplicaStorageBackend::nodes() const{
        return std::vector<const StorageNode*>(nodes_.begin(), nodes_.end());
    }
    bool ReplicaStorageBackend::setNodeAvailability(const std::string& node_id, bool available){
        for(auto* node : nodes_){
            if(node->id() == node_id){
                node->setAvailable(available);
                return true;
            }
        }
        return false;
    }
    std::vector<std::string> ReplicaStorageBackend::replicaNodeIdsForKey(const std::string& key) const{
        std::vector<std::string> ids;
        for(const auto* node : replicaSetForKey(key)){
            ids.push_back(node->id());
        }
        return ids;
    }
    std::vector<ReplicaAuditItem> ReplicaStorageBackend::auditReplicasForKey(const std::string& key) const{
        std::vector<ReplicaAuditItem> items;
        for(const auto* node : replicaSetForKey(key)){
            ReplicaAuditItem item;
            item.node_id = node->id();
            item.available = node->isAvailable();
            item.exists = node->existsObject(key);
            const auto size = node->objectSize(key);    
            if(size.ok){
                item.size = size.size;
            }
            items.push_back(item);
        }
        return items;
    }
    ReplicaRepairResult ReplicaStorageBackend::repairReplicasForKey(const std::string& key){
        const StorageNode* source_node = nullptr;
        std::string source_data;
    
        for(const auto* node : replicaSetForKey(key)){
            if(!node->isAvailable()){
                continue;
            }
    
            const auto read = node->readObject(key);
            if(read.ok){
                source_node = node;
                source_data = read.data;
                break;
            }
        }
    
        if(source_node == nullptr){
            return ReplicaRepairResult{false, 0, ""};
        }
    
        std::size_t repaired_count = 0;
        for(auto* node : replicaSetForKey(key)){
            if(!node->isAvailable() || node->existsObject(key)){
                continue;
            }
    
            if(node->writeObject(key, source_data)){
                repaired_count++;
            }
        }
    
        return ReplicaRepairResult{repaired_count > 0, repaired_count, source_node->id()};
    }
}
