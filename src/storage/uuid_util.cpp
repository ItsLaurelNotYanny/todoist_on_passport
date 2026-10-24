#include "todoist/uuid_util.hpp"
#include <sys/time.h>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <fstream>

namespace todoist {
namespace util {

std::string generateUuidV4() {
    uint8_t bytes[16];
    bool randomLoaded = false;

    // 1. Read from /dev/urandom (POSIX: standard on both Linux and QNX)
    std::ifstream urandom("/dev/urandom", std::ios::in | std::ios::binary);
    if (urandom) {
        urandom.read(reinterpret_cast<char*>(bytes), 16);
        if (urandom.gcount() == 16) {
            randomLoaded = true;
        }
        urandom.close();
    }

    // 2. Fallback to POSIX/C89 rand() if /dev/urandom is unavailable
    if (!randomLoaded) {
        static bool seeded = false;
        if (!seeded) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            std::srand(static_cast<unsigned int>(tv.tv_sec ^ tv.tv_usec));
            seeded = true;
        }
        for (int i = 0; i < 16; ++i) {
            bytes[i] = static_cast<uint8_t>(std::rand() & 0xFF);
        }
    }

    // Set UUID version 4 (bits 4-7 of byte 6)
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    // Set UUID variant (bits 6-7 of byte 8)
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            ss << '-';
        }
        ss << std::setw(2) << static_cast<int>(bytes[i]);
    }

    return ss.str();
}

std::string generateTempId() {
    return "temp_uuid_" + generateUuidV4();
}

int64_t currentEpochMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + static_cast<int64_t>(tv.tv_usec) / 1000;
}

} // namespace util
} // namespace todoist
