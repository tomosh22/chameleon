#include "cml/coff/binding.h"

#include <string>
#include <unordered_set>
#include <utility>

#include "cml/util/log.h"

namespace cml::coff {

namespace {

constexpr std::uint32_t kTextCharacteristics =
    scn::CntCode | (5u << scn::AlignShift) /* 16 bytes */ | scn::MemExecute | scn::MemRead;
constexpr std::uint32_t kDataCharacteristics =
    scn::CntInitializedData | (4u << scn::AlignShift) /* 8 bytes */ | scn::MemRead | scn::MemWrite;
constexpr std::uint16_t kRel32 = static_cast<std::uint16_t>(RelocType::Rel32);
constexpr std::uint16_t kTypeFunction = 0x20;  // DTYPE_FUNCTION << 4

}  // namespace

std::string slot_name(std::string_view function_name) { return std::string(kSlotPrefix).append(function_name); }

Result<ObjectModel> make_binding_model(std::span<const BindSymbol> symbols) {
    std::size_t functions = 0;
    std::size_t datas = 0;
    std::unordered_set<std::string> seen;
    seen.reserve(symbols.size() * 2);
    for (const BindSymbol& s : symbols) {
        if (s.name.empty() || s.name.find('\0') != std::string::npos) {
            return Diag::make(diag::kBindNameInvalid, "bind symbol name is empty or contains a NUL byte",
                              "Pass the decorated linker name of the symbol to bind.");
        }
        if (s.name == "__ImageBase") {
            return Diag::make(diag::kBindNameInvalid,
                              "'__ImageBase' is provided by the linker and cannot be bound",
                              "Leave __ImageBase out of the bind list; the linker resolves it itself.");
        }
        if (!seen.insert(s.name).second) {
            return Diag::make(diag::kBindNameDuplicate, "symbol '" + s.name + "' is listed twice for binding",
                              "List each symbol once.");
        }
        if (s.kind == BindKind::Function) {
            ++functions;
            if (!seen.insert(slot_name(s.name)).second) {
                return Diag::make(diag::kBindNameDuplicate,
                                  "slot symbol '" + slot_name(s.name) + "' collides with another bound symbol",
                                  "Do not bind symbols whose names start with __cml_slot_.");
            }
        } else {
            ++datas;
        }
    }
    // Everything must fit 32-bit offsets, and symbol/section numbers must fit the regular format.
    if (functions > 0x00FFFFFFull || datas > 0x1FFFFFFFull || functions + datas > 0x1FFFFFFFull) {
        return Diag::make(diag::kBindTooMany,
                          "binding object would exceed the COFF size limits (" + std::to_string(functions) +
                              " functions, " + std::to_string(datas) + " data symbols)",
                          "Bind fewer symbols per patch.");
    }

    ObjectModel m = ObjectModel::make_empty(Machine::Amd64, false);
    const bool want_text = functions > 0;
    const bool want_data = functions + datas > 0;
    const std::uint32_t text_size = static_cast<std::uint32_t>(functions) * kThunkSize;
    const std::uint32_t data_size = static_cast<std::uint32_t>(functions + datas) * kSlotSize;

    std::uint32_t text_section = 0;
    std::uint32_t data_section = 0;
    std::uint32_t next_section = 1;
    if (want_text) text_section = next_section++;
    if (want_data) data_section = next_section++;

    // Raw symbol indices: [.text def + aux] [.data def + aux] then per symbol in input order.
    std::uint32_t next_symbol = (want_text ? 2u : 0u) + (want_data ? 2u : 0u);

    ByteVector text(text_size, std::byte{0xCC});
    std::vector<ModelRelocation> text_relocs;
    text_relocs.reserve(functions);

    std::vector<ModelSymbol> body;
    body.reserve(symbols.size() * 2);
    std::uint32_t thunk_index = 0;
    std::uint32_t slot_cursor = 0;

    auto push_symbol = [&](const std::string& name, std::uint32_t value, std::int32_t section, std::uint16_t type) -> Result<void> {
        auto raw = m.encode_symbol_name(name);
        if (!raw) {
            return raw.error();
        }
        ModelSymbol sym;
        sym.name = name;
        sym.raw_name = *raw;
        sym.value = value;
        sym.section_number = section;
        sym.type = type;
        sym.storage_class = static_cast<std::uint8_t>(StorageClass::External);
        body.push_back(std::move(sym));
        ++next_symbol;
        return {};
    };

    for (const BindSymbol& s : symbols) {
        if (s.kind == BindKind::Function) {
            const std::uint32_t thunk_offset = thunk_index * kThunkSize;
            const std::uint32_t slot_offset = slot_cursor;
            slot_cursor += kSlotSize;
            // Symbol order: thunk, then its slot. The slot's raw index is therefore thunk + 1.
            const std::uint32_t slot_symbol = next_symbol + 1;
            if (auto r = push_symbol(s.name, thunk_offset, static_cast<std::int32_t>(text_section), kTypeFunction); !r) {
                return r.error();
            }
            if (auto r = push_symbol(slot_name(s.name), slot_offset, static_cast<std::int32_t>(data_section), 0); !r) {
                return r.error();
            }
            text[thunk_offset + 0] = std::byte{0xFF};
            text[thunk_offset + 1] = std::byte{0x25};
            for (std::uint32_t k = 2; k < 6; ++k) {
                text[thunk_offset + k] = std::byte{0};  // rel32, filled by the linker
            }
            text_relocs.push_back({thunk_offset + 2, slot_symbol, kRel32});
            ++thunk_index;
        } else {
            const std::uint32_t offset = slot_cursor;
            slot_cursor += kSlotSize;
            if (auto r = push_symbol(s.name, offset, static_cast<std::int32_t>(data_section), 0); !r) {
                return r.error();
            }
        }
    }

    // Sections first (symbols refer to them), then the symbols in the order planned above.
    const std::uint32_t reloc_count = static_cast<std::uint32_t>(text_relocs.size());
    if (want_text) {
        auto idx = m.add_section(".text", kTextCharacteristics, std::move(text), std::move(text_relocs));
        if (!idx) {
            return idx.error();
        }
    }
    if (want_data) {
        auto idx = m.add_section(".data", kDataCharacteristics, ByteVector(data_size, std::byte{0}));
        if (!idx) {
            return idx.error();
        }
    }
    const auto add_section_symbol = [&](const char* name, std::uint32_t section, std::uint32_t length,
                                        std::uint32_t relocs) -> Result<void> {
        ModelSymbol sym;
        sym.name = name;
        auto raw = m.encode_symbol_name(name);
        if (!raw) {
            return raw.error();
        }
        sym.raw_name = *raw;
        sym.section_number = static_cast<std::int32_t>(section);
        sym.storage_class = static_cast<std::uint8_t>(StorageClass::Static);
        sym.aux = make_section_definition_aux(false, length, relocs);
        m.symbols.push_back(std::move(sym));
        return {};
    };
    if (want_text) {
        if (auto r = add_section_symbol(".text", text_section, text_size, reloc_count); !r) {
            return r.error();
        }
    }
    if (want_data) {
        if (auto r = add_section_symbol(".data", data_section, data_size, 0); !r) {
            return r.error();
        }
    }
    for (ModelSymbol& s : body) {
        m.symbols.push_back(std::move(s));
    }
    m.has_symbol_table = true;
    CML_LOG(Trace, "coff: binding object model: %zu functions, %zu data symbols, %zu symbol records", functions, datas,
            static_cast<std::size_t>(m.raw_symbol_count()));
    return m;
}

Result<ByteVector> make_binding_object(std::span<const BindSymbol> symbols) {
    auto model = make_binding_model(symbols);
    if (!model) {
        return model.error();
    }
    return write(*model);
}

}  // namespace cml::coff
