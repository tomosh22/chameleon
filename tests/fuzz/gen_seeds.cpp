// Regenerates the generated part of the fuzz seed corpus (WP-28):
//
//   cml_fuzz_gen_seeds <tests/fuzz/corpus>
//
// coff/   hand-built objects from the unit tests' independent builder (regular, bigobj, COMDAT, weak external, extended
//         relocations, long names) plus truncated and bit-flipped variants of them
// pe/     the unit tests' PE32+ image (imports, exports with a forwarder, debug, load config, TLS, .pdata)
// pdb/    the unit tests' synthetic PDB (several compilands, publics, sections, build info) with a small block size;
//         WP-37: PDBs with a TPI and an IPI stream (function types, a closure class, function ids, the type-index
//         offset table) and the raw `.debug$T` / `.debug$S` sections of an object (own records, a type-server
//         reference, precompiled types)
// ipc/    one frame of every message type, both directions, with realistic content
// control one line of every control-protocol shape
//
// The seeds are small on purpose. Reproducers of bugs fuzzing found are NOT written here: they are added to the corpus by
// hand next to a description in docs/wp/WP-28.md.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cml/ipc/messages.h"
#include "coff/obj_builder.h"
#include "pdb/pdb_builder.h"
#include "pdb/type_builder.h"
#include "pe/pe_builder.h"

namespace fs = std::filesystem;

