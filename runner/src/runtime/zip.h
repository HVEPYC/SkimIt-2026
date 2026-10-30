// Minimal zip reader (stored and deflated entries): the Android entry point extracts the guest
// libraries and Mono assemblies from the app's own APK.
#pragma once

#include <string>
#include <vector>

namespace skim::runtime {

// Extracts every entry whose name starts with one of `prefixes` to dest/<entry name>.
// Returns the number of files written, or -1 on error (logged).
int unzip(const std::string& zip, const std::vector<std::string>& prefixes, const std::string& dest);

}  // namespace skim::runtime
