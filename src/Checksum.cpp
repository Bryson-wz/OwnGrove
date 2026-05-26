#include "photobridge/Checksum.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <array>

namespace{
    constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78u;
    std::string toHex8(std::uint32_t value){
        std::stringstream out;
        out << std::hex <<std::nouppercase
            << std::setw(8)<<std::setfill('0')
            << value;
        return out.str();
    }
}
namespace photobridge {
    std::uint32_t crc32cStart(){
        return 0xFFFFFFFFu;
    }
    std::uint32_t crc32cUpdate(std::uint32_t crc, const char* data,std::size_t size){
        for(std::size_t i = 0; i < size; ++i){
            crc ^= static_cast<std::uint8_t>(data[i]);
            for(int bit = 0; bit < 8; ++bit){
                if((crc & 1u)!=0){
                    crc = (crc >> 1) ^ kCrc32cPolynomial;
                } else {
                    crc = crc >> 1;
                }
            }
        }
        return crc;
    }
    std::string crc32cFinished(std::uint32_t crc){
        return toHex8(crc ^ 0xFFFFFFFFu);
    }
    std::string crc32cHex(std::string_view data){
        std::uint32_t crc = 0;
        crc = crc32cUpdate(0xFFFFFFFFu, data.data(), data.size());
        return toHex8(crc ^ 0xFFFFFFFFu);
    }
    std::string crc32cFileHex(const std::filesystem::path& path){
        std::ifstream input(path, std::ios::binary);
        if(!input.is_open()){
            return "";
        }
        std::array<char,64*1024> buffer;
        auto crc = crc32cStart();
        while(input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto read_size = input.gcount();
            
            if(read_size > 0){
                crc = crc32cUpdate(crc, buffer.data(), static_cast<std::size_t>(read_size));
            }
        }
        return crc32cFinished(crc);
    }

}