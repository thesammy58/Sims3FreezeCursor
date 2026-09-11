#pragma once

namespace S3FC {

// Logged on the first line of every session. With public downloads there will
// be people running several versions and reporting at once, and a log that
// does not say which build produced it cannot be read.
inline constexpr const char* kVersion = "1.0.0";

} // namespace S3FC
