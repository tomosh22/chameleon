#pragma once

// Test-only builders for the bind planner tests (WP-07):
//   SynthObj   a synthetic x64 COFF object made with WP-06's writer (ObjectModel + write()),
//   SynthWorld a synthetic "old module": a PDB laid out by the pdb_builder of WP-04 and opened with
//              cml_pdb, with a TU compiland ("logic.cpp.obj") and a second one ("main.cpp.obj").
// Neither depends on a compiler, so every expectation is exact.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/coff/writer.h"
#include "cml/core/bind_plan.h"
#include "cml/pdb/index.h"
#include "doctest.h"
#include "../pdb/pdb_builder.h"
#include "../pdb/test_util.h"

namespace cml_bind_test {

namespace coff = cml::coff;
namespace tp = cml::testpdb;

inline constexpr std::uint32_t kText = coff::scn::CntCode | coff::scn::MemExecute | coff::scn::MemRead | 0x00500000u;
inline constexpr std::uint32_t kTextComdat = kText | coff::scn::LnkComdat;
inline constexpr std::uint32_t kData = coff::scn::CntInitializedData | coff::scn::MemRead | coff::scn::MemWrite | 0x00500000u;
inline constexpr std::uint32_t kRdata = coff::scn::CntInitializedData | coff::scn::MemRead | 0x00500000u;
inline constexpr std::uint32_t kDataComdat = kData | coff::scn::LnkComdat;
inline constexpr std::uint16_t kFnType = 0x20;  // DTYPE_FUNCTION

constexpr coff::RelocType kRel32 = coff::RelocType::Rel32;
constexpr coff::RelocType kRel32_1 = coff::RelocType::Rel32_1;
constexpr coff::RelocType kAddr64 = coff::RelocType::Addr64;
constexpr coff::RelocType kAddr32Nb = coff::RelocType::Addr32Nb;

struct Built {
    std::unique_ptr<coff::OwnedObject> owned;
    const coff::Object& obj() const { return owned->object; }
};

class SynthObj {
public:
    SynthObj() : m_(coff::ObjectModel::make_empty(coff::Machine::Amd64)) {}

    struct Func {
        std::uint32_t section = 0;
        std::uint32_t symbol = 0;  // raw symbol index of the leader
    };

    // Adds a section and its section-definition symbol; returns the 1-based section index.
    std::uint32_t section(const std::string& name, std::uint32_t chars, std::uint32_t size,
                          coff::ComdatSelection sel = coff::ComdatSelection::None, std::uint32_t associated = 0) {
        coff::ByteVector data(size, std::byte{0});
        auto idx = m_.add_section(name, chars, std::move(data));
        REQUIRE_MESSAGE(idx.has_value(), idx.error().format());
        coff::ByteVector aux = coff::make_section_definition_aux(false, size, 0, 0, associated, static_cast<std::uint8_t>(sel));
        auto s = m_.add_symbol(name, 0, static_cast<std::int32_t>(*idx), 0, coff::StorageClass::Static, std::move(aux));
        REQUIRE_MESSAGE(s.has_value(), s.error().format());
        return *idx;
    }

    // A COMDAT .text$mn section with an external leader `name`.
    Func function(const std::string& name, bool external = true, std::uint32_t size = 16,
                  coff::ComdatSelection sel = coff::ComdatSelection::Any) {
        Func f;
        f.section = section(".text$mn", kTextComdat, size, sel);
        f.symbol = symbol(name, 0, static_cast<std::int32_t>(f.section), true, external);
        return f;
    }

    // A non-COMDAT section's symbol (value = offset in the section).
    std::uint32_t symbol(const std::string& name, std::uint32_t value, std::int32_t section_number, bool is_function,
                         bool external = true) {
        auto s = m_.add_symbol(name, value, section_number, is_function ? kFnType : 0,
                               external ? coff::StorageClass::External : coff::StorageClass::Static);
        REQUIRE_MESSAGE(s.has_value(), s.error().format());
        return *s;
    }

    std::uint32_t undefined(const std::string& name, bool is_function = false) {
        auto s = m_.add_symbol(name, 0, 0, is_function ? kFnType : 0, coff::StorageClass::External);
        REQUIRE_MESSAGE(s.has_value(), s.error().format());
        return *s;
    }

    // Relocation at `offset` of section `sec` against raw symbol `sym`; writes the in-place addend.
    void reloc(std::uint32_t sec, std::uint32_t offset, std::uint32_t sym, coff::RelocType type, std::int64_t addend = 0) {
        coff::ModelSection& s = m_.sections[sec - 1];
        const bool wide = type == coff::RelocType::Addr64;
        const std::size_t width = wide ? 8u : 4u;
        if (s.data.size() < offset + width) {
            s.data.resize(offset + width, std::byte{0});
        }
        for (std::size_t i = 0; i < width; ++i) {
            s.data[offset + i] = static_cast<std::byte>((static_cast<std::uint64_t>(addend) >> (8 * i)) & 0xFF);
        }
        coff::ModelRelocation r;
        r.virtual_address = offset;
        r.symbol_index = sym;
        r.type = static_cast<std::uint16_t>(type);
        s.relocations.push_back(r);
    }

    coff::ObjectModel& model() { return m_; }

