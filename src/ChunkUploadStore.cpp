#include "photobridge/ChunkUploadStore.h"

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
        if(chunk_count == 0){
            return SaveChunkResult::InvalidSession;
        }
        if(index >= chunk_count){
            return SaveChunkResult::InvalidIndex;
        }
        const auto chunk_file = chunks_dir / ("chunk_" + std::to_string(index)+".part");
        std::ofstream file(chunk_file,std::ios::binary);
        if(!file.is_open()){
            return SaveChunkResult::SaveFailed;
        }

        file.write(chunk.data(),static_cast<std::streamsize>(chunk.size()));
        if(!file.good()){
            return SaveChunkResult::SaveFailed;
        }
        file.close();
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
        if(filename.empty() || chunk_count == 0){
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
            if(std::filesystem::exists(chunk_file)){
                uploaded_indexes.push_back(i);
            } else {
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
        if(session_id.empty()){
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

}
