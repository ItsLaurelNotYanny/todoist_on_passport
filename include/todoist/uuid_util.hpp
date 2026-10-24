#ifndef TODOIST_UUID_UTIL_HPP
#define TODOIST_UUID_UTIL_HPP

#include <string>
#include <stdint.h>

namespace todoist {
namespace util {

// Generate RFC 4122 version 4 UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000")
std::string generateUuidV4();

// Generate temporary ID for offline task creation (e.g., "temp_uuid_550e8400-...")
std::string generateTempId();

// Get current Unix epoch time in milliseconds using POSIX gettimeofday
int64_t currentEpochMs();

} // namespace util
} // namespace todoist

#endif // TODOIST_UUID_UTIL_HPP
