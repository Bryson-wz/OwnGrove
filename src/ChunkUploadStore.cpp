#include "owngrove/ChunkUploadStore.h"
#include "owngrove/Checksum.h"
#include "owngrove/StorageBackend.h"
#include "owngrove/LocalStorageBackend.h"

#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <utility>
#include <system_error>
namespace{
    std::string extractStringField(const std::string& text, const std::string& key){
        const std::string pattern = "\""+key+"\":\"";
        const auto start = text.find(pattern);
        if(start == std::string::npos){
            return "";
        }
        const auto value_start = start + pattern.length();
        const auto value_end = text.find("\"", value_start);
        if(value_end == std::string::npos){
            return "";
        }
        return text.substr(value_start, value_end - value_start);
    }

    std::uintmax_t extractUintField(const std::string& text, const std::string& key){
        const std::string pattern = "\""+key+"\":";
        const auto start = text.find(pattern);
        if(start == std::string::npos){
            return 0;
        }
        const auto value_start = start + pattern.length();
        const auto value_end = text.find_first_of(",}", value_start);
        if (value_end == std::string::npos) {
            return 0;
        }        
        const auto value = text.substr(value_start, value_end - value_start);
        try {
            return static_cast<std::uintmax_t>(std::stoull(value));
        } catch (...) {
            return 0;
        }        
    }
    std::string escapeJson(const std::string& text){
        std::string escaped;
        for (char ch : text) {
            switch (ch) {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    escaped += ch;
                    break;
            }
        }
        return escaped;
    }

