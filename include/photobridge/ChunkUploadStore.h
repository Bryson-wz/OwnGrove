#pragma once

#include <filesystem>
#include <string>
#include <cstdint>
#include <vector>

namespace photobridge {
    struct ChunkUploadSession{
        std::string session_id;
        std::string filename;
        std::uintmax_t total_size = 0;
        std::uintmax_t chunk_size = 0;
        std::uintmax_t chunk_count = 0;
        std::string status;
        std::filesystem::path chunks_dir;
    };
    enum class InitResult{
        Success,
        InvalidFilename,
        InvalidSize,
        FileExists,
        InitFailed,
    };
    struct InitSessionResponse{
        InitResult result;
        ChunkUploadSession session;
    };
    enum class SaveChunkResult{
        Success,
        InvalidSession,
        InvalidIndex,
        EmptyChunk,
        SaveFailed,
    };
    enum class CompleteUploadResult {
        Success,
        InvalidSession,
        MissingChunk,
        MergeFailed,
        SaveFailed,
    };
    
    struct CompleteUploadResponse{
        CompleteUploadResult result;
        std::string filename;
        std::uintmax_t size = 0;
    };
    enum class UploadStatusResult{
        Success,
        InvalidSession
    };
    struct UploadStatusResponse{
        UploadStatusResult result;
        ChunkUploadSession session;
        std::vector<std::uintmax_t> uploaded_indexes;
        std::vector<std::uintmax_t> missing_indexes;
    };
    class ChunkUploadStore{
        public:
        explicit ChunkUploadStore(
            std::filesystem::path temp_dir,
            std::filesystem::path upload_dir
        );
        InitSessionResponse initSession(
            const std::string& filename,
            std::uintmax_t total_size,
            std::uintmax_t chunk_size
        ) const;
        SaveChunkResult saveChunk(
            const std::string& session_id,
            std::uintmax_t index,
            const std::string& chunk
        ) const;
        CompleteUploadResponse completeUpload(
            const std::string& session_id
        ) const;
        UploadStatusResponse getUploadStatus(
            const std::string& session_id
        ) const;
        bool cleanupSession(const std::string& session_id) const;
        private:
        std::filesystem::path temp_dir_;
        std::filesystem::path upload_dir_;
        bool isSafeFilename(const std::string& filename) const;
        std::string generateSessionId() const;
        
    };
}
