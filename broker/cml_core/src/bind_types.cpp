#include "cml/core/bind_types.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <unordered_map>

#include "cml/pdb/diag_ids.h"
#include "cml/util/log.h"

namespace cml::core::bind {

namespace {

std::span<const std::uint8_t> bytes_of(coff::ByteSpan s) noexcept {
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

std::uint32_t rd32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

std::string hex32(std::uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%X", v);
    return text;
}

}  // namespace

const char* to_string(ObjectTypeOrigin origin) noexcept {
    switch (origin) {
        case ObjectTypeOrigin::NotRead: return "not-read";
        case ObjectTypeOrigin::DebugT: return "debugT";
        case ObjectTypeOrigin::TypeServer: return "typeserver";
        case ObjectTypeOrigin::Unreadable: return "unreadable";
    }
    return "?";
}

ObjectSignatures::ObjectSignatures(const coff::Object& object, std::string object_name) : object_(object), object_name_(std::move(object_name)) {}
ObjectSignatures::~ObjectSignatures() = default;

void ObjectSignatures::unreadable(std::uint32_t diag_id, std::string why) {
    origin_ = ObjectTypeOrigin::Unreadable;
    detail_ = std::move(why);
    types_ = nullptr;
    unknown_.known = false;
    unknown_.diag_id = diag_id;
    unknown_.why = "the type records of " + object_name_ + " cannot be read: " + detail_;
}

void ObjectSignatures::load() {
    loaded_ = true;
    no_record_.diag_id = pdb::diag::kBadObjectSymbols;
    no_record_.why = "the function has no procedure record in the .debug$S sections of " + object_name_;

    // ---- the type records ----
    const coff::Section* debug_t = nullptr;
    std::size_t debug_t_sections = 0;
    for (const coff::Section& sec : object_.sections()) {
        if (sec.name == ".debug$T" && !sec.data.empty()) {
            ++debug_t_sections;
            if (debug_t == nullptr) {
                debug_t = &sec;
            }
        }
    }
    if (debug_t == nullptr) {
        unreadable(pdb::diag::kNoObjectTypes, "the object has no .debug$T section (it was compiled without CodeView debug information)");
    } else if (debug_t_sections > 1) {
        unreadable(pdb::diag::kNoObjectTypes, "the object has " + std::to_string(debug_t_sections) + " .debug$T sections");
    } else {
        const std::span<const std::uint8_t> bytes = bytes_of(debug_t->data);
        Result<pdb::DebugTInfo> info = pdb::inspect_debug_t(bytes);
        if (!info) {
            unreadable(info.error().code, info.error().message);
        } else if (info->kind == pdb::DebugTInfo::Kind::Empty) {
            unreadable(pdb::diag::kNoObjectTypes, "its .debug$T section holds no records");
        } else if (info->kind == pdb::DebugTInfo::Kind::TypeServer) {
            // /Zi: the types are in the compiler PDB. It must be the very PDB the object was compiled against.
            const std::string& path = info->server.pdb_path;
            Result<std::unique_ptr<pdb::TypeServerPdb>> server = pdb::TypeServerPdb::open(path);
            if (!server) {
                unreadable(server.error().code, "its types are in the compiler PDB '" + path + "' (the object was compiled with /Zi), which cannot be read: " + server.error().message);
            } else if ((*server)->id().guid != info->server.id.guid) {
                unreadable(pdb::diag::kTypeServerMismatch, "its types are in the compiler PDB '" + path + "', but the file there is " + (*server)->id().to_string() +
                                                               " and the object was compiled against " + info->server.id.to_string());
            } else {
                if ((*server)->id().age != info->server.id.age) {
                    CML_LOG(Debug, "bind: types of %s: compiler PDB '%s' has age %u, the object recorded age %u (same GUID: accepted)", object_name_.c_str(), path.c_str(),
                            (*server)->id().age, info->server.id.age);
                }
                type_server_ = std::move(server).value();
                types_ = type_server_.get();
                origin_ = ObjectTypeOrigin::TypeServer;
                detail_ = "compiler PDB '" + path + "'";
            }
        } else {
            Result<pdb::ObjectTypes> parsed = pdb::ObjectTypes::parse(bytes);
            if (!parsed) {
                unreadable(parsed.error().code, parsed.error().message);
            } else {
                object_types_ = std::make_unique<pdb::ObjectTypes>(std::move(parsed).value());
                types_ = object_types_.get();
                origin_ = ObjectTypeOrigin::DebugT;
                detail_ = std::to_string(object_types_->record_count()) + " records";
                if (info->kind == pdb::DebugTInfo::Kind::Precompiled) {
                    detail_ += "; types below " + hex32(info->first_own_index) + " are in the precompiled header object '" + info->precompiled_object +
                               "' and are not resolved (signatures that need them are unknown)";
                }
            }
        }
    }

    // ---- the procedure records: which type each function has ----
    if (types_ != nullptr) {
        std::size_t bad_sections = 0;
        for (const coff::Section& sec : object_.sections()) {
            if (sec.name != ".debug$S" || sec.data.empty()) {
                continue;
            }
            const std::span<const std::uint8_t> bytes = bytes_of(sec.data);
            Result<std::vector<pdb::ObjectProc>> procs = pdb::read_object_procs(bytes);
            if (!procs) {
                ++bad_sections;
                CML_LOG(Debug, "bind: types of %s: .debug$S section %u skipped: %s", object_name_.c_str(), sec.index, procs.error().message.c_str());
                continue;
            }
            if (procs->empty()) {
                continue;
            }
            // SECREL relocations of this section by site: the one on a record's offset field names the function.
            std::unordered_map<std::uint32_t, std::uint32_t> secrel;  // site -> symbol index
            for (const coff::Relocation& r : object_.relocations(sec)) {
                if (r.type == coff::RelocType::SecRel) {
                    secrel.emplace(r.virtual_address, r.symbol_index);
                }
            }
            for (const pdb::ObjectProc& p : *procs) {
                const auto site = secrel.find(p.offset_field);
                if (site == secrel.end() || static_cast<std::uint64_t>(p.offset_field) + 4 > bytes.size()) {
                    continue;
                }
                const coff::Symbol* target = object_.symbol_at(site->second);
                if (target == nullptr || target->section_number <= 0) {
                    continue;
                }
                const std::uint64_t offset = static_cast<std::uint64_t>(target->value) + rd32(bytes.data() + p.offset_field);
                if (offset > 0xFFFFFFFFull) {
                    continue;
                }
                const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(target->section_number)) << 32) | offset;
                const auto [it, inserted] = procs_.try_emplace(key);
                if (inserted) {
                    it->second.type_index = p.type_index;
                    it->second.type_is_id = p.type_is_id;
                } else if (it->second.type_index != p.type_index || it->second.type_is_id != p.type_is_id) {
                    it->second.conflict = true;
                }
                // The function-local statics of the procedure: each record names its variable through the SECREL
                // relocation on its offset field, like the procedure record names its function.
                ProcStatics& statics = it->second.statics;
                if (!inserted) {
                    if (!statics.statics.empty() || !p.statics.empty()) {
                        statics.usable = false;
                        statics.why = "two procedure records of " + object_name_ + " name this function";
                    }
                    continue;
                }
                if (!p.statics_complete) {
                    statics.usable = false;
                    statics.why = "the procedure has more than " + std::to_string(pdb::kMaxProcStatics) + " function-local statics";
                }
                for (const pdb::ProcStatic& s : p.statics) {
                    LocalStatic local;
                    local.record = s;
                    const auto data_site = secrel.find(s.offset_field);
                    const coff::Symbol* variable = data_site == secrel.end() ? nullptr : object_.symbol_at(data_site->second);
                    if (variable != nullptr && variable->section_number > 0 && static_cast<std::uint64_t>(s.offset_field) + 4 <= bytes.size()) {
                        const std::uint64_t at = static_cast<std::uint64_t>(variable->value) + rd32(bytes.data() + s.offset_field);
                        if (at <= 0xFFFFFFFFull) {
                            local.section = static_cast<std::uint32_t>(variable->section_number);
                            local.value = static_cast<std::uint32_t>(at);
                            const std::uint64_t data_key = (static_cast<std::uint64_t>(local.section) << 32) | at;
                            const auto [owner, first] = static_owner_.try_emplace(data_key, key);
                            if (!first && owner->second != key) {
                                owner->second = kSeveralFunctions;
                            }
                        }
                    }
                    statics.statics.push_back(std::move(local));
                }
            }
        }
        if (bad_sections != 0) {
            detail_ += "; " + std::to_string(bad_sections) + " malformed .debug$S section(s) skipped";
        }
    }
    // The evidence line of ADR-0042 (WP-37): which path supplied the new side's types.
    CML_LOG(Info, "WP37-TYPES source=%s object=%s procs=%zu detail=%s", to_string(origin_), object_name_.c_str(), procs_.size(), detail_.c_str());
}