    std::uintmax_t expectedChunkSize(
        std::uintmax_t index,
        std::uintmax_t chunk_count,
        std::uintmax_t chunk_size,
        std::uintmax_t total_size
    ){
        if(index + 1 < chunk_count){
            return chunk_size;
        }
        return total_size - chunk_size * (chunk_count - 1);
    }
    std::string buildChunkMetaJson(std::uintmax_t index, std::uintmax_t size, const std::string& checksum){
        std::ostringstream meta;
        meta << "{"
             << "\"index\":" << index << ","
             << "\"size\":" << size << ","
             << "\"checksum_algorithm\":\"crc32c\","
             << "\"checksum\":\"" << checksum << "\""
             << "}";
        return meta.str();
    }
    std::string chunkPartKey(const std::string& session_id,std::uintmax_t index){
        return "uploads_tmp/upload_" + session_id + "/chunks/chunk_" + std::to_string(index) + ".part";
    }
    std::string chunkMetaKey(const std::string& session_id,std::uintmax_t index){
        return "uploads_tmp/upload_" + session_id + "/chunks/chunk_" + std::to_string(index) + ".meta";
    }
    std::string sessionKey(const std::string& session_id){
        return "uploads_tmp/upload_" + session_id + "/session.json";
    }
    std::string uploadPrefix(const std::string& session_id) {
        return "uploads_tmp/upload_" + session_id + "/";
    }
}
namespace owngrove {
    ChunkUploadStore::ChunkUploadStore(std::filesystem::path temp_dir,std::filesystem::path upload_dir,StorageBackend& storage_backend)
        : temp_dir_(std::move(temp_dir))
        , upload_dir_(std::move(upload_dir))
        , storage_backend_(storage_backend)
    {
    }
    InitSessionResponse ChunkUploadStore::initSession(const std::string& filename,std::uintmax_t total_size,std::uintmax_t chunk_size) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!isSafeFilename(filename)){
            return InitSessionResponse{InitResult::InvalidFilename, ChunkUploadSession{}};
        }
        if(total_size == 0 || chunk_size == 0){
            return InitSessionResponse{InitResult::InvalidSize, ChunkUploadSession{}};
        }
        const auto session_id = generateSessionId();
        const auto session_dir = temp_dir_ / ("upload_" + session_id);
        const auto chunks_dir = session_dir / "chunks";
        const auto chunk_count = (total_size + chunk_size - 1) / chunk_size;
        if (storage_backend_.existsObject(sessionKey(session_id))) {
            return InitSessionResponse{InitResult::FileExists, ChunkUploadSession{}};
        }
        
        std::ostringstream session_json;
        session_json << "{"
        << "\"filename\":\"" << escapeJson(filename) << "\","
        << "\"session_id\":\"" << escapeJson(session_id) << "\","
        << "\"total_size\":" << total_size << ","
        << "\"chunk_size\":" << chunk_size << ","
        << "\"chunk_count\":" << chunk_count << ","
        << "\"status\":\"pending\""
        << "}";
        if(!storage_backend_.writeObject(sessionKey(session_id), session_json.str())){
            return InitSessionResponse{InitResult::InitFailed,ChunkUploadSession{}};
        }
        return InitSessionResponse{InitResult::Success, ChunkUploadSession{session_id, filename, total_size, chunk_size, chunk_count, "pending", chunks_dir}};
    }
    SaveChunkResult ChunkUploadStore::saveChunk(const std::string& session_id,std::uintmax_t index,const std::string& chunk) const{
        std::lock_guard<std::mutex> lock(mutex_);
        if(session_id.empty()){
            return SaveChunkResult::InvalidSession;
        }
        if(chunk.empty()){
            return SaveChunkResult::EmptyChunk;
        }

        const auto session_result = storage_backend_.readObject(sessionKey(session_id));
        if(!session_result.ok){
            return SaveChunkResult::InvalidSession;
        }
        const auto& session_text = session_result.data;
        const auto chunk_count = extractUintField(session_text, "chunk_count");
        const auto chunk_size = extractUintField(session_text, "chunk_size");
        const auto total_size = extractUintField(session_text, "total_size");
        if(chunk_count == 0||chunk_size == 0||total_size == 0){
            return SaveChunkResult::InvalidSession;
        }
        if(index >= chunk_count){
            return SaveChunkResult::InvalidIndex;
        }
        const auto actual_size = chunk.size();
        const auto expected_size = expectedChunkSize(index, chunk_count, chunk_size, total_size);
        if(actual_size != expected_size){
            return SaveChunkResult::InvalidSize;
        }
        const auto chunk_key = chunkPartKey(session_id, index);
        const auto chunk_meta_key = chunkMetaKey(session_id, index);
        const auto checksum = crc32cHex(chunk);
        const auto meta_json = buildChunkMetaJson(index, actual_size, checksum);
        if(storage_backend_.existsObject(chunk_key)){
            const auto existing_size = storage_backend_.objectSize(chunk_key);
            if(!existing_size.ok){
                return SaveChunkResult::InvalidSession;
            }
            if(existing_size.size != actual_size){
                return SaveChunkResult::Conflict;
            }
            const auto existing_result = storage_backend_.readObject(chunk_key);
            if(!existing_result.ok){
                return SaveChunkResult::InvalidSession;
            }
            const auto existing_checksum = crc32cHex(existing_result.data);
            if(existing_checksum != checksum){
                return SaveChunkResult::Conflict;
            }
            if(storage_backend_.existsObject(chunk_meta_key)){
                const auto meta_result = storage_backend_.readObject(chunk_meta_key);
                if (!meta_result.ok) {
                    return SaveChunkResult::InvalidSession;
                }
                const auto& meta_text = meta_result.data;
                const auto meta_size = extractUintField(meta_text, "size");
                const auto checksum_algorithm = extractStringField(meta_text, "checksum_algorithm");
                const auto checksum_expected = extractStringField(meta_text, "checksum");
                const auto checksum_actual = crc32cHex(existing_result.data);
                if(meta_size == actual_size && checksum_algorithm == "crc32c" && checksum_expected == checksum_actual && checksum_actual == checksum){
                    return SaveChunkResult::Success;
                }
            }

            if(!storage_backend_.writeObject(chunk_meta_key, meta_json)){
                storage_backend_.deleteObject(chunk_key);
                storage_backend_.deleteObject(chunk_meta_key);
                return SaveChunkResult::SaveFailed;
            }
            return SaveChunkResult::Success;
        }

        if(!storage_backend_.writeObject(chunk_key, chunk)){
            return SaveChunkResult::SaveFailed;
        }


        if(!storage_backend_.writeObject(chunk_meta_key, meta_json)){
            storage_backend_.deleteObject(chunk_key);
            storage_backend_.deleteObject(chunk_meta_key);
            return SaveChunkResult::SaveFailed;
        }
        return SaveChunkResult::Success;
    }

    CompleteUploadResponse ChunkUploadStore::completeUpload(const std::string& session_id) const{
        std::lock_guard<std::mutex> lock(mutex_);
        const auto session_result = storage_backend_.readObject(sessionKey(session_id));
        if(!session_result.ok){
            return {CompleteUploadResult::InvalidSession,"",0};
        }
        const auto& session_text = session_result.data;
        const auto filename = extractStringField(session_text, "filename");
        if(!isSafeFilename(filename)){
            return {CompleteUploadResult::InvalidSession,"",0};
        }
        const auto chunk_count = extractUintField(session_text, "chunk_count");
        const auto chunk_size = extractUintField(session_text, "chunk_size");
        const auto total_size = extractUintField(session_text, "total_size");
        if(filename.empty() || chunk_count == 0 || chunk_size == 0 || total_size == 0){
            return {CompleteUploadResult::InvalidSession,"",0};
        }
        std::error_code ec;
        std::filesystem::create_directories(upload_dir_, ec);
        const auto output_path = upload_dir_ / filename;
        std::ofstream output_file(output_path,std::ios::binary);
        if(!output_file.is_open()){
            return {CompleteUploadResult::SaveFailed,"",0};
        }
        for(std::uintmax_t i = 0; i < chunk_count; ++i){
            const auto chunk_key = chunkPartKey(session_id, i);
            const auto chunk_meta_key = chunkMetaKey(session_id, i);
            if(!storage_backend_.existsObject(chunk_key)){
                output_file.close();
                std::filesystem::remove(output_path, ec);
                return {CompleteUploadResult::MissingChunk,filename,0};
            }
            if (!storage_backend_.existsObject(chunk_meta_key)) {
                output_file.close();
                std::filesystem::remove(output_path, ec);
                return {CompleteUploadResult::MergeFailed, filename, 0};
            }
            const auto meta_result = storage_backend_.readObject(chunk_meta_key);
            if (!meta_result.ok) {
                output_file.close();
                std::filesystem::remove(output_path, ec);
                return {CompleteUploadResult::MergeFailed, filename, 0};
            }
            const auto chunk_result = storage_backend_.readObject(chunk_key);
            if(!chunk_result.ok){
                output_file.close();
                std::filesystem::remove(output_path, ec);
                return {CompleteUploadResult::MergeFailed, filename, 0};
            }
            const auto& meta_text = meta_result.data;
            const auto& chunk_data = chunk_result.data;
            const auto checksum_algorithm = extractStringField(meta_text, "checksum_algorithm");
            const auto checksum_expected = extractStringField(meta_text, "checksum");
            const auto checksum_actual = crc32cHex(chunk_data);
            const auto meta_size = extractUintField(meta_text, "size");
            const auto actual_size = chunk_data.size();
            const auto expected_size = expectedChunkSize(i, chunk_count, chunk_size, total_size);
            if(meta_size != actual_size || actual_size != expected_size || checksum_algorithm != "crc32c" || checksum_actual != checksum_expected){
                output_file.close();
                std::filesystem::remove(output_path, ec);
                return {CompleteUploadResult::MergeFailed,filename,0};
            }
            output_file.write(chunk_data.data(), static_cast<std::streamsize>(chunk_data.size()));
            if(!output_file.good()){
                output_file.close();
                std::filesystem::remove(output_path, ec);
                return {CompleteUploadResult::MergeFailed,"",0};
            }
        }
        output_file.close();
        const auto final_size = std::filesystem::file_size(output_path, ec);
        return {CompleteUploadResult::Success, filename, ec ? static_cast<std::uintmax_t>(0) : final_size};
    }
    UploadStatusResponse ChunkUploadStore::getUploadStatus(const std::string& session_id) const{
        std::lock_guard<std::mutex> lock(mutex_);
        if(session_id.empty()){
            return UploadStatusResponse{UploadStatusResult::InvalidSession, ChunkUploadSession{}, {}, {}};
        }
        if(!storage_backend_.existsObject(sessionKey(session_id))){
            return UploadStatusResponse{UploadStatusResult::InvalidSession, ChunkUploadSession{}, {}, {}};
        }
        const auto session_result = storage_backend_.readObject(sessionKey(session_id));
        if(!session_result.ok){
            return UploadStatusResponse{UploadStatusResult::InvalidSession, ChunkUploadSession{}, {}, {}};
        }
        const auto& session_text = session_result.data;

        const auto filename = extractStringField(session_text, "filename");
        const auto parsed_session_id = extractStringField(session_text, "session_id");
        const auto total_size = extractUintField(session_text, "total_size");
        const auto chunk_size = extractUintField(session_text, "chunk_size");
        const auto chunk_count = extractUintField(session_text, "chunk_count");
        const auto status = extractStringField(session_text, "status");
        if(filename.empty() || parsed_session_id.empty() || total_size == 0 || chunk_size == 0 || chunk_count == 0 || status.empty()){
            return UploadStatusResponse{UploadStatusResult::InvalidSession, ChunkUploadSession{}, {}, {}};
        }
        std::vector<std::uintmax_t> uploaded_indexes;
        std::vector<std::uintmax_t> missing_indexes;
        for(std::uintmax_t i = 0; i < chunk_count; ++i){
            const auto chunk_key = chunkPartKey(session_id, i);
            const auto chunk_meta_key = chunkMetaKey(session_id, i);
            const auto expected_size = expectedChunkSize(i, chunk_count, chunk_size, total_size);

            if(!storage_backend_.existsObject(chunk_key) || !storage_backend_.existsObject(chunk_meta_key)){
                missing_indexes.push_back(i);
                continue;
            }

            const auto meta_result = storage_backend_.readObject(chunk_meta_key);
            if (!meta_result.ok) {
                missing_indexes.push_back(i);
                continue;
            }

            const auto chunk_result = storage_backend_.readObject(chunk_key);
            if (!chunk_result.ok) {
                missing_indexes.push_back(i);
                continue;
            }
            const auto& chunk_data = chunk_result.data;
            const auto& meta_text = meta_result.data;
            const auto meta_size = extractUintField(meta_text, "size");
            const auto checksum_algorithm = extractStringField(meta_text, "checksum_algorithm");
            const auto checksum_expected = extractStringField(meta_text, "checksum");
            const auto checksum_actual = crc32cHex(chunk_data);
            const auto actual_size = chunk_data.size();
            if(meta_size == expected_size && actual_size == expected_size && checksum_algorithm == "crc32c" && checksum_actual == checksum_expected){
                uploaded_indexes.push_back(i);
            }
            else{
                missing_indexes.push_back(i);
            }
        }
        ChunkUploadSession session{
            parsed_session_id,
            filename,
            total_size,
            chunk_size,
            chunk_count,
            status,
            std::filesystem::path{}
        };
        return UploadStatusResponse{UploadStatusResult::Success, session, uploaded_indexes, missing_indexes};
    }
    bool ChunkUploadStore::cleanupSession(const std::string& session_id) const{
        std::lock_guard<std::mutex> lock(mutex_);
        if(!isSafeSessionId(session_id)){
            return false;
        }
        if(!storage_backend_.existsObject(sessionKey(session_id))){
            return false;
        }
        return storage_backend_.deletePrefix(uploadPrefix(session_id));
    }

    bool ChunkUploadStore::isSafeFilename(const std::string& filename) const{
        return !filename.empty()
            && filename.find("..") == std::string::npos
            && filename.find_first_of("/\\") == std::string::npos;
    }
    std::string ChunkUploadStore::generateSessionId() const{
        return std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    }
    AbortUploadResult ChunkUploadStore::abortUpload(const std::string& session_id) const{
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isSafeSessionId(session_id)) {
            return AbortUploadResult::InvalidSession;
        }
    
        if(!storage_backend_.existsObject(sessionKey(session_id))){
            return AbortUploadResult::NotFound;
        }
        return storage_backend_.deletePrefix(uploadPrefix(session_id)) ? AbortUploadResult::Success : AbortUploadResult::AbortFailed;
    }

    CleanupExpiredUploadsResponse ChunkUploadStore::cleanupExpiredUploads(std::uintmax_t max_age_seconds) const {
        std::lock_guard<std::mutex> lock(mutex_);
        CleanupExpiredUploadsResponse response{};
        if (max_age_seconds == 0) {
            return response;
        }

        const auto keys = storage_backend_.listKeys("uploads_tmp/");
        const auto now = std::filesystem::file_time_type::clock::now();
        const auto max_age = std::chrono::seconds(max_age_seconds);

        for (const auto& key : keys) {
            if (!key.ends_with("/session.json")) {
                continue;
            }

            const auto stat = storage_backend_.statObject(key);
            if (!stat.ok) {
                response.failed_count++;
                continue;
            }

            if (now - stat.last_modified <= max_age) {
                continue;
            }

            const auto prefix = key.substr(0, key.size() - std::string("session.json").size());
            if (storage_backend_.deletePrefix(prefix)) {
                response.removed_count++;
            } else {
                response.failed_count++;
            }
        }
    
        return response;
    }

    bool ChunkUploadStore::isSafeSessionId(const std::string& session_id) const {
        if (session_id.empty()) {
            return false;
        }

        for (unsigned char ch : session_id) {
            if (!std::isalnum(ch) && ch != '-' && ch != '_') {
                return false;
            }
        }

        return true;
    }
}