    Built build() {
        auto bytes = coff::write(m_);
        REQUIRE_MESSAGE(bytes.has_value(), bytes.error().format());
        std::vector<std::byte> v(bytes->begin(), bytes->end());
        auto obj = coff::Object::parse(std::span<const std::byte>(v.data(), v.size()));
        REQUIRE_MESSAGE(obj.has_value(), obj.error().format());
        Built b;
        b.owned = std::make_unique<coff::OwnedObject>(std::move(v), std::move(*obj));
        return b;
    }

private:
    coff::ObjectModel m_;
};

// The old module. Image sections: .text (1, rva 0x1000), .rdata (2, rva 0x2000), .data (3, rva 0x3000).
// Compiland 0 = the TU being patched, 1 = main.cpp.obj, 2 = a CRT object from a library.
class SynthWorld {
public:
    static constexpr std::uint64_t kBase = 0x140000000ull;
    static constexpr std::uint16_t kTextSec = 1;
    static constexpr std::uint16_t kRdataSec = 2;
    static constexpr std::uint16_t kDataSec = 3;

    SynthWorld() {
        spec.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020},
                         {".rdata", 0x800, 0x2000, 0x800, 0x40000040},
                         {".data", 0x800, 0x3000, 0x800, 0xC0000040}};
        tp::Module tu;
        tu.name = "C:\\proj\\obj\\logic.cpp.obj";
        tu.symbols.objname(tu.name);
        tu.symbols.compile3(1, 0, 0xD0, "Test Compiler 19.44");
        spec.modules.push_back(std::move(tu));
        tp::Module mn;
        mn.name = "C:\\proj\\obj\\main.cpp.obj";
        mn.symbols.objname(mn.name);
        mn.symbols.compile3(1, 0, 0xD0, "Test Compiler 19.44");
        spec.modules.push_back(std::move(mn));
        tp::Module crt;
        crt.name = "C:\\vc\\obj\\gs_support.obj";
        crt.lib = "C:\\vc\\lib\\libcmt.lib";
        crt.symbols.objname(crt.name);
        spec.modules.push_back(std::move(crt));
        // The TU owns .text 0x000..0x100 and .data 0x000..0x80; main.cpp the next ranges.
        spec.contributions = {{kTextSec, 0x000, 0x100, 0}, {kTextSec, 0x100, 0x100, 1}, {kTextSec, 0x200, 0x100, 2},
                              {kDataSec, 0x000, 0x80, 0},  {kDataSec, 0x80, 0x80, 1},   {kDataSec, 0x100, 0x40, 2}};
    }

    tp::Spec spec;

    tp::Module& tu() { return spec.modules[0]; }
    tp::Module& main_obj() { return spec.modules[1]; }

    // S_PUB32: code|function flags for functions.
    void pub_func(const std::string& name, std::uint32_t text_offset) { spec.symrec.pub32(name, 0x3, kTextSec, text_offset); }
    void pub_data(const std::string& name, std::uint32_t data_offset, std::uint16_t sec = kDataSec) {
        spec.symrec.pub32(name, 0, sec, data_offset);
    }
    // Module records (display names).
    void proc(tp::Module& m, const std::string& name, std::uint32_t text_offset, std::uint32_t len = 0x20, bool local = false) {
        m.symbols.proc(local ? 0x110F : 0x1110, name, kTextSec, text_offset, len);
        m.symbols.end();
    }
    void data(tp::Module& m, const std::string& name, std::uint32_t data_offset, bool local = true) {
        m.symbols.data(local ? 0x110C : 0x110D, name, kDataSec, data_offset);
    }
    // A proc with function-local statics.
    void proc_with_statics(tp::Module& m, const std::string& name, std::uint32_t text_offset,
                           const std::vector<std::pair<std::string, std::uint32_t>>& statics, std::uint32_t len = 0x30) {
        m.symbols.proc(0x1110, name, kTextSec, text_offset, len);
        for (const auto& s : statics) {
            m.symbols.data(0x110C, s.first, kDataSec, s.second);
        }
        m.symbols.end();
    }

    void open() {
        dir = std::make_unique<tp::TempDir>();
        const std::string path = dir->file("old.pdb");
        tp::write_file(path, tp::build(spec));
        cml::pdb::OpenOptions o;
        o.use_cache = false;
        auto r = cml::pdb::PdbIndex::open_pdb(path, o);
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        index = *r;
    }

    cml::core::bind::PlanInput input(const Built& obj, const std::string& compiland = "logic.cpp.obj") const {
        cml::core::bind::PlanInput in;
        in.object = &obj.obj();
        in.object_name = "logic.cpp.obj";
        in.module_pdb = index.get();
        in.module_name = "old.exe";
        in.module_base = kBase;
        in.compiland = compiland;
        in.registry = registry;
        in.modules = modules;
        return in;
    }

    const cml::core::SymbolRegistry* registry = nullptr;
    const cml::core::ProcessModules* modules = nullptr;

    std::unique_ptr<tp::TempDir> dir;
    std::shared_ptr<const cml::pdb::PdbIndex> index;
};

inline const cml::core::bind::SymbolDecision* find(const cml::core::bind::BindPlan& p, const std::string& name) {
    return p.find_decision(name);
}

inline bool has_fixup(const cml::core::bind::BindPlan& p, cml::core::bind::FixupKind kind, std::uint32_t section,
                      std::uint32_t offset, const std::string& symbol) {
    for (const auto& f : p.fixups) {
        if (f.kind == kind && f.section_index == section && f.offset == offset && f.symbol == symbol) {
            return true;
        }
    }
    return false;
}

}  // namespace cml_bind_test
