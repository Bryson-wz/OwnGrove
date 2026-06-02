#pragma once
#include "photobridge/StorageBackend.h"
#include "photobridge/StorageNode.h"

#include <vector>
#include <string>
#include <cstdint>
#include <utility>

namespace photobridge {
    struct ReplicaAuditItem {
        std::string node_id;
        bool available = false;
        bool exists = false;
        std::uintmax_t size = 0;
    };
    struct ReplicaRepairResult {
        bool repaired = false;
        std::size_t repaired_count = 0;
        std::string source_node_id;
    };
    class ReplicaStorageBackend : public StorageBackend {
    public:
        explicit ReplicaStorageBackend(std::vector<StorageNode*> nodes, std::size_t replica_count, std::size_t write_quorum);
        bool writeObject(const std::string& key, const std::string& data) override;
        ReadObjectResult readObject(const std::string& key) const override;
        bool deleteObject(const std::string& key) override;
        bool existsObject(const std::string& key) const override;
        ObjectSizeResult objectSize(const std::string& key) const override;
        ObjectStatResult statObject(const std::string& key) const override;
        std::vector<std::string> listKeys(const std::string& prefix) const override;
        bool deletePrefix(const std::string& prefix) override;
        std::vector<const StorageNode*> nodes() const;
        bool setNodeAvailability(const std::string& node_id, bool available);
        std::vector<std::string> replicaNodeIdsForKey(const std::string& key) const;
        std::vector<ReplicaAuditItem> auditReplicasForKey(const std::string& key) const;
        ReplicaRepairResult repairReplicasForKey(const std::string& key);
    private:
        std::size_t primaryIndexForKey(const std::string& key) const;
        std::vector<StorageNode*> replicaSetForKey(const std::string& key);
        std::vector<const StorageNode*> replicaSetForKey(const std::string& key) const;

        std::vector<StorageNode*> nodes_;
        std::size_t replica_count_ = 0;
        std::size_t write_quorum_ = 0;
    };
}
