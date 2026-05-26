#include "photobridge/ChunkUploadStore.h"
#include "photobridge/Checksum.h"

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

    bool writeChunkMeta(
        const std::filesystem::path& chunk_meta_file,
        std::uintmax_t index,
        std::uintmax_t size,
        const std::string& checksum
    ){
        std::ofstream meta(chunk_meta_file, std::ios::binary);
        if(!meta.is_open()){
            return false;
        }
        meta << "{"
             << "\"index\":" << index << ","
             << "\"size\":" << size << ","
             << "\"checksum_algorithm\":\"crc32c\","
             << "\"checksum\":\"" << checksum << "\""
             << "}";
        return meta.good();
    }

    
}
namespace photobridge {
    ChunkUploadStore::ChunkUploadStore(std::filesystem::path temp_dir,std::filesystem::path upload_dir)
    : temp_dir_(std::move(temp_dir))
    , upload_dir_(std::move(upload_dir))
    {
    }
    InitSessionResponse ChunkUploadStore::initSession(const std::string& filename,std::uintmax_t total_size,std::uintmax_t chunk_size) const{
        if(!isSafeFilename(filename)){
            return InitSessionResponse{InitResult::InvalidFilename, ChunkUploadSession{}};
        }
        if(total_size == 0 || chunk_size == 0 || chunk_size > total_size){
            return InitSessionResponse{InitResult::InvalidSize, ChunkUploadSession{}};
        }
        const auto session_id = generateSessionId();
        const auto session_dir = temp_dir_ / ("upload_" + session_id);
        const auto chunks_dir = session_dir / "chunks";
        const auto session_file = session_dir / "session.json";
        const auto chunk_count = (total_size + chunk_size - 1) / chunk_size;
        if (std::filesystem::exists(session_dir)) {
            return InitSessionResponse{InitResult::FileExists, ChunkUploadSession{}};
        }
        
        std::error_code ec;
        std::filesystem::create_directories(chunks_dir, ec);
        if (ec) {
            return InitSessionResponse{InitResult::InitFailed, ChunkUploadSession{}};
        }
        std::ofstream file(session_file);
        if(!file.is_open()){
            return InitSessionResponse{InitResult::InitFailed,ChunkUploadSession{}};
        }
        file << "{"
        << "\"filename\":\"" << escapeJson(filename) << "\","
        << "\"session_id\":\"" << escapeJson(session_id) << "\","
        << "\"total_size\":" << total_size << ","
        << "\"chunk_size\":" << chunk_size << ","
        << "\"chunk_count\":" << chunk_count << ","
        << "\"status\":\"pending\""
        << "}";
        if(!file.good()){
            return InitSessionResponse{InitResult::InitFailed,ChunkUploadSession{}};
        }
        file.close();
        return InitSessionResponse{InitResult::Success, ChunkUploadSession{session_id, filename, total_size, chunk_size, chunk_count, "pending", chunks_dir}};
    }
    SaveChunkResult ChunkUploadStore::saveChunk(const std::string& session_id,std::uintmax_t index,const std::string& chunk) const{
        if(session_id.empty()){
            return SaveChunkResult::InvalidSession;
        }
        if(chunk.empty()){
            return SaveChunkResult::EmptyChunk;
        }

        const auto session_dir = temp_dir_ / ("upload_" + session_id);
        const auto chunks_dir = session_dir / "chunks";
        if(!std::filesystem::is_directory(session_dir)){
            return SaveChunkResult::InvalidSession;
        }
        if(!std::filesystem::is_directory(chunks_dir)){
            return SaveChunkResult::InvalidSession;
        }
        const auto session_file = session_dir / "session.json";
        std::ifstream session_stream(session_file);
        if(!session_stream.is_open()){
            return SaveChunkResult::InvalidSession;
        }
        std::stringstream buffer;
        buffer << session_stream.rdbuf();
        const auto session_text = buffer.str();
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
        const auto chunk_file = chunks_dir / ("chunk_" + std::to_string(index)+".part");
        const auto chunk_meta_file = chunks_dir / ("chunk_" + std::to_string(index)+".meta");
        const auto checksum = crc32cHex(chunk);
        if(std::filesystem::exists(chunk_file)){
            const auto existing_size = std::filesystem::file_size(chunk_file);
            if(existing_size != actual_size){
                return SaveChunkResult::Conflict;
            }
            const auto existing_checksum = crc32cFileHex(chunk_file);
            if(existing_checksum != checksum){
                return SaveChunkResult::Conflict;
            }
            if(std::filesystem::exists(chunk_meta_file)){
                std::ifstream meta_stream(chunk_meta_file);
                std::stringstream meta_buffer;
                meta_buffer << meta_stream.rdbuf();
                const auto meta_text = meta_buffer.str();
                const auto meta_size = extractUintField(meta_text, "size");
                const auto checksum_algorithm = extractStringField(meta_text, "checksum_algorithm");
                const auto checksum_expected = extractStringField(meta_text, "checksum");
                const auto checksum_actual = crc32cFileHex(chunk_file);
                if(meta_stream.is_open() &&
                meta_size == actual_size &&
                checksum_algorithm == "crc32c" &&
                checksum_expected == checksum_actual &&
                checksum_actual == checksum){
                    return SaveChunkResult::Success;
                }
            }

            if(writeChunkMeta(chunk_meta_file, index, actual_size, checksum)){
                return SaveChunkResult::Success;
            }
            return SaveChunkResult::SaveFailed;
        }
        std::ofstream file(chunk_file,std::ios::binary);
        if(!file.is_open()){
            return SaveChunkResult::SaveFailed;
        }

        file.write(chunk.data(),static_cast<std::streamsize>(chunk.size()));
        if(!file.good()){
            std::filesystem::remove(chunk_file);
            return SaveChunkResult::SaveFailed;
        }
        file.close();

        if(!writeChunkMeta(chunk_meta_file, index, actual_size, checksum)){
            std::filesystem::remove(chunk_file);
            std::filesystem::remove(chunk_meta_file);
            return SaveChunkResult::SaveFailed;
        }
        return SaveChunkResult::Success;
    }

