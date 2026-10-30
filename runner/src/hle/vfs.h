// Guest file system view: guest path prefixes mapped to host directories, plus synthetic files.
// Anything not mounted does not exist for the guest.
#pragma once

#include <functional>
#include <string>

namespace skim::vfs {
void mount(const std::string& guest_prefix, const std::string& host_prefix);
void add_file(const std::string& guest_path, const std::string& content);
// A synthetic file whose content is produced at open time (e.g. /proc/self/maps).
void add_generated(const std::string& guest_path, std::function<std::string()> make);
// Host path for a guest path, or "" if not visible. For synthetic files, `content` (if given)
// receives the content and *is_synthetic is set.
std::string translate(const std::string& guest_path, std::string* content = nullptr, bool* is_synthetic = nullptr);
void set_cwd(const std::string& guest_dir);
std::string cwd();
}  // namespace skim::vfs
