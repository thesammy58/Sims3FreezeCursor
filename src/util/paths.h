#pragma once
#include <string>

namespace S3FC {

// PATHS - Resolves Documents/Electronic Arts/<Localized Sims 3>/Sims3FreezeCursor/
// Deliberately a SEPARATE folder from S3MM's own Documents/.../S3MM/ - this
// mod keeps its own log file so the two can run side by side without
// stepping on each other's files.
namespace Paths {

// Base directory: Documents/Electronic Arts/<Localized Game>/Sims3FreezeCursor/
const std::wstring& GetDirectory();

// Sims3FreezeCursor.log
std::string GetLogPath();

// Ensure the directory exists, creating it if needed
bool EnsureDirectoryExists();

} // namespace Paths
} // namespace S3FC
