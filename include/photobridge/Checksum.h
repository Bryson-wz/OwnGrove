#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <filesystem>


namespace photobridge {
    std::string crc32cHex(std::string_view data);
    std::uint32_t crc32cStart();
    std::uint32_t crc32cUpdate(std::uint32_t crc, const char* data, std::size_t size);
    std::string crc32cFinished(std::uint32_t crc);
    std::string crc32cFileHex(const std::filesystem::path& path);
}