const ObjectSignatures::ProcStatics* ObjectSignatures::statics_of(std::uint32_t section, std::uint32_t value) {
    if (!loaded_) {
        load();
    }
    const auto it = procs_.find((static_cast<std::uint64_t>(section) << 32) | value);
    return it == procs_.end() ? nullptr : &it->second.statics;
}

std::optional<std::uint64_t> ObjectSignatures::function_of_static(std::uint32_t section, std::uint32_t value) {
    if (!loaded_) {
        load();
    }
    const auto it = static_owner_.find((static_cast<std::uint64_t>(section) << 32) | value);
    if (it == static_owner_.end() || it->second == kSeveralFunctions) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> ObjectSignatures::type_text(std::uint32_t type_index, std::string* why) {
    if (!loaded_) {
        load();
    }
    if (types_ == nullptr) {
        if (why != nullptr) {
            *why = unknown_.why;
        }
        return std::nullopt;
    }
    return pdb::canonical_type(*types_, type_index, why);
}

const pdb::FunctionSignature& ObjectSignatures::at(std::uint32_t section, std::uint32_t value) {
    if (!loaded_) {
        load();
    }
    if (types_ == nullptr) {
        return unknown_;
    }
    const auto it = procs_.find((static_cast<std::uint64_t>(section) << 32) | value);
    if (it == procs_.end()) {
        return no_record_;
    }
    Proc& p = it->second;
    if (!p.computed) {
        p.computed = true;
        if (p.conflict) {
            p.signature.diag_id = pdb::diag::kBadObjectSymbols;
            p.signature.why = "two procedure records of " + object_name_ + " name this function with different types";
        } else {
            p.signature = pdb::function_signature(*types_, p.type_index, p.type_is_id);
        }
    }
    return p.signature;
}

}  // namespace cml::core::bind
