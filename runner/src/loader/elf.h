// ELF32 (ARM, EABI) shared-object loader for the guest. Maps PT_LOAD segments into the guest
// arena, applies REL relocations and binds imports, the way Android 4.x's linker did:
// the library itself first, then the libraries loaded before it, then HLE.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest/memory.h"

namespace skim {

class GuestThread;

struct Library {
    std::string name;
    gaddr base = 0, end = 0;
    std::unordered_map<std::string, gaddr> exports;  // defined dynamic symbols (Thumb bit kept)
    gaddr init = 0;
    std::vector<gaddr> init_array;
    gaddr exidx = 0;
    std::uint32_t exidx_count = 0;
    std::vector<std::string> unresolved;  // imports bound to lazy stubs
    struct Segment { gaddr start, end; bool write, exec; };
    std::vector<Segment> segments;        // PT_LOAD ranges (page-rounded), for /proc/self/maps
};

namespace loader {
// Loads and relocates `host_path` as `name`. Does not run constructors.
Library* load(const std::string& host_path, const std::string& name);
void run_constructors(Library& lib, GuestThread& t);

Library* find(const std::string& name);
Library* containing(gaddr a);
gaddr lookup(const std::string& symbol);  // across all loaded libraries, in load order
const std::vector<std::unique_ptr<Library>>& all();
}  // namespace loader

}  // namespace skim
