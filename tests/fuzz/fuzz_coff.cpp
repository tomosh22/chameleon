// Fuzz target: the COFF object reader and the object writer's model (WP-28).
// Input = the bytes of an .obj file. Everything the reader exposes is walked, because "parse succeeded" promises
// that every accessor is bounds-safe (core/cml_coff/include/cml/coff/coff.h).
#include <optional>
#include <string_view>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/coff/writer.h"
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace cml;
    const coff::ByteSpan bytes(reinterpret_cast<const std::byte*>(data), size);
    Result<coff::Object> parsed = coff::Object::parse(bytes);
    if (!parsed) {
        fuzz::use(parsed.error().code);
        return 0;
    }
    const coff::Object& obj = *parsed;
    std::uint64_t acc = obj.header().section_count + obj.header().symbol_count;
    for (const coff::Section& sec : obj.sections()) {
        acc += sec.name.size() + sec.data.size() + sec.alignment();
        Result<std::vector<coff::ResolvedRelocation>> resolved = obj.resolve_relocations(sec);
        acc += resolved ? resolved->size() : 0;
        for (const coff::Relocation& r : obj.relocations(sec)) {
            acc += r.symbol_index + r.virtual_address;
        }
        if (const std::optional<coff::ComdatInfo> c = obj.comdat(sec)) {
            acc += c->associated_section + (c->leader != nullptr ? c->leader->name.size() : 0);
        }
        acc += obj.associative_children(sec).size();
    }
    for (const coff::Symbol& sym : obj.symbols()) {
        acc += sym.name.size();
        acc += obj.section_of(sym) != nullptr ? 1 : 0;
        acc += obj.section_definition(sym) ? 1 : 0;
        acc += obj.function_definition(sym) ? 1 : 0;
        acc += obj.weak_external(sym) ? 1 : 0;
        if (const std::optional<std::string_view> f = obj.file_name(sym)) {
            acc += f->size();
        }
        acc += obj.weak_external_default(sym) != nullptr ? 1 : 0;
        acc += obj.find_symbol(sym.name) != nullptr ? 1 : 0;
        acc += obj.symbol_at(sym.index) != nullptr ? 1 : 0;
    }
    for (const coff::Directive& d : obj.directives()) {
        acc += d.text.size();
    }
    // The writer: a model of whatever the reader accepted, and the byte-exact rewrite. They may refuse (a diagnostic) but
    // never crash.
    Result<coff::ObjectModel> model = coff::ObjectModel::from_object(obj);
    if (model) {
        Result<coff::ByteVector> out = coff::write(*model);
        acc += out ? out->size() : 0;
    }
    Result<coff::ByteVector> again = coff::rewrite(obj);
    acc += again ? again->size() : 0;
    fuzz::use(acc);
    return 0;
}