namespace {

void write_seed(const fs::path& dir, const std::string& name, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(dir);
    std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::printf("%s/%s (%zu bytes)\n", dir.string().c_str(), name.c_str(), bytes.size());
}

void write_text(const fs::path& dir, const std::string& name, const std::string& text) {
    write_seed(dir, name, std::vector<std::uint8_t>(text.begin(), text.end()));
}

// A damaged copy: cut after `keep` bytes, or one byte replaced.
std::vector<std::uint8_t> truncated(std::vector<std::uint8_t> b, std::size_t keep) {
    if (keep < b.size()) {
        b.resize(keep);
    }
    return b;
}
std::vector<std::uint8_t> poked(std::vector<std::uint8_t> b, std::size_t at, std::uint8_t value) {
    if (at < b.size()) {
        b[at] = value;
    }
    return b;
}

// ---- COFF -------------------------------------------------------------------------------------------------------------

using namespace cml_test;

constexpr std::uint32_t kTextChars = 0x20u | 0x20000000u | 0x40000000u | 0x00500000u;  // code, execute, read, align 16

SymbolSpec sym(std::string name, std::int32_t section, std::uint32_t value = 0, std::uint8_t cls = 2, std::uint16_t type = 0) {
    SymbolSpec s;
    s.name = std::move(name);
    s.section = section;
    s.value = value;
    s.storage_class = cls;
    s.type = type;
    return s;
}

ObjSpec feature_spec(bool big) {
    ObjSpec o;
    o.bigobj = big;
    SectionSpec text;
    text.name = ".text";
    text.characteristics = kTextChars | 0x1000u;  // COMDAT
    text.data = Bytes(16, 0x90);
    text.relocs = {{0x4, 3, 4}, {0xC, 2, 3}};
    o.sections.push_back(text);
    SectionSpec data;
    data.name = ".data";
    data.characteristics = 0x40u | 0x40000000u | 0x80000000u;
    data.data = Bytes{1, 2, 3, 4, 5, 6, 7, 8};
    o.sections.push_back(data);
    const std::string drectve = " /DEFAULTLIB:\"LIBCMT\" /alternatename:foo=bar /EXPORT:baz /INCLUDE:_fltused";
    SectionSpec dr;
    dr.name = ".drectve";
    dr.characteristics = 0x200u | 0x800u;
    dr.data = Bytes(drectve.begin(), drectve.end());
    o.sections.push_back(dr);
    SectionSpec longname;
    longname.name = ".a_section_with_a_long_name";
    longname.characteristics = 0x40u | 0x40000000u;
    longname.data = Bytes{9};
    o.sections.push_back(longname);
    SectionSpec bss;
    bss.name = ".bss";
    bss.characteristics = 0x80u | 0x40000000u | 0x80000000u;
    bss.bss_size = 64;
    o.sections.push_back(bss);

    SymbolSpec text_sym = sym(".text", 1, 0, 3);
    text_sym.aux.push_back(aux_section_def(big, 16, 2, 0, 0xABCD, 0, 2));
    o.symbols.push_back(text_sym);
    o.symbols.push_back(sym("?func@@YAXXZ", 1, 0, 2, 0x20));
    o.symbols.push_back(sym("undefined_target", 0));
    SymbolSpec weak = sym("weak_ext", 0, 0, 105);
    weak.aux.push_back(aux_weak(6, 3));
    o.symbols.push_back(weak);
    o.symbols.push_back(sym("weak_default", 1, 8));
    SymbolSpec file = sym(".file", -2, 0, 103);
    file.aux.push_back(aux_file("fixture.cpp"));
    o.symbols.push_back(file);
    o.symbols.push_back(sym("@feat.00", -1, 0x80010190u, 3));
    SymbolSpec data_sym = sym(".data", 2, 0, 3);
    data_sym.aux.push_back(aux_section_def(big, 8, 0, 0, 0, 0, 0));
    o.symbols.push_back(data_sym);
    o.symbols.push_back(sym("a_rather_long_symbol_name_that_goes_to_the_string_table", 2, 4));
    return o;
}

ObjSpec minimal_spec() {
    ObjSpec o;
    SectionSpec text;
    text.name = ".text$mn";
    text.characteristics = kTextChars;
    text.data = Bytes{0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    o.sections.push_back(text);
    o.symbols.push_back(sym("?compute_value@@YAHXZ", 1, 0, 2, 0x20));
    return o;
}

ObjSpec extended_reloc_spec() {
    ObjSpec o;
    SectionSpec text;
    text.name = ".text";
    text.characteristics = kTextChars;
    text.data = Bytes(32, 0xCC);
    text.relocs = {{0, 1, 4}, {8, 1, 4}};
    text.extended_relocs = true;
    o.sections.push_back(text);
    o.symbols.push_back(sym("target", 0));
    return o;
}

ObjSpec comdat_associative_spec() {
    ObjSpec o;
    SectionSpec text;
    text.name = ".text$mn";
    text.characteristics = kTextChars | 0x1000u;
    text.data = Bytes(8, 0x90);
    o.sections.push_back(text);
    SectionSpec xdata;
    xdata.name = ".xdata";
    xdata.characteristics = 0x40u | 0x40000000u | 0x1000u;
    xdata.data = Bytes(8, 1);
    o.sections.push_back(xdata);
    SymbolSpec t = sym(".text$mn", 1, 0, 3);
    t.aux.push_back(aux_section_def(false, 8, 0, 0, 1, 0, 2));
    o.symbols.push_back(t);
    o.symbols.push_back(sym("?f@@YAXXZ", 1, 0, 2, 0x20));
    SymbolSpec x = sym(".xdata", 2, 0, 3);
    x.aux.push_back(aux_section_def(false, 8, 0, 0, 1, 1, 5));  // associative with section 1
    o.symbols.push_back(x);
    return o;
}

void coff_seeds(const fs::path& dir) {
    write_seed(dir, "minimal.bin", build_object(minimal_spec()).bytes);
    write_seed(dir, "features.bin", build_object(feature_spec(false)).bytes);
    write_seed(dir, "features_bigobj.bin", build_object(feature_spec(true)).bytes);
    write_seed(dir, "extended_relocs.bin", build_object(extended_reloc_spec()).bytes);
    write_seed(dir, "comdat_associative.bin", build_object(comdat_associative_spec()).bytes);
    const Bytes features = build_object(feature_spec(false)).bytes;
    write_seed(dir, "features_truncated_symtab.bin", truncated(features, features.size() - 40));
    write_seed(dir, "features_short_header.bin", truncated(features, 14));
    write_seed(dir, "features_bad_section_ptr.bin", poked(features, 20 + 20, 0xFF));
    write_seed(dir, "empty.bin", Bytes());
}

// ---- PE ---------------------------------------------------------------------------------------------------------------

void pe_seeds(const fs::path& dir) {
    const PeBytes pe = build_pe_on_disk();
    write_seed(dir, "image.bin", pe);
    write_seed(dir, "image_truncated_rdata.bin", truncated(pe, 0x500));
    write_seed(dir, "image_headers_only.bin", truncated(pe, 0x200));
    write_seed(dir, "image_bad_lfanew.bin", poked(pe, 0x3C, 0xF0));
    cml::testpdb::Bytes small = cml::testpdb::build_pe({0x3C, 0x83, 0xC3, 0x8C, 0x00, 0x87, 0x0D, 0xEB, 0x4C, 0x4C, 0x44, 0x20, 0x50, 0x44, 0x42, 0x2E}, 3,
                                                       "C:\\proj\\build\\fixture_basic.pdb");
    write_seed(dir, "codeview_only.bin", small);
}

// ---- PDB --------------------------------------------------------------------------------------------------------------

void pdb_seeds(const fs::path& dir) {
    using namespace cml::testpdb;
    Spec spec;
    spec.block_size = 512;
    spec.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}, {".rdata", 0x800, 0x2000, 0x800, 0x40000040}};
    Module m;
    m.name = "C:\\proj\\obj\\logic.cpp.obj";
    m.symbols.objname(m.name);
    m.symbols.compile3(1, (1u << 10) | (1u << 14) | (1u << 13), 0xD0, "Test Compiler 19.44");
    m.symbols.envblock({{"cwd", "C:\\proj"}, {"cl", "C:\\tools\\cl.exe"}, {"cmd", "/c /Zi /Od logic.cpp"}, {"src", "C:\\proj\\src\\logic.cpp"}});
    m.symbols.proc(0x1110, "compute_value", 1, 0x100, 0x20);
    m.symbols.end();
    m.symbols.data(0x110D, "g_tick", 2, 0x0);
    m.files = {"C:\\proj\\src\\logic.cpp", "C:\\proj\\src\\logic.h"};
    spec.modules.push_back(std::move(m));
    const Bytes tiny = build(spec);
    write_seed(dir, "tiny.bin", tiny);
    write_seed(dir, "tiny_truncated.bin", truncated(tiny, tiny.size() / 2));
    write_seed(dir, "tiny_bad_block_size.bin", poked(tiny, 32, 0x03));
    write_seed(dir, "tiny_bad_dir_size.bin", poked(tiny, 47, 0x7F));
    Spec fragmented = spec;
    fragmented.fragmented = true;
    write_seed(dir, "tiny_fragmented.bin", build(fragmented));
    write_seed(dir, "empty.bin", Bytes());

