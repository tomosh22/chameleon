#include "hook/user_hooks.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace cml::agent::user_hooks {

namespace {

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t read_u64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(read_u32(p)) | (static_cast<std::uint64_t>(read_u32(p + 4)) << 32);
}

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

bool inside(std::uint64_t address, std::uint64_t base, std::uint64_t size) {
    return address >= base && address - base < size;
}

bool known_kind(std::uint32_t kind) {
    return kind >= static_cast<std::uint32_t>(Kind::PrePatch) && kind <= static_cast<std::uint32_t>(Kind::PreRestart);
}

}  // namespace

const char* to_string(Kind kind) noexcept {
    switch (kind) {
        case Kind::PrePatch: return "pre-patch";
        case Kind::PostPatch: return "post-patch";
        case Kind::CompileStart: return "compile-start";
        case Kind::CompileSuccess: return "compile-success";
        case Kind::CompileError: return "compile-error";
        case Kind::PreRestart: return "pre-restart";
    }
    return "unknown";
}

void parse_section(const std::uint8_t* bytes, std::size_t size, std::uint64_t image_base, std::uint64_t image_size,
                   KeyReader read_key, void* read_context, std::vector<Record>* out, ParseStats* stats) {
    if (bytes == nullptr || out == nullptr) {
        return;
    }
    ParseStats local;
    ParseStats& s = stats != nullptr ? *stats : local;
    std::size_t offset = 0;
    while (offset + kRecordSize <= size) {
        if (read_u32(bytes + offset) != kMagic) {
            offset += 8;
            continue;
        }
        const std::uint32_t kind = read_u32(bytes + offset + 4);
        const std::uint64_t fn = read_u64(bytes + offset + 8);
        const std::uint64_t key_address = read_u64(bytes + offset + 16);
        if (out->size() >= kMaxRecordsPerImage) {
            s.too_many = true;
            return;
        }
        Record record;
        record.kind = static_cast<Kind>(kind);
        record.fn = fn;
        const bool ok = known_kind(kind) && inside(fn, image_base, image_size) &&
                        inside(key_address, image_base, image_size) && read_key != nullptr &&
                        read_key(read_context, key_address, &record.key);
        if (!ok) {
            ++s.rejected;
        } else {
            out->push_back(std::move(record));
        }
        offset += kRecordSize;
    }
}

bool find_hook_sections(const std::uint8_t* headers, std::size_t size, std::vector<SectionRange>* out) {
    if (headers == nullptr || out == nullptr || size < 0x40 || headers[0] != 'M' || headers[1] != 'Z') {
        return false;
    }
    const std::uint32_t pe = read_u32(headers + 0x3C);
    if (pe > size || size - pe < 24 || std::memcmp(headers + pe, "PE\0\0", 4) != 0) {
        return false;
    }
    const std::uint16_t sections = read_u16(headers + pe + 6);
    const std::uint16_t optional_size = read_u16(headers + pe + 20);
    const std::size_t optional = pe + 24;
    if (size - optional < optional_size || optional_size < 2 || read_u16(headers + optional) != 0x20B) {
        return false;  // PE32+ only
    }
    std::size_t table = optional + optional_size;
    for (std::uint16_t i = 0; i < sections; ++i, table += 40) {
        if (table > size || size - table < 40) {
            break;  // headers cut short: use what is there
        }
        const std::uint8_t* h = headers + table;
        static const char kName[8] = {'.', 'c', 'm', 'l', 'h', 'k', 0, 0};
        if (std::memcmp(h, kName, 8) == 0) {
            SectionRange range;
            range.size = read_u32(h + 8);
            range.rva = read_u32(h + 12);
            out->push_back(range);
        }
    }
    return true;
}

std::vector<Record> select_hooks(Kind kind, const std::vector<Source>& sources) {
    struct Chosen {
        const Record* record;
    };
    std::vector<const Record*> from_modules;
    std::vector<Chosen> from_patches;  // newest wins per key, first-seen position
    for (const Source& source : sources) {
        if (source.patch) {
            continue;
        }
        for (const Record& r : source.records) {
            if (r.kind == kind) {
                from_modules.push_back(&r);
            }
        }
    }
    for (const Source& source : sources) {
        if (!source.patch) {
            continue;
        }
        for (const Record& r : source.records) {
            if (r.kind != kind) {
                continue;
            }
            const auto in_module = std::find_if(from_modules.begin(), from_modules.end(),
                                                [&r](const Record* m) { return m->key == r.key; });
            if (in_module != from_modules.end()) {
                continue;
            }
            const auto earlier = std::find_if(from_patches.begin(), from_patches.end(),
                                              [&r](const Chosen& c) { return c.record->key == r.key; });
            if (earlier != from_patches.end()) {
                earlier->record = &r;  // a newer image: its copy replaces the older one
            } else {
                from_patches.push_back(Chosen{&r});
            }
        }
    }
    std::vector<Record> result;
    result.reserve(from_modules.size() + from_patches.size());
    for (const Record* r : from_modules) {
        result.push_back(*r);
    }
    for (const Chosen& c : from_patches) {
        result.push_back(*c.record);
    }
    return result;
}

}  // namespace cml::agent::user_hooks