    CompleteUploadResponse ChunkUploadStore::completeUpload(const std::string& session_id) const{
        const auto session_dir = temp_dir_ / ("upload_" + session_id);
        const auto chunks_dir = session_dir / "chunks";
        const auto session_file = session_dir / "session.json";

        if(!std::filesystem::is_directory(chunks_dir)||
           !std::filesystem::exists(session_file)){
            return {CompleteUploadResult::InvalidSession,"",0};
        }
        std::ifstream session_stream(session_file);
        std::stringstream buffer;
        buffer << session_stream.rdbuf();
        const auto session_text = buffer.str();
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
        std::filesystem::create_directories(upload_dir_);
        const auto output_path = upload_dir_ / filename;
        std::ofstream output_file(output_path,std::ios::binary);
        if(!output_file.is_open()){
            return {CompleteUploadResult::SaveFailed,"",0};
        }
        for(std::uintmax_t i = 0; i < chunk_count; ++i){
            const auto chunk_file = chunks_dir / ("chunk_" + std::to_string(i)+".part");
            if(!std::filesystem::exists(chunk_file)){
                output_file.close();
                std::filesystem::remove(output_path);
                return {CompleteUploadResult::MissingChunk,filename,0};
            }
            const auto chunk_meta_file = chunks_dir / ("chunk_" + std::to_string(i)+".meta");
            if (!std::filesystem::exists(chunk_meta_file)) {
                output_file.close();
                std::filesystem::remove(output_path);
                return {CompleteUploadResult::MergeFailed, filename, 0};
            }
            std::ifstream meta_stream(chunk_meta_file);
            if (!meta_stream.is_open()) {
                output_file.close();
                std::filesystem::remove(output_path);
                return {CompleteUploadResult::MergeFailed, filename, 0};
            }
            std::stringstream meta_buffer;
            meta_buffer << meta_stream.rdbuf();
            const auto meta_text = meta_buffer.str();
            const auto checksum_algorithm = extractStringField(meta_text, "checksum_algorithm");
            const auto checksum_expected = extractStringField(meta_text, "checksum");
            const auto checksum_actual = crc32cFileHex(chunk_file);
            const auto meta_size = extractUintField(meta_text, "size");
            const auto actual_size = std::filesystem::file_size(chunk_file);
            const auto expected_size = expectedChunkSize(i, chunk_count, chunk_size, total_size);
            if(meta_size != actual_size || actual_size != expected_size || checksum_algorithm != "crc32c" || checksum_actual != checksum_expected){
                output_file.close();
                std::filesystem::remove(output_path);
                return {CompleteUploadResult::MergeFailed,filename,0};
            }
            std::ifstream input_file(chunk_file,std::ios::binary);
            if(!input_file.is_open()){
                output_file.close();
                std::filesystem::remove(output_path);
                return {CompleteUploadResult::MergeFailed, filename, 0};
            }
            output_file<<input_file.rdbuf();    
            if(!output_file.good()){
                output_file.close();
                std::filesystem::remove(output_path);
                return {CompleteUploadResult::MergeFailed,"",0};
            }
            input_file.close();
        }
        output_file.close();
        return {CompleteUploadResult::Success,filename,std::filesystem::file_size(output_path)};
    }
    UploadStatusResponse ChunkUploadStore::getUploadStatus(const std::string& session_id) const{
        if(session_id.empty()){
            return UploadStatusResponse{UploadStatusResult::InvalidSession, ChunkUploadSession{}, {}, {}};
        }
        const auto session_dir = temp_dir_/("upload_" + session_id);
        const auto session_file = session_dir /"session.json";
        const auto chunks_dir = session_dir / "chunks";
        if(!std::filesystem::is_directory(session_dir) ||
           !std::filesystem::is_directory(chunks_dir) ||
           !std::filesystem::exists(session_file)){
            return UploadStatusResponse{UploadStatusResult::InvalidSession, ChunkUploadSession{}, {}, {}};
        }
        std::ifstream session_stream(session_file);
        if(!session_stream.is_open()){
            return UploadStatusResponse{UploadStatusResult::InvalidSession, ChunkUploadSession{}, {}, {}};
        }
        std::stringstream buffer;
        buffer<<session_stream.rdbuf();
        const auto session_text = buffer.str();

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
            const auto chunk_file = chunks_dir / ("chunk_" + std::to_string(i)+".part");
            const auto chunk_meta_file = chunks_dir / ("chunk_" + std::to_string(i)+".meta");
            const auto expected_size = expectedChunkSize(i, chunk_count, chunk_size, total_size);

            if(!std::filesystem::exists(chunk_file) || !std::filesystem::exists(chunk_meta_file)){
                missing_indexes.push_back(i);
                continue;
            }

            std::ifstream meta_stream(chunk_meta_file);
            if(!meta_stream.is_open()){
                missing_indexes.push_back(i);
                continue;
            }

            std::stringstream meta_buffer;
            meta_buffer << meta_stream.rdbuf();
            const auto meta_text = meta_buffer.str();
            const auto meta_size = extractUintField(meta_text, "size");
            const auto checksum_algorithm = extractStringField(meta_text, "checksum_algorithm");
            const auto checksum_expected = extractStringField(meta_text, "checksum");
            const auto checksum_actual = crc32cFileHex(chunk_file);
            const auto actual_size = std::filesystem::file_size(chunk_file);
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
            chunks_dir
        };
        return UploadStatusResponse{UploadStatusResult::Success, session, uploaded_indexes, missing_indexes};
    }
    bool ChunkUploadStore::cleanupSession(const std::string& session_id) const{
        if(!isSafeSessionId(session_id)){
            return false;
        }

        const auto session_dir = temp_dir_/("upload_" + session_id);
        std::error_code ec;
        std::filesystem::remove_all(session_dir, ec);
        return !ec;
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
        if (!isSafeSessionId(session_id)) {
            return AbortUploadResult::InvalidSession;
        }
    
        const auto session_dir = temp_dir_/("upload_" + session_id);
        if(!std::filesystem::exists(session_dir)){
            return AbortUploadResult::NotFound;
        }
        if(!std::filesystem::is_directory(session_dir)){
            return AbortUploadResult::InvalidSession;
        }

        std::error_code ec;
        std::filesystem::remove_all(session_dir, ec);

        return ec ? AbortUploadResult::AbortFailed : AbortUploadResult::Success;
    }

    CleanupExpiredUploadsResponse ChunkUploadStore::cleanupExpiredUploads(std::uintmax_t max_age_seconds) const{
        CleanupExpiredUploadsResponse response{};
        if(max_age_seconds == 0){
            return response;
        }
        if(!std::filesystem::is_directory(temp_dir_)){
            return response;
        }
        std::error_code ec;
        std::filesystem::directory_iterator it(temp_dir_, ec);
        const auto now = std::filesystem::file_time_type::clock::now();
        for(const auto &entry : std::filesystem::directory_iterator(temp_dir_)){
            if(!entry.is_directory()){
                continue;
            }

            const auto session_dir = entry.path();
            const auto dirname = session_dir.filename().string();
            if(dirname.find("upload_") != 0){
                continue;
            }

            std::error_code time_ec;
            const auto last_write = std::filesystem::last_write_time(session_dir, time_ec);
            if(time_ec){
                continue;
            }
            const auto age = now - last_write;
            const auto max_age = std::chrono::seconds(max_age_seconds);
            if(age <= max_age){
                continue;
            }
            std::error_code remove_ec;
            std::filesystem::remove_all(session_dir, remove_ec);
            if(remove_ec){
                response.failed_count++;
                continue;
            }
            response.removed_count++;
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