    // ---- WP-37: type streams ----
    // float floor(float); Vec2 floor(const Vec2&); the call operator of a closure; a function taking a pointer to an
    // array of an enum; ids for all of them. Filler records in between so that the offset table has several chunks.
    TypeSet types;
    const auto filler = [&types](int n) {
        for (int i = 0; i < n; ++i) {
            types.add(kLfPointer, lf_pointer(kTChar));
        }
    };
    const std::uint32_t args_float = types.add(kLfArgList, lf_arglist({kTFloat}));
    const std::uint32_t fn_float = types.add(kLfProcedure, lf_procedure(kTFloat, args_float, 1));
    filler(5);
    const std::uint32_t vec2 = types.add(kLfStructure, lf_udt("Vec2", ".?AUVec2@@", true));
    const std::uint32_t const_vec2 = types.add(kLfModifier, lf_modifier(vec2, 1));
    const std::uint32_t ref_vec2 = types.add(kLfPointer, lf_pointer(const_vec2, kRef64));
    const std::uint32_t args_vec2 = types.add(kLfArgList, lf_arglist({ref_vec2}));
    const std::uint32_t fn_vec2 = types.add(kLfProcedure, lf_procedure(vec2, args_vec2, 1));
    filler(5);
    const std::uint32_t closure = types.add(kLfClass, lf_udt("f::<lambda_1>", ".?AV<lambda_1>@?1??f@@YAXXZ@", true));
    const std::uint32_t const_closure = types.add(kLfModifier, lf_modifier(closure, 1));
    const std::uint32_t this_ptr = types.add(kLfPointer, lf_pointer(const_closure, kPtr64 | 0x400));
    const std::uint32_t args_none = types.add(kLfArgList, lf_arglist({}));
    const std::uint32_t fn_call = types.add(kLfMFunction, lf_mfunction(kTInt, closure, this_ptr, args_none, 0));
    filler(5);
    const std::uint32_t color = types.add(kLfEnum, lf_enum("Color", ".?AW4Color@@"));
    const std::uint32_t colors = types.add(kLfArray, lf_array(color, 16));
    const std::uint32_t ptr_colors = types.add(kLfPointer, lf_pointer(colors));
    const std::uint32_t c_struct = types.add(kLfStructure, lf_udt("point", ""));
    const std::uint32_t args_mixed = types.add(kLfArgList, lf_arglist({ptr_colors, c_struct, kTDouble}));
    const std::uint32_t fn_mixed = types.add(kLfProcedure, lf_procedure(kTVoid, args_mixed, 3));

    Spec typed;
    typed.block_size = 512;
    typed.sections = spec.sections;
    types.add_to(typed.tpi);
    const std::uint32_t id_float = typed.ipi.record(kLfFuncId, lf_func_id(fn_float, "floor"));
    const std::uint32_t id_call = typed.ipi.record(kLfMFuncId, lf_mfunc_id(closure, fn_call, "operator()"));
    Module tm;
    tm.name = "C:\\proj\\obj\\logic.cpp.obj";
    tm.symbols.objname(tm.name);
    const struct {
        std::uint16_t kind;
        const char* name;
        std::uint32_t type;
    } procs[] = {{0x110F, "floor", fn_float}, {0x110F, "floor", fn_vec2}, {0x110F, "`f'::`2'::<lambda_1>::operator()", fn_call},
                 {0x1110, "mixed", fn_mixed}, {0x1146, "floor", id_float}, {0x1147, "f::<lambda_1>::operator()", id_call}};
    std::uint32_t off = 0x100;
    for (const auto& p : procs) {
        tm.symbols.proc(p.kind, p.name, 1, off, 0x10, 0, p.type);
        tm.symbols.data(0x110C, "sname", 2, off);
        tm.symbols.end();
        off += 0x10;
    }
    typed.modules.push_back(std::move(tm));
    typed.type_offsets_every = 4;
    const Bytes with_types = build(typed);
    write_seed(dir, "types.bin", with_types);
    write_seed(dir, "types_truncated.bin", truncated(with_types, with_types.size() * 2 / 3));
    Spec typed_fragmented = typed;
    typed_fragmented.fragmented = true;
    write_seed(dir, "types_fragmented.bin", build(typed_fragmented));
    Spec no_table = typed;
    no_table.type_offsets_every = 0;
    write_seed(dir, "types_no_table.bin", build(no_table));
    {
        // A table whose second entry starts inside a record, and one that promises too many records.
        Spec bad = typed;
        bad.tpi_offsets_use_override = true;
        bad.tpi_offsets_override = typed.tpi.index_offsets(4);
        bad.tpi_offsets_override[12] = static_cast<std::uint8_t>(bad.tpi_offsets_override[12] + 2);
        write_seed(dir, "types_table_into_record.bin", build(bad));
        bad.tpi_offsets_override = typed.tpi.index_offsets(4);
        bad.tpi_offsets_override[8] = static_cast<std::uint8_t>(bad.tpi_offsets_override[8] + 3);
        write_seed(dir, "types_table_wrong_count.bin", build(bad));
    }
    {
        // A pointer that points at itself and a function whose argument list is the function.
        Spec cyclic = typed;
        const std::uint32_t self = cyclic.tpi.next_id();
        cyclic.tpi.record(kLfPointer, lf_pointer(self));
        const std::uint32_t args_self = cyclic.tpi.record(kLfArgList, lf_arglist(std::vector<std::uint32_t>(8, self + 2)));
        const std::uint32_t fn_self = cyclic.tpi.record(kLfProcedure, lf_procedure(self, args_self, 8));
        cyclic.modules[0].symbols.proc(0x110F, "cyclic", 1, 0x400, 0x10, 0, fn_self);
        cyclic.modules[0].symbols.end();
        write_seed(dir, "types_cyclic.bin", build(cyclic));
    }

    // ---- WP-37: the debug sections of an object ----
    TypeSet object_types = types;  // types and ids share one index space in an object
    const std::uint32_t oid_float = object_types.add(kLfFuncId, lf_func_id(fn_float, "floor"));
    const std::uint32_t oid_vec2 = object_types.add(kLfFuncId, lf_func_id(fn_vec2, "floor"));
    const std::uint32_t oid_call = object_types.add(kLfMFuncId, lf_mfunc_id(closure, fn_call, "operator()"));
    const Bytes debug_t_bytes = object_types.debug_t();
    write_seed(dir, "debug_t.bin", debug_t_bytes);
    write_seed(dir, "debug_t_truncated.bin", truncated(debug_t_bytes, debug_t_bytes.size() - 7));
    {
        TypeSet server;
        server.add(kLfTypeServer2, lf_typeserver2({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, 3, "C:\\proj\\obj\\vc143.pdb"));
        write_seed(dir, "debug_t_typeserver.bin", server.debug_t());
        TypeSet own;
        own.set_first(0x1100);
        const std::uint32_t own_args = own.add(kLfArgList, lf_arglist({0x1010, kTInt}));
        const std::uint32_t own_fn = own.add(kLfProcedure, lf_procedure(kTInt, own_args, 2));
        own.add(kLfFuncId, lf_func_id(own_fn, "pick"));
        const std::pair<std::uint16_t, Bytes> head(kLfPrecomp, lf_precomp(0x1000, 0x100, "C:\\proj\\obj\\pch.obj"));
        write_seed(dir, "debug_t_precomp.bin", own.debug_t(&head));
    }
    const Bytes debug_s_bytes = debug_s({{0x1146, "floor", oid_float}, {0x1146, "floor", oid_vec2}, {0x1147, "f::<lambda_1>::operator()", oid_call}, {0x110F, "old_style", fn_mixed}});
    write_seed(dir, "debug_s.bin", debug_s_bytes);
    write_seed(dir, "debug_s_truncated.bin", truncated(debug_s_bytes, debug_s_bytes.size() - 9));
    // ADR-0042, amended: function-local statics in blocks (and in an inline site), in an object and in a module stream.
    const auto with_statics = [](Records& r, std::uint16_t code_seg, std::uint16_t data_seg, std::uint32_t code, std::uint32_t data) {
        r.proc(0x1110, "get_argument_options", code_seg, code, 0x10);
        r.block();
        r.data(0x110C, "names", data_seg, data);
        r.data(0x110C, "$TSS0", data_seg, data + 8);
        r.block();
        r.data(0x1112, "deep", data_seg, data + 0x10);
        r.end();
        r.end();
        r.block();
        r.data(0x110C, "names", data_seg, data + 0x18);
        r.end();
        r.add(0x114D, Bytes(12, 0));
        r.data(0x110C, "inlined", data_seg, data + 0x20);
        r.add(0x114E, Bytes());
        r.end();
    };
    Records object_records;
    with_statics(object_records, 0, 0, 0, 0);
    write_seed(dir, "debug_s_statics.bin", debug_s_section(object_records.bytes()));
    Spec blocks;
    blocks.sections = spec.sections;
    Module bm;
    bm.name = "C:\\proj\\obj\\logic.cpp.obj";
    bm.symbols.objname(bm.name);
    with_statics(bm.symbols, 1, 2, 0x100, 0x40);
    blocks.modules.push_back(std::move(bm));
    write_seed(dir, "statics_blocks.bin", build(blocks));
}

// ---- IPC --------------------------------------------------------------------------------------------------------------

template <class M>
void frame(const fs::path& dir, const std::string& name, const M& message) {
    const cml::Result<std::vector<std::uint8_t>> bytes = cml::ipc::encode_frame(cml::ipc::Message(message));
    if (!bytes) {
        std::fprintf(stderr, "cannot encode %s: %s\n", name.c_str(), bytes.error().format().c_str());
        std::exit(1);
    }
    write_seed(dir, name + ".frame", *bytes);
}

void ipc_seeds(const fs::path& dir) {
    using namespace cml::ipc;
    Hello hello;
    hello.request_id = 1;
    hello.pid = 4242;
    hello.mode = AgentMode::Synchronized;
    hello.agent_abi_version = 2;
    hello.agent_path = "C:\\app\\cml_agent.dll";
    hello.command_line = "app.exe --agent";
    hello.restart_token = "tok";
    frame(dir, "hello", hello);
    Welcome welcome;
    welcome.request_id = 1;
    welcome.agent_id = 7;
    welcome.broker_version = "0.1.0";
    frame(dir, "welcome", welcome);
    ModuleList list;
    list.request_id = 2;
    list.modules = {{0x140000000ull, 0x10000, "C:\\app\\app.exe"}, {0x7FF800000000ull, 0x20000, "C:\\Windows\\System32\\ntdll.dll"}};
    frame(dir, "module_list", list);
    frame(dir, "enable_module", EnableModule{3, kModuleOptionAllImports, "C:\\app\\app.exe"});
    frame(dir, "disable_module", DisableModule{4, "app.exe"});
    frame(dir, "module_loaded", ModuleLoaded{{0x180000000ull, 0x4000, "C:\\app\\plugin.dll"}});
    frame(dir, "module_unloaded", ModuleUnloaded{{0x180000000ull, 0x4000, "C:\\app\\plugin.dll"}});
    frame(dir, "log", Log{3, "reload: \xC3\xA4 sweep done"});
    frame(dir, "reply", Reply{5, 4203, "the address range was taken"});

    ReserveRange reserve;
    reserve.request_id = 6;
    reserve.module_base = 0x140000000ull;
    reserve.module_size = 0x10000;
    frame(dir, "reserve_range", reserve);
    ReserveRangeResult reserved;
    reserved.request_id = 6;
    reserved.message = "reserved 2 block(s)";
    reserved.blocks = {{0x150000000ull, 64ull << 20}, {0x154000000ull, 64ull << 20}};
    frame(dir, "reserve_range_result", reserved);
    frame(dir, "release_range", ReleaseRange{7, 0x150000000ull, 0, ReleaseMode::UnloadPatch});

    LoadPatch load;
    load.request_id = 8;
    load.patch_id = 1;
    PatchCandidate cand;
    cand.path = "C:\\tmp\\patch_1_1.dll";
    cand.image_base = 0x150000000ull;
    cand.image_size = 0x5000;
    cand.fixups = {{0x1010, FixupKind::Rel32, 4, 0x140001000ull}, {0x2000, FixupKind::Addr64, 0, 0x140003000ull}, {0x2008, FixupKind::SlotWrite, 0, 0x140001100ull}};
    load.candidates.push_back(cand);
    load.initializer_rvas = {0x3000, 0x3008};
    frame(dir, "load_patch", load);
    PatchLoaded loaded;
    loaded.request_id = 8;
    loaded.patch_id = 1;
    loaded.message = "loaded";
    loaded.image_base = 0x150000000ull;
    loaded.image_size = 0x5000;
    loaded.attempts = 1;
    loaded.fixups_applied = 3;
    frame(dir, "patch_loaded", loaded);
    ApplyFixups apply;
    apply.request_id = 9;
    apply.image_base = 0x150000000ull;
    apply.fixups = cand.fixups;
    frame(dir, "apply_fixups", apply);
    frame(dir, "run_initializers", RunInitializers{10, 0x150000000ull, {0x3000}});

    InstallHooks hooks;
    hooks.request_id = 11;
    hooks.hooks = {{0x140001000ull, 0x150001000ull, "compute_value"}, {0x140001100ull, 0x150001100ull, "helper"}};
    frame(dir, "install_hooks", hooks);
    HooksInstalled installed;
    installed.request_id = 11;
    installed.attempts = 1;
    installed.threads = 4;
    installed.window_ms = 2;
    installed.results = {{HookStrategy::FiveByte, HookResultStatus::Installed, 0, ""}, {HookStrategy::PaddingTwoByte, HookResultStatus::Failed, 4301, "too short"}};
    frame(dir, "hooks_installed", installed);

    Exception ex;
    ex.exception_id = 1;
    ex.thread_id = 99;
    ex.code = 0xC0000005u;
    ex.access = 0;
    ex.address = 0x140001234ull;
    ex.data_address = 8;
    ex.stack_pointer = 0x000000AABBCC0000ull;
    ex.module_base = 0x140000000ull;
    ex.module_path = "C:\\app\\app.exe";
    frame(dir, "exception", ex);
    frame(dir, "exception_action", ExceptionAction{12, 1, ExceptionActionKind::Leave});
    frame(dir, "exception_config", ExceptionConfig{13, kExceptionConfigEnabled});
    frame(dir, "compile_event", CompileEvent{CompilePhase::Error, 2200, "app.exe", "logic.cpp(3): error C2065"});

    frame(dir, "arm_fault", ArmFault{16, "hook-install", 1});
    frame(dir, "arm_fault_none", ArmFault{17, "none", 1});
    frame(dir, "restart", Restart{14, 0});
    frame(dir, "restart_cancel", RestartCancel{14});
    RestartInfo info;
    info.request_id = 14;
    info.hooks_run = 1;
    info.flags = kRestartInfoHadConsole;
    info.image_path = u"C:\\app\\app.exe";
    info.command_line = u"app.exe --agent";
    info.working_directory = u"C:\\app";
    info.environment = std::u16string(u"A=b\0C=d\0\0", 9);
    info.modules = {{kModuleOptionAllImports, "app.exe"}};
    frame(dir, "restart_info", info);
    ReenableModules re;
    re.request_id = 15;
    re.modules = {{0, "plugin.dll"}};
    frame(dir, "reenable_modules", re);

    // A few raw shapes: unknown type, wrong version, oversized length, empty.
    write_seed(dir, "unknown_type.frame", {0, 0, 0, 0, 0xEF, 0xBE, 1, 0});
    write_seed(dir, "wrong_version.frame", {0, 0, 0, 0, 0x03, 0x00, 9, 0});
    write_seed(dir, "huge_length.frame", {0xFF, 0xFF, 0xFF, 0x7F, 0x03, 0x00, 1, 0});
    write_seed(dir, "empty.frame", {});
}

// ---- control pipe -----------------------------------------------------------------------------------------------------

void control_seeds(const fs::path& dir) {
    write_text(dir, "request_status.json", R"({"id":1,"cmd":"status","args":{}})");
    write_text(dir, "request_reload.json", R"({"id":2,"cmd":"reload","args":{"pid":4242,"module":"app.exe","timeout_ms":60000,"confirm":true}})");
    write_text(dir, "request_set_option.json", R"({"id":3,"cmd":"set-option","args":{"name":"narrow_hooks","value":false}})");
    write_text(dir, "request_subscribe.json", R"({"id":4,"cmd":"subscribe","args":{"events":["diag","patch_applied","log"]}})");
    write_text(dir, "request_unicode.json", "{\"id\":5,\"cmd\":\"wait\",\"args\":{\"for\":\"index\",\"note\":\"caf\\u00e9 \\ud83d\\ude00 \xC3\xA9\"}}");
    write_text(dir, "response_ok.json",
               R"({"id":1,"ok":true,"result":{"version":"0.1.0","capabilities":["status","reload","wait","set-option","subscribe","diag-events"],"processes":[{"pid":4242,"modules":[{"path":"C:\\app\\app.exe","enabled":true,"index":{"state":"ready","percent":100,"message":""}}]}],"truncated":false,"generation":3,"reload_running":false}})");
    write_text(dir, "response_error.json",
               R"({"id":2,"ok":false,"result":{},"diag":{"id":"CML5304","message":"a reload is already running","action":"Wait for it."}})");
    write_text(dir, "event_diag.json", R"({"event":"diag","id":"CML3101","message":"x","action":"y","severity":"error","stage":"link","pid":4242})");
    write_text(dir, "event_patch_applied.json", R"({"event":"patch_applied","pid":4242,"generation":1,"timing_ms":{"total":12.5,"link":3.25}})");
    write_text(dir, "numbers.json", R"([0,-0,1,-1,1e5,-1.5E-3,9223372036854775807,-9223372036854775808,18446744073709551616,0.1,1E400])");
    write_text(dir, "nested.json", R"({"a":{"b":{"c":[[[[1,2,{"d":null,"e":true,"f":false}]]]]}}})");
    write_text(dir, "escapes.json", R"("\"\\\/\b\f\n\r\t\u0041\u00e9\ud83d\ude00")");
    write_text(dir, "bad_lone_surrogate.json", R"("\ud800")");
    write_text(dir, "bad_trailing_comma.json", R"({"id":1,"cmd":"status",})");
    write_text(dir, "bad_duplicate_key.json", R"({"id":1,"id":2,"cmd":"status"})");
    write_text(dir, "bad_unterminated.json", R"({"id":1,"cmd":"sta)");
    write_text(dir, "bad_utf8.json", std::string("{\"cmd\":\"\xC0\xAF\"}"));
    write_text(dir, "deep.json", std::string(40, '[') + std::string(40, ']'));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: cml_fuzz_gen_seeds <corpus directory>\n");
        return 2;
    }
    const fs::path root(argv[1]);
    coff_seeds(root / "coff");
    pe_seeds(root / "pe");
    pdb_seeds(root / "pdb");
    ipc_seeds(root / "ipc");
    control_seeds(root / "control");
    return 0;
}
