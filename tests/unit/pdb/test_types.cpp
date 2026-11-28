// WP-37 (ADR-0042): the type reader. Canonical signatures from a PDB's TPI / IPI streams, from an object's `.debug$T`
// and from a type-server PDB; lazy and sparse reading; the object's `.debug$S` procedure records; malformed input.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "cml/pdb/pdb.h"
#include <ostream>  // doctest streams string_view operands; MSVC needs the full definition

#include "doctest.h"
#include "test_util.h"
#include "type_builder.h"

using namespace cml::testpdb;
using cml::pdb::FunctionSignature;
using cml::pdb::OpenOptions;
using cml::pdb::PdbIndex;
using cml::pdb::SymbolView;
using cml::pdb::TypeStreamStats;
namespace dg = cml::pdb::diag;

namespace {

OpenOptions no_cache() {
    OpenOptions o;
    o.use_cache = false;
    return o;
}

std::span<const std::uint8_t> span_of(const Bytes& b) {
    return std::span<const std::uint8_t>(b.data(), b.size());
}

// The types of a small program, in the order a compiler could emit them:
//   struct Vec2;  float floor(float);  Vec2 floor(const Vec2&);  and a closure `f::<lambda_1>` with operator().
struct Program {
    std::uint32_t args_float = 0, fn_float = 0;
    std::uint32_t vec2 = 0, args_vec2 = 0, fn_vec2 = 0;
    std::uint32_t closure = 0, fn_call = 0;
    std::uint32_t args_none = 0;
};

inline constexpr const char* kClosureUnique = ".?AV<lambda_1>@?1??f@@YAXXZ@";

// `padding` extra records go in front, so the indices of the same types differ between two sets.
Program add_program(TypeSet& t, std::uint32_t padding = 0) {
    for (std::uint32_t i = 0; i < padding; ++i) {
        t.add(kLfPointer, lf_pointer(kTChar));
    }
    Program p;
    p.args_float = t.add(kLfArgList, lf_arglist({kTFloat}));
    p.fn_float = t.add(kLfProcedure, lf_procedure(kTFloat, p.args_float, 1));
    p.vec2 = t.add(kLfStructure, lf_udt("Vec2", ".?AUVec2@@", true));
    const std::uint32_t const_vec2 = t.add(kLfModifier, lf_modifier(p.vec2, 1));
    const std::uint32_t ref = t.add(kLfPointer, lf_pointer(const_vec2, kRef64));
    p.args_vec2 = t.add(kLfArgList, lf_arglist({ref}));
    p.fn_vec2 = t.add(kLfProcedure, lf_procedure(p.vec2, p.args_vec2, 1));
    p.closure = t.add(kLfClass, lf_udt("f::<lambda_1>", kClosureUnique, true));
    const std::uint32_t const_closure = t.add(kLfModifier, lf_modifier(p.closure, 1));
    const std::uint32_t this_ptr = t.add(kLfPointer, lf_pointer(const_closure, kPtr64 | 0x400));  // const pointer
    p.args_none = t.add(kLfArgList, lf_arglist({}));
    p.fn_call = t.add(kLfMFunction, lf_mfunction(kTInt, p.closure, this_ptr, p.args_none, 0));
    return p;
}

const char* const kSigFloat = "F(cc=0,fa=0,r=t40,a=[t40])";
const char* const kSigVec2 = "F(cc=0,fa=0,r=U{.?AUVec2@@},a=[P1002C(M1(U{.?AUVec2@@}))])";
const char* const kSigCall = "MF(cc=0,fa=0,r=t74,c=U{.?AV<lambda_1>@?1??f@@YAXXZ@},this=P1040C(M1(U{.?AV<lambda_1>@?1??f@@YAXXZ@})),adj=0,a=[])";

// A PDB with one module whose procs carry the given (kind, name, type) triples.
struct Proc {
    std::uint16_t kind;
    const char* name;
    std::uint32_t type;
};
Spec spec_with(const std::vector<Proc>& procs) {
    Spec s;
    s.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}, {".data", 0x400, 0x3000, 0x400, 0xC0000040}};
    Module m;
    m.name = "C:\\proj\\obj\\logic.cpp.obj";
    m.symbols.objname(m.name);
    std::uint32_t off = 0x100;
    for (const Proc& p : procs) {
        m.symbols.proc(p.kind, p.name, 1, off, 0x10, 0, p.type);
        m.symbols.data(0x110C, "sname", 2, off);  // a function-local static of every proc
        m.symbols.end();
        off += 0x10;
    }
    s.modules.push_back(std::move(m));
    return s;
}

std::shared_ptr<const PdbIndex> open_spec(const TempDir& dir, const Spec& spec, const char* name = "t.pdb") {
    const std::string path = dir.file(name);
    write_file(path, build(spec));
    auto r = PdbIndex::open_pdb(path, no_cache());
    REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? std::string() : r.error().format()));
    return *r;
}

std::vector<SymbolView> procs_of(const PdbIndex& idx) {
    auto syms = idx.compiland_symbols(0);
    REQUIRE(syms.has_value());
    std::vector<SymbolView> out;
    for (const SymbolView& v : *syms) {
        if (cml::pdb::is_proc_kind(v.kind)) {
            out.push_back(v);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("types: canonical signatures from the TPI stream, through type and id indices") {
    TempDir dir;
    TypeSet types;
    const Program p = add_program(types);
    Spec spec;
    {
        // The _ID procs name an LF_FUNC_ID / LF_MFUNC_ID of the IPI stream, which names the function type.
        const std::uint32_t id_float = spec.ipi.record(kLfFuncId, lf_func_id(p.fn_float, "floor"));
        const std::uint32_t id_call = spec.ipi.record(kLfMFuncId, lf_mfunc_id(p.closure, p.fn_call, "operator()"));
        Spec with = spec_with({{0x110F, "floor", p.fn_float}, {0x110F, "floor", p.fn_vec2}, {0x110F, "f::<lambda_1>::operator()", p.fn_call},
                               {0x1146, "floor", id_float}, {0x1147, "f::<lambda_1>::operator()", id_call}});
        with.ipi = spec.ipi;
        spec = std::move(with);
    }
    types.add_to(spec.tpi);
    const auto idx = open_spec(dir, spec);

    // Opening the index reads no type record and not even the stream header.
    CHECK_FALSE(idx->type_stream_stats(true).opened);
    CHECK_FALSE(idx->type_stream_stats(false).opened);

    const std::vector<SymbolView> procs = procs_of(*idx);
    REQUIRE(procs.size() == 5);
    CHECK_FALSE(procs[0].type_is_id);
    CHECK(procs[3].type_is_id);
    CHECK(procs[4].type_is_id);
    CHECK(procs[3].kind == cml::pdb::SymbolKind::LocalProc);
    CHECK(procs[4].kind == cml::pdb::SymbolKind::GlobalProc);

    const FunctionSignature f = idx->proc_signature(procs[0]);
    REQUIRE_MESSAGE(f.known, f.why);
    CHECK(f.text == kSigFloat);
    CHECK_FALSE(f.member);
    CHECK(idx->type_stream_stats(true).opened);
    CHECK_FALSE(idx->type_stream_stats(false).opened);  // no id was looked up yet

    const FunctionSignature v = idx->proc_signature(procs[1]);
    REQUIRE_MESSAGE(v.known, v.why);
    CHECK(v.text == kSigVec2);
    CHECK(v.text != f.text);

    const FunctionSignature c = idx->proc_signature(procs[2]);
    REQUIRE_MESSAGE(c.known, c.why);
    CHECK(c.text == kSigCall);
    CHECK(c.member);
    CHECK(c.class_unique_name == kClosureUnique);
    CHECK(c.class_name == "f::<lambda_1>");

    // The same functions through the id stream.
    const FunctionSignature fid = idx->proc_signature(procs[3]);
    REQUIRE_MESSAGE(fid.known, fid.why);
    CHECK(fid.text == kSigFloat);
    CHECK(idx->type_stream_stats(false).opened);
    const FunctionSignature cid = idx->proc_signature(procs[4]);
    REQUIRE_MESSAGE(cid.known, cid.why);
    CHECK(cid.text == kSigCall);
    CHECK(cid.class_unique_name == kClosureUnique);

    // A data symbol has no signature.
    auto syms = idx->compiland_symbols(0);
    REQUIRE(syms.has_value());
    for (const SymbolView& s : *syms) {
        if (!cml::pdb::is_proc_kind(s.kind)) {
            CHECK_FALSE(idx->proc_signature(s).known);
        }
    }
}

TEST_CASE("types: the same function has the same signature in a PDB and in an object's .debug$T (other indices)") {
    TempDir dir;
    TypeSet pdb_types;
    const Program in_pdb = add_program(pdb_types);
    Spec spec = spec_with({{0x110F, "floor", in_pdb.fn_float}, {0x110F, "floor", in_pdb.fn_vec2}, {0x110F, "f::<lambda_1>::operator()", in_pdb.fn_call}});
    pdb_types.add_to(spec.tpi);
    const auto idx = open_spec(dir, spec);
    const std::vector<SymbolView> procs = procs_of(*idx);

    // The object: 37 unrelated records first, then the same types, then the ids in the same index space.
    TypeSet obj_types;
    const Program in_obj = add_program(obj_types, 37);
    CHECK(in_obj.fn_vec2 != in_pdb.fn_vec2);
    const std::uint32_t id_float = obj_types.add(kLfFuncId, lf_func_id(in_obj.fn_float, "floor"));
    const std::uint32_t id_vec2 = obj_types.add(kLfFuncId, lf_func_id(in_obj.fn_vec2, "floor"));
    const std::uint32_t id_call = obj_types.add(kLfMFuncId, lf_mfunc_id(in_obj.closure, in_obj.fn_call, "operator()"));
    const Bytes section = obj_types.debug_t();
    auto parsed = cml::pdb::ObjectTypes::parse(span_of(section));
    REQUIRE_MESSAGE(parsed.has_value(), (parsed.has_value() ? std::string() : parsed.error().format()));
    CHECK(parsed->record_count() == obj_types.size());
    CHECK(parsed->info().kind == cml::pdb::DebugTInfo::Kind::Records);

    const std::uint32_t ids[] = {id_float, id_vec2, id_call};
    for (std::size_t i = 0; i < 3; ++i) {
        const FunctionSignature from_obj = cml::pdb::function_signature(*parsed, ids[i], true);
        const FunctionSignature from_pdb = idx->proc_signature(procs[i]);
        REQUIRE_MESSAGE(from_obj.known, from_obj.why);
        REQUIRE_MESSAGE(from_pdb.known, from_pdb.why);
        CHECK(from_obj.text == from_pdb.text);
        CHECK(from_obj.class_unique_name == from_pdb.class_unique_name);
    }
    // Directly by type index too.
    CHECK(cml::pdb::function_signature(*parsed, in_obj.fn_vec2, false).text == kSigVec2);
    // An index outside the section, an id used as a type and a type used as an id are unknown, with CML1431.
    for (const FunctionSignature& bad : {cml::pdb::function_signature(*parsed, 0x7FFFFFF0u, false), cml::pdb::function_signature(*parsed, id_float, false),
                                         cml::pdb::function_signature(*parsed, in_obj.fn_float, true), cml::pdb::function_signature(*parsed, 0x74, false)}) {
        CHECK_FALSE(bad.known);
        CHECK(bad.diag_id == dg::kBadTypeRecord);
        CHECK_FALSE(bad.why.empty());
        CHECK(bad.text.empty());
    }
}

TEST_CASE("types: what distinguishes two signatures") {
    TypeSet t;
    const std::uint32_t args_i = t.add(kLfArgList, lf_arglist({kTInt}));
    const std::uint32_t args_d = t.add(kLfArgList, lf_arglist({kTDouble}));
    const std::uint32_t args_ii = t.add(kLfArgList, lf_arglist({kTInt, kTInt}));
    const std::uint32_t ptr_c = t.add(kLfPointer, lf_pointer(kTChar));
    const std::uint32_t const_c = t.add(kLfModifier, lf_modifier(kTChar, 1));
    const std::uint32_t ptr_cc = t.add(kLfPointer, lf_pointer(const_c));
    const std::uint32_t args_p = t.add(kLfArgList, lf_arglist({ptr_c}));
    const std::uint32_t args_pc = t.add(kLfArgList, lf_arglist({ptr_cc}));
    const std::uint32_t arr4 = t.add(kLfArray, lf_array(kTInt, 16));
    const std::uint32_t arr8 = t.add(kLfArray, lf_array(kTInt, 32));
    const std::uint32_t p_arr4 = t.add(kLfPointer, lf_pointer(arr4));
    const std::uint32_t p_arr8 = t.add(kLfPointer, lf_pointer(arr8));
    const std::uint32_t args_a4 = t.add(kLfArgList, lf_arglist({p_arr4}));
    const std::uint32_t args_a8 = t.add(kLfArgList, lf_arglist({p_arr8}));
    const std::uint32_t en_a = t.add(kLfEnum, lf_enum("Color", ".?AW4Color@@"));
    const std::uint32_t en_b = t.add(kLfEnum, lf_enum("Shade", ".?AW4Shade@@"));
    const std::uint32_t args_ea = t.add(kLfArgList, lf_arglist({en_a}));
    const std::uint32_t args_eb = t.add(kLfArgList, lf_arglist({en_b}));
    // C structs: no unique name, the name decides.
    const std::uint32_t c_a = t.add(kLfStructure, lf_udt("point", ""));
    const std::uint32_t c_b = t.add(kLfStructure, lf_udt("size", ""));
    const std::uint32_t args_ca = t.add(kLfArgList, lf_arglist({c_a}));
    const std::uint32_t args_cb = t.add(kLfArgList, lf_arglist({c_b}));
    // The same class as a forward reference and as its definition: one type.
    const std::uint32_t fwd = t.add(kLfClass, lf_udt("Widget", ".?AVWidget@@", true));
    const std::uint32_t def = t.add(kLfClass, lf_udt("Widget", ".?AVWidget@@", false, 24));
    const std::uint32_t args_fwd = t.add(kLfArgList, lf_arglist({fwd}));
    const std::uint32_t args_def = t.add(kLfArgList, lf_arglist({def}));

    std::vector<std::uint32_t> fns;
    const auto fn = [&](std::uint32_t ret, std::uint32_t args, std::uint16_t count, std::uint8_t call = 0) {
        fns.push_back(t.add(kLfProcedure, lf_procedure(ret, args, count, call)));
        return fns.back();
    };
    const std::uint32_t f_i = fn(kTInt, args_i, 1);
    const std::uint32_t f_d = fn(kTInt, args_d, 1);
    const std::uint32_t f_ii = fn(kTInt, args_ii, 2);
    const std::uint32_t f_ret = fn(kTDouble, args_i, 1);
    const std::uint32_t f_cc = fn(kTInt, args_i, 1, 7);  // another calling convention
    const std::uint32_t f_p = fn(kTInt, args_p, 1);
    const std::uint32_t f_pc = fn(kTInt, args_pc, 1);
    const std::uint32_t f_a4 = fn(kTInt, args_a4, 1);
    const std::uint32_t f_a8 = fn(kTInt, args_a8, 1);
    const std::uint32_t f_ea = fn(kTInt, args_ea, 1);
    const std::uint32_t f_eb = fn(kTInt, args_eb, 1);
    const std::uint32_t f_ca = fn(kTInt, args_ca, 1);
    const std::uint32_t f_cb = fn(kTInt, args_cb, 1);
    const std::uint32_t f_fwd = fn(kTInt, args_fwd, 1);
    const std::uint32_t f_def = fn(kTInt, args_def, 1);
    (void)f_i; (void)f_d; (void)f_ii; (void)f_ret; (void)f_cc; (void)f_p; (void)f_pc; (void)f_a4; (void)f_a8; (void)f_ea; (void)f_eb; (void)f_ca; (void)f_cb;

    const Bytes section = t.debug_t();
    auto parsed = cml::pdb::ObjectTypes::parse(span_of(section));
    REQUIRE(parsed.has_value());
    std::vector<std::string> texts;
    for (const std::uint32_t f : fns) {
        const FunctionSignature s = cml::pdb::function_signature(*parsed, f, false);
        REQUIRE_MESSAGE(s.known, s.why);
        texts.push_back(s.text);
    }
    // Every pair differs, except the forward reference and the definition of one class (the last two).
    for (std::size_t a = 0; a < texts.size(); ++a) {
        for (std::size_t b = a + 1; b < texts.size(); ++b) {
            const bool same_class = fns[a] == f_fwd && fns[b] == f_def;
            CHECK_MESSAGE((texts[a] == texts[b]) == same_class, texts[a] << " vs " << texts[b]);
        }
    }
    CHECK(cml::pdb::canonical_type(*parsed, ptr_cc) == std::optional<std::string>("P1000C(M1(t70))"));
    CHECK(cml::pdb::canonical_type(*parsed, arr4) == std::optional<std::string>("A10(t74)"));
    CHECK(cml::pdb::canonical_type(*parsed, c_a) == std::optional<std::string>("N1505{point}"));
    std::string why;
    CHECK_FALSE(cml::pdb::canonical_type(*parsed, args_i, &why).has_value());  // an argument list is not a type
    CHECK(why.find("1201") != std::string::npos);
}

TEST_CASE("types: a large TPI stream is read by chunk through the type-index offset table") {
    TempDir dir;
    for (const bool fragmented : {false, true}) {
        CAPTURE(fragmented);
        // 6000 filler records, the program in the middle and again at the very end.
        TypeSet types;
        for (int i = 0; i < 3000; ++i) {
            types.add(kLfStructure, lf_udt("Filler" + std::to_string(i), ".?AUFiller" + std::to_string(i) + "@@"));
        }
        const Program mid = add_program(types);
        for (int i = 0; i < 3000; ++i) {
            types.add(kLfPointer, lf_pointer(kTChar));
        }
        const Program last = add_program(types);

        Spec sparse = spec_with({{0x110F, "floor", mid.fn_vec2}, {0x110F, "f::<lambda_1>::operator()", last.fn_call}});
        types.add_to(sparse.tpi);
        sparse.fragmented = fragmented;
        sparse.type_offsets_every = 64;
        Spec dense = sparse;
        dense.type_offsets_every = 0;

        const auto a = open_spec(dir, sparse, fragmented ? "sparse_f.pdb" : "sparse.pdb");
        const auto b = open_spec(dir, dense, fragmented ? "dense_f.pdb" : "dense.pdb");
        const std::vector<SymbolView> pa = procs_of(*a);
        const std::vector<SymbolView> pb = procs_of(*b);
        REQUIRE(pa.size() == 2);
        for (std::size_t i = 0; i < 2; ++i) {
            const FunctionSignature sa = a->proc_signature(pa[i]);
            const FunctionSignature sb = b->proc_signature(pb[i]);
            REQUIRE_MESSAGE(sa.known, sa.why);
            REQUIRE_MESSAGE(sb.known, sb.why);
            CHECK(sa.text == sb.text);
        }
        CHECK(a->proc_signature(pa[0]).text == kSigVec2);
        CHECK(a->proc_signature(pa[1]).text == kSigCall);

        const TypeStreamStats sparse_stats = a->type_stream_stats(true);
        const TypeStreamStats dense_stats = b->type_stream_stats(true);
        CHECK(sparse_stats.sparse);
        CHECK(sparse_stats.chunks > 90);            // about 6000 / 64
        CHECK(sparse_stats.chunks_loaded <= 4);     // the two places the signatures live in
        CHECK(sparse_stats.bytes_loaded < sparse_stats.record_bytes / 10);
        CHECK_FALSE(dense_stats.sparse);
        CHECK(dense_stats.chunks == 1);
        CHECK(dense_stats.bytes_loaded == dense_stats.record_bytes);  // the whole stream was walked
        if (fragmented) {
            CHECK(sparse_stats.bytes_copied <= sparse_stats.bytes_loaded);
            CHECK(dense_stats.bytes_copied == dense_stats.record_bytes);  // a fragmented stream read whole is copied whole
        } else {
            CHECK(sparse_stats.bytes_copied == 0);
        }
        // Indices at the edges: the first, the last, one past the end, one below the first.
        const cml::pdb::TypeSource& src = a->types();
        cml::pdb::TypeRecord r;
        CHECK(src.find_type(0x1000, r));
        CHECK(r.kind == kLfStructure);
        CHECK(src.find_type(types.next() - 1, r));
        CHECK(r.kind == kLfMFunction);
        CHECK_FALSE(src.find_type(types.next(), r));
        CHECK_FALSE(src.find_type(0x0FFF, r));
        CHECK_FALSE(src.find_type(0xFFFFFFFFu, r));
    }
}

TEST_CASE("types: a lying type-index offset table never yields a wrong record") {
    TempDir dir;
    TypeSet types;
    for (int i = 0; i < 400; ++i) {
        types.add(kLfStructure, lf_udt("Filler" + std::to_string(i), ".?AUFiller" + std::to_string(i) + "@@"));
    }
    const Program p = add_program(types);
    Spec good = spec_with({{0x110F, "floor", p.fn_vec2}});
    types.add_to(good.tpi);
    good.type_offsets_every = 32;
    const Bytes table = good.tpi.index_offsets(32);

    const auto signature_with = [&](const Bytes& bad_table, const char* file) {
        Spec s = good;
        s.tpi_offsets_use_override = true;
        s.tpi_offsets_override = bad_table;
        const auto idx = open_spec(dir, s, file);
        const FunctionSignature signature = idx->proc_signature(procs_of(*idx)[0]);
        return std::make_pair(signature, idx->type_stream_stats(true));
    };
    SUBCASE("an intact table") {
        const auto [sig, stats] = signature_with(table, "ok.pdb");
        REQUIRE(sig.known);
        CHECK(sig.text == kSigVec2);
        CHECK(stats.sparse);
    }
    SUBCASE("defects that make the table unusable: the stream is walked whole and the answer stays right") {
        std::vector<Bytes> bad;
        bad.push_back(Bytes(table.begin(), table.end() - 3));  // not a multiple of 8
        {
            Bytes t = table;  // the first entry is not (first index, 0)
            t[4] = 8;
            bad.push_back(t);
        }
        {
            Bytes t = table;  // indices not increasing
            std::memcpy(&t[16], &t[8], 4);
            bad.push_back(t);
        }
        {
            Bytes t = table;  // an offset beyond the record area
            t[8 + 7] = 0x7F;
            bad.push_back(t);
        }
        bad.push_back(Bytes());  // empty
        int n = 0;
        for (const Bytes& t : bad) {
            CAPTURE(n);
            const auto [sig, stats] = signature_with(t, ("bad" + std::to_string(n++) + ".pdb").c_str());
            REQUIRE_MESSAGE(sig.known, sig.why);
            CHECK(sig.text == kSigVec2);
            CHECK_FALSE(stats.sparse);
        }
    }
    SUBCASE("a consistent-looking table that points into the middle of records: unknown, never a wrong signature") {
        Bytes t = table;
        // Shift every offset but the first by 2 bytes: each chunk now starts inside a record.
        for (std::size_t e = 1; e < t.size() / 8; ++e) {
            std::uint32_t off;
            std::memcpy(&off, &t[e * 8 + 4], 4);
            off += 2;
            std::memcpy(&t[e * 8 + 4], &off, 4);
        }
        const auto [sig, stats] = signature_with(t, "shifted.pdb");
        CHECK(stats.sparse);
        if (sig.known) {
            CHECK(sig.text == kSigVec2);  // only a chunk whose records add up can be read
        } else {
            CHECK(sig.diag_id == dg::kBadTypeRecord);
        }
    }
    SUBCASE("a table that promises more records per chunk than there are") {
        Bytes t = table;
        // Entry 1 claims a later first index: chunk 0 holds fewer records than promised.
        std::uint32_t index;
        std::memcpy(&index, &t[8], 4);
        index += 5;
        std::memcpy(&t[8], &index, 4);
        Spec s = good;
        s.tpi_offsets_use_override = true;
        s.tpi_offsets_override = t;
        const auto idx = open_spec(dir, s, "count.pdb");
        cml::pdb::TypeRecord r;
        CHECK_FALSE(idx->types().find_type(0x1000, r));      // chunk 0 is bad as a whole
        CHECK_FALSE(idx->types().find_type(0x1000 + 33, r));  // still chunk 0 by the table: 37 promised, 32 there
        CHECK_FALSE(idx->types().find_type(0x1000 + 40, r));  // chunk 1: 27 promised, 32 there
    }
}

TEST_CASE("types: malformed records are unknown with CML1431, a missing stream is CML1430") {
    TempDir dir;
    SUBCASE("no TPI stream at all") {
        Spec spec = spec_with({{0x110F, "f", 0x1000}});
        const auto idx = open_spec(dir, spec);
        const FunctionSignature s = idx->proc_signature(procs_of(*idx)[0]);
        CHECK_FALSE(s.known);
        CHECK(s.diag_id == dg::kBadTypeStream);
        CHECK(s.why.find("TPI") != std::string::npos);
        cml::pdb::TypeRecord r;
        CHECK_FALSE(idx->types().find_type(0x1000, r));
    }
    SUBCASE("an _ID proc whose IPI record is something else") {
        TypeSet types;
        const Program p = add_program(types);
        Spec spec = spec_with({{0x1146, "f", 0x1000}});
        spec.ipi.string_id("not a function id");
        types.add_to(spec.tpi);
        (void)p;
        const auto idx = open_spec(dir, spec);
        const FunctionSignature s = idx->proc_signature(procs_of(*idx)[0]);
        CHECK_FALSE(s.known);
        CHECK(s.diag_id == dg::kBadTypeRecord);
        CHECK(s.why.find("LF_FUNC_ID") != std::string::npos);
    }
    SUBCASE("record shapes") {
        struct Case {
            const char* what;
            TypeSet types;
            std::uint32_t fn = 0;
        };
        std::vector<Case> cases;
        {
            Case c{"argument list count larger than the record", {}, 0};
            Bytes args = lf_arglist({kTInt});
            args[0] = 200;
            const std::uint32_t a = c.types.add(kLfArgList, args);
            c.fn = c.types.add(kLfProcedure, lf_procedure(kTInt, a, 1));
            cases.push_back(std::move(c));
        }
        {
            Case c{"argument list is another kind of record", {}, 0};
            const std::uint32_t a = c.types.add(kLfPointer, lf_pointer(kTInt));
            c.fn = c.types.add(kLfProcedure, lf_procedure(kTInt, a, 1));
            cases.push_back(std::move(c));
        }
        {
            Case c{"procedure record cut short", {}, 0};
            const std::uint32_t a = c.types.add(kLfArgList, lf_arglist({}));
            Bytes body = lf_procedure(kTInt, a, 0);
            body.resize(6);
            c.fn = c.types.add(kLfProcedure, body);
            cases.push_back(std::move(c));
        }
        {
            Case c{"member function record cut short", {}, 0};
            const std::uint32_t a = c.types.add(kLfArgList, lf_arglist({}));
            Bytes body = lf_mfunction(kTInt, 0x1000, 0x1000, a, 0);
            body.resize(18);
            c.fn = c.types.add(kLfMFunction, body);
            cases.push_back(std::move(c));
        }
        {
            Case c{"a pointer that points at itself", {}, 0};
            const std::uint32_t self = c.types.next();
            c.types.add(kLfPointer, lf_pointer(self));
            const std::uint32_t a = c.types.add(kLfArgList, lf_arglist({self}));
            c.fn = c.types.add(kLfProcedure, lf_procedure(kTInt, a, 1));
            cases.push_back(std::move(c));
        }
        {
            Case c{"a function type that returns itself", {}, 0};
            const std::uint32_t a = c.types.add(kLfArgList, lf_arglist({}));
            const std::uint32_t self = c.types.next();
            c.fn = c.types.add(kLfProcedure, lf_procedure(self, a, 0));
            cases.push_back(std::move(c));
        }
        {
            Case c{"an argument list that fans out (every argument is the function itself)", {}, 0};
            const std::uint32_t fn_index = c.types.next() + 1;
            const std::uint32_t a = c.types.add(kLfArgList, lf_arglist(std::vector<std::uint32_t>(64, fn_index)));
            c.fn = c.types.add(kLfProcedure, lf_procedure(kTInt, a, 64));
            cases.push_back(std::move(c));
        }
        {
            Case c{"a class record without a terminated name", {}, 0};
            Bytes udt = lf_udt("Broken", ".?AUBroken@@");
            udt.resize(udt.size() - 1);  // the unique name loses its terminator
            while (udt.size() % 4 != 0) {
                udt.push_back('x');
            }
            const std::uint32_t u = c.types.add(kLfStructure, udt);
            const std::uint32_t a = c.types.add(kLfArgList, lf_arglist({u}));
            c.fn = c.types.add(kLfProcedure, lf_procedure(kTInt, a, 1));
            cases.push_back(std::move(c));
        }
        {
            Case c{"a record kind the reader does not know", {}, 0};
            const std::uint32_t odd = c.types.add(0x1609, Bytes(24, 0));  // LF_STRUCTURE2
            const std::uint32_t a = c.types.add(kLfArgList, lf_arglist({odd}));
            c.fn = c.types.add(kLfProcedure, lf_procedure(kTInt, a, 1));
            cases.push_back(std::move(c));
        }
        {
            Case c{"an array with a size leaf the reader cannot read", {}, 0};
            Bytes arr = lf_array(kTInt, 16);
            arr[8] = 0x05;
            arr[9] = 0x80;  // LF_REAL32: not an integer
            const std::uint32_t r = c.types.add(kLfArray, arr);
            const std::uint32_t pr = c.types.add(kLfPointer, lf_pointer(r));
            const std::uint32_t a = c.types.add(kLfArgList, lf_arglist({pr}));
            c.fn = c.types.add(kLfProcedure, lf_procedure(kTInt, a, 1));
            cases.push_back(std::move(c));
        }
        {
            Case c{"the function type is not a function", {}, 0};
            c.fn = c.types.add(kLfPointer, lf_pointer(kTInt));
            cases.push_back(std::move(c));
        }
        for (Case& c : cases) {
            CAPTURE(c.what);
            // In a PDB ...
            Spec spec = spec_with({{0x110F, "f", c.fn}});
            c.types.add_to(spec.tpi);
            const auto idx = open_spec(dir, spec);
            const FunctionSignature from_pdb = idx->proc_signature(procs_of(*idx)[0]);
            CHECK_FALSE(from_pdb.known);
            CHECK(from_pdb.diag_id == dg::kBadTypeRecord);
            CHECK_FALSE(from_pdb.why.empty());
            // ... and in an object.
            const Bytes section = c.types.debug_t();
            auto parsed = cml::pdb::ObjectTypes::parse(span_of(section));
            REQUIRE(parsed.has_value());
            const FunctionSignature from_obj = cml::pdb::function_signature(*parsed, c.fn, false);
            CHECK_FALSE(from_obj.known);
            CHECK(from_obj.why == from_pdb.why);
        }
    }
}

TEST_CASE("types: the head of a .debug$T section (own records, type server, precompiled types)") {
    TypeSet types;
    const Program p = add_program(types);
    SUBCASE("not a type section") {
        for (const Bytes& bad : {Bytes(), Bytes{4, 0, 0}, Bytes{2, 0, 0, 0, 1, 2, 3, 4}}) {
            auto info = cml::pdb::inspect_debug_t(span_of(bad));
            REQUIRE_FALSE(info.has_value());
            CHECK(info.error().code == dg::kNoObjectTypes);
            CHECK(info.error().id() == "CML1432");
            CHECK_FALSE(cml::pdb::ObjectTypes::parse(span_of(bad)).has_value());
        }
    }
    SUBCASE("only the signature") {
        const Bytes empty = {4, 0, 0, 0};
        auto info = cml::pdb::inspect_debug_t(span_of(empty));
        REQUIRE(info.has_value());
        CHECK(info->kind == cml::pdb::DebugTInfo::Kind::Empty);
    }
    SUBCASE("a type-server reference (/Zi)") {
        const std::array<std::uint8_t, 16> guid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        TypeSet server;
        server.add(kLfTypeServer2, lf_typeserver2(guid, 7, "C:\\proj\\obj\\vc143.pdb"));
        const Bytes section = server.debug_t();
        auto info = cml::pdb::inspect_debug_t(span_of(section));
        REQUIRE(info.has_value());
        CHECK(info->kind == cml::pdb::DebugTInfo::Kind::TypeServer);
        CHECK(info->server.id.guid == guid);
        CHECK(info->server.id.age == 7);
        CHECK(info->server.pdb_path == "C:\\proj\\obj\\vc143.pdb");
        auto parsed = cml::pdb::ObjectTypes::parse(span_of(section));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == dg::kNoObjectTypes);
        CHECK(parsed.error().message.find("vc143.pdb") != std::string::npos);
    }
    SUBCASE("precompiled types (/Z7 /Yu): the object's own records are readable, the PCH's are not") {
        // The PCH object holds 0x200 types; this object's records start at 0x1200.
        TypeSet own;
        own.set_first(0x1200);
        const std::uint32_t pch_struct = 0x1050;  // lives in the PCH object
        const std::uint32_t args_own = own.add(kLfArgList, lf_arglist({kTInt}));
        const std::uint32_t fn_own = own.add(kLfProcedure, lf_procedure(kTInt, args_own, 1));
        const std::uint32_t args_pch = own.add(kLfArgList, lf_arglist({pch_struct}));
        const std::uint32_t fn_pch = own.add(kLfProcedure, lf_procedure(kTInt, args_pch, 1));
        const std::pair<std::uint16_t, Bytes> head(kLfPrecomp, lf_precomp(0x1000, 0x200, "C:\\proj\\obj\\pch.obj"));
        const Bytes section = own.debug_t(&head);
        auto info = cml::pdb::inspect_debug_t(span_of(section));
        REQUIRE(info.has_value());
        CHECK(info->kind == cml::pdb::DebugTInfo::Kind::Precompiled);
        CHECK(info->first_own_index == 0x1200);
        CHECK(info->precompiled_object == "C:\\proj\\obj\\pch.obj");
        auto parsed = cml::pdb::ObjectTypes::parse(span_of(section));
        REQUIRE(parsed.has_value());
        CHECK(parsed->first_index() == 0x1200);
        CHECK(parsed->record_count() == 4);
        const FunctionSignature own_sig = cml::pdb::function_signature(*parsed, fn_own, false);
        REQUIRE_MESSAGE(own_sig.known, own_sig.why);
        CHECK(own_sig.text == "F(cc=0,fa=0,r=t74,a=[t74])");
        const FunctionSignature pch_sig = cml::pdb::function_signature(*parsed, fn_pch, false);
        CHECK_FALSE(pch_sig.known);  // needs a type of the PCH object: unknown, never guessed
        CHECK(pch_sig.why.find("1050") != std::string::npos);
        cml::pdb::TypeRecord r;
        CHECK_FALSE(parsed->find_type(0x1000, r));
    }
    SUBCASE("a record that leaves the section") {
        Bytes section = types.debug_t();
        section.resize(section.size() - 5);
        auto parsed = cml::pdb::ObjectTypes::parse(span_of(section));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == dg::kBadTypeRecord);
        Bytes head_only = {4, 0, 0, 0, 0xFF, 0x7F, 0x08, 0x10};
        CHECK_FALSE(cml::pdb::inspect_debug_t(span_of(head_only)).has_value());
    }
    (void)p;
}

TEST_CASE("types: the procedure records of an object's .debug$S") {
    std::vector<std::uint32_t> fields;
    const Bytes section = debug_s({{0x1146, "floor", 0x1005}, {0x1147, "Tree::walk", 0x1010}, {0x110F, "old_style", 0x1020}}, &fields);
    auto procs = cml::pdb::read_object_procs(span_of(section));
    REQUIRE_MESSAGE(procs.has_value(), (procs.has_value() ? std::string() : procs.error().format()));
    REQUIRE(procs->size() == 3);
    CHECK((*procs)[0].name == "floor");
    CHECK((*procs)[0].type_index == 0x1005);
    CHECK((*procs)[0].type_is_id);
    CHECK_FALSE((*procs)[0].global);
    CHECK((*procs)[0].offset_field == fields[0]);
    CHECK((*procs)[1].name == "Tree::walk");
    CHECK((*procs)[1].global);
    CHECK((*procs)[1].type_is_id);
    CHECK((*procs)[1].offset_field == fields[1]);
    CHECK((*procs)[2].type_index == 0x1020);
    CHECK_FALSE((*procs)[2].type_is_id);
    CHECK((*procs)[2].offset_field == fields[2]);

    SUBCASE("empty, and no symbols") {
        CHECK(cml::pdb::read_object_procs(span_of(Bytes())).value().empty());
        const Bytes only_signature = {4, 0, 0, 0};
        CHECK(cml::pdb::read_object_procs(span_of(only_signature)).value().empty());
    }
    SUBCASE("an ignored subsection is skipped") {
        Bytes ignored = section;
        // The symbols subsection is the second one: set the "ignore" bit of its kind.
        const std::size_t kind_at = 4 + 8 + 8;
        REQUIRE(ignored[kind_at] == 0xF1);
        ignored[kind_at + 3] = 0x80;
        auto r = cml::pdb::read_object_procs(span_of(ignored));
        REQUIRE(r.has_value());
        CHECK(r->empty());
    }
    SUBCASE("malformed: CML1433") {
        Bytes bad_signature = section;
        bad_signature[0] = 2;
        Bytes long_subsection = section;
        long_subsection[4 + 4] = 0xFF;  // the first subsection claims 0xFF.. bytes
        long_subsection[4 + 5] = 0xFF;
        long_subsection[4 + 6] = 0xFF;
        Bytes bad_record = section;
        bad_record[4 + 8 + 8 + 8] = 0xFF;  // the first symbol record's length
        bad_record[4 + 8 + 8 + 9] = 0xFF;
        for (const Bytes& b : {bad_signature, long_subsection, bad_record}) {
            auto r = cml::pdb::read_object_procs(span_of(b));
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error().code == dg::kBadObjectSymbols);
            CHECK(r.error().id() == "CML1433");
        }
    }
}

TEST_CASE("types: a compiler PDB opened as a type server") {
    TempDir dir;
    TypeSet types;
    const Program p = add_program(types);
    Spec spec = spec_with({});
    types.add_to(spec.tpi);
    const std::uint32_t id_vec2 = spec.ipi.record(kLfFuncId, lf_func_id(p.fn_vec2, "floor"));
    spec.age = 3;
    spec.info_age_override = 9;  // LF_TYPESERVER2 records the info-stream age
    const std::string path = dir.file("vc143.pdb");
    write_file(path, build(spec));

    auto server = cml::pdb::TypeServerPdb::open(path);
    REQUIRE_MESSAGE(server.has_value(), (server.has_value() ? std::string() : server.error().format()));
    CHECK((*server)->id().guid == spec.guid);
    CHECK((*server)->id().age == 9);
    CHECK((*server)->path() == path);
    const FunctionSignature s = cml::pdb::function_signature(**server, id_vec2, true);
    REQUIRE_MESSAGE(s.known, s.why);
    CHECK(s.text == kSigVec2);
    CHECK(cml::pdb::function_signature(**server, p.fn_call, false).class_unique_name == kClosureUnique);

    SUBCASE("a missing file, a file that is no PDB, a PDB without a TPI stream") {
        auto missing = cml::pdb::TypeServerPdb::open(dir.file("absent.pdb"));
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == dg::kCannotOpenFile);
        write_file(dir.file("junk.pdb"), Bytes(5000, 0x41));
        CHECK_FALSE(cml::pdb::TypeServerPdb::open(dir.file("junk.pdb")).has_value());
        write_file(dir.file("no_tpi.pdb"), build(spec_with({})));
        auto no_tpi = cml::pdb::TypeServerPdb::open(dir.file("no_tpi.pdb"));
        REQUIRE_FALSE(no_tpi.has_value());
        CHECK(no_tpi.error().code == dg::kBadTypeStream);
        CHECK(no_tpi.error().id() == "CML1430");
    }
}

TEST_CASE("types: every corruption of a type stream is survived (flip and truncate)") {
    TempDir dir;
    TypeSet types;
    const Program p = add_program(types);
    Spec spec = spec_with({{0x110F, "floor", p.fn_float}, {0x110F, "floor", p.fn_vec2}, {0x110F, "f::<lambda_1>::operator()", p.fn_call}});
    types.add_to(spec.tpi);
    const std::uint32_t id = spec.ipi.record(kLfMFuncId, lf_mfunc_id(p.closure, p.fn_call, "operator()"));
    spec.modules[0].symbols.proc(0x1146, "g", 1, 0x400, 0x10, 0, id);
    spec.modules[0].symbols.end();
    spec.type_offsets_every = 4;
    const Bytes good = build(spec);

    // Where the TPI stream lies in the file (its blocks are consecutive in this layout).
    const Bytes tpi_stream = spec.tpi.stream(7, static_cast<std::uint32_t>(spec.tpi.index_offsets(4).size()));
    const auto begin = std::search(good.begin(), good.end(), tpi_stream.begin(), tpi_stream.end());
    REQUIRE(begin != good.end());
    const std::size_t tpi_at = static_cast<std::size_t>(begin - good.begin());

    const auto exercise = [&](const Bytes& image, const char* file) {
        const std::string path = dir.file(file);
        write_file(path, image);
        auto r = PdbIndex::open_pdb(path, no_cache());
        if (!r) {
            return;
        }
        auto syms = (*r)->compiland_symbols(0);
        if (!syms) {
            return;
        }
        for (const SymbolView& v : *syms) {
            const FunctionSignature s = (*r)->proc_signature(v);
            if (s.known) {
                CHECK_FALSE(s.text.empty());
            } else {
                CHECK(s.diag_id >= 1400);
                CHECK_FALSE(s.why.empty());
            }
        }
        cml::pdb::TypeRecord rec;
        for (std::uint32_t i = 0x0FF0; i < 0x1040; ++i) {
            (void)(*r)->types().find_type(i, rec);
            (void)(*r)->types().find_id(i, rec);
        }
    };
    exercise(good, "good.pdb");
    // Every byte of the TPI stream set to four values.
    for (std::size_t i = 0; i < tpi_stream.size(); ++i) {
        for (const std::uint8_t v : {std::uint8_t{0x00}, std::uint8_t{0xFF}, std::uint8_t{0x80}, static_cast<std::uint8_t>(good[tpi_at + i] ^ 0x01)}) {
            Bytes image = good;
            image[tpi_at + i] = v;
            exercise(image, "flip.pdb");
        }
    }
}

TEST_CASE("types: the translation-unit discriminator is not part of a signature") {
    using cml::pdb::without_tu_discriminator;
    // cl: a backtick and the unit's hash after the name of a type local to a function (observed in CI, cl 19.44).
    CHECK(without_tu_discriminator(".?AV<lambda_1>@?1??ls_step@@YAHXZ@`a8f45668") == ".?AV<lambda_1>@?1??ls_step@@YAHXZ@");
    CHECK(without_tu_discriminator(".?AV<lambda_1>@?1??ls_step@@YAHXZ@`9BFD0EF1") == ".?AV<lambda_1>@?1??ls_step@@YAHXZ@");
    // In the middle of a name (a type nested in such a type).
    CHECK(without_tu_discriminator(".?AUInner@<lambda_1>@?1??f@@YAXXZ@`0123abcd@@") == ".?AUInner@<lambda_1>@?1??f@@YAXXZ@@@");
    // The anonymous namespace of the unit.
    CHECK(without_tu_discriminator(".?AUPoint@?A0xa8f45668@@") == ".?AUPoint@?A0x@@");
    CHECK(without_tu_discriminator(".?AV?$vector@UPoint@?A0x9bfd0ef1@@@std@@") == ".?AV?$vector@UPoint@?A0x@@@std@@");
    // Everything else is kept: other lengths after a backtick, a name without either, an empty name.
    for (const char* same : {".?AUVec2@@", ".?AV<lambda_1>@?1??f@@YAXXZ@", ".?AUx@`1234567", ".?AUx@`123456789", ".?AUx@`1234567g", "?A0x", "?A0x12", "`", ""}) {
        CAPTURE(same);
        CHECK(without_tu_discriminator(same) == same);
    }

    // The same closure and the same anonymous-namespace struct, recorded under two unit hashes (the old PDB and an
    // object compiled from a copy of the source at another path): one signature. Another lambda: another signature.
    const auto program = [](const char* hash, int lambda) {
        TypeSet t;
        const std::string closure_unique = ".?AV<lambda_" + std::to_string(lambda) + ">@?1??f@@YAXXZ@`" + hash;
        const std::uint32_t closure = t.add(kLfClass, lf_udt("f::<lambda_1>", closure_unique, true));
        const std::uint32_t const_closure = t.add(kLfModifier, lf_modifier(closure, 1));
        const std::uint32_t this_ptr = t.add(kLfPointer, lf_pointer(const_closure, kPtr64 | 0x400));
        const std::uint32_t point = t.add(kLfStructure, lf_udt("`anonymous namespace'::Point", std::string(".?AUPoint@?A0x") + hash + "@@", true));
        const std::uint32_t args = t.add(kLfArgList, lf_arglist({point}));
        const std::uint32_t fn = t.add(kLfMFunction, lf_mfunction(kTInt, closure, this_ptr, args, 1));
        return std::make_pair(t.debug_t(), fn);
    };
    const auto signature_of = [](const std::pair<Bytes, std::uint32_t>& p) {
        auto parsed = cml::pdb::ObjectTypes::parse(span_of(p.first));
        REQUIRE(parsed.has_value());
        return cml::pdb::function_signature(*parsed, p.second, false);
    };
    const FunctionSignature a = signature_of(program("9bfd0ef1", 1));
    const FunctionSignature b = signature_of(program("a8f45668", 1));
    const FunctionSignature other = signature_of(program("a8f45668", 2));
    REQUIRE(a.known);
    REQUIRE(b.known);
    CHECK(a.text == b.text);
    CHECK(a.text.find('`') == std::string::npos);
    CHECK(a.text.find("9bfd0ef1") == std::string::npos);
    CHECK(a.text != other.text);
    // The recorded name is kept as it is.
    CHECK(a.class_unique_name == ".?AV<lambda_1>@?1??f@@YAXXZ@`9bfd0ef1");
    CHECK(b.class_unique_name == ".?AV<lambda_1>@?1??f@@YAXXZ@`a8f45668");
}

// ---- function-local statics and their place in the procedure (ADR-0042, amended) --------------------------------------

namespace {

// f() { static a;  { static names; { static deep; } }  { static names; }  <inlined call: static inl>  static tail; }
// and g() { static names; } after it. `seg` / `off`: an address for a PDB, zero for an object.
void write_two_procs(Records& r, std::uint16_t seg, std::uint32_t code, std::uint32_t data) {
    const std::uint16_t dseg = static_cast<std::uint16_t>(seg == 0 ? 0 : 2);
    r.proc(0x1110, "f", seg, code, 0x10);
    r.data(0x110C, "a", dseg, data + 0x00);
    r.block();
    r.data(0x110C, "names", dseg, data + 0x08, kTDouble);
    r.block();
    r.data(0x110C, "deep", dseg, data + 0x10);
    r.end();
    r.end();
    r.block();
    r.data(0x1112, "names", dseg, data + 0x18);  // S_LTHREAD32
    r.end();
    r.add(0x114D, Bytes(12, 0));  // S_INLINESITE
    r.data(0x110C, "inl", dseg, data + 0x20);
    r.add(0x114E, Bytes());       // S_INLINESITE_END
    r.data(0x110C, "tail", dseg, data + 0x28);
    r.end();
    r.proc(0x110F, "g", seg, code + 0x10, 0x10);
    r.data(0x110C, "names", dseg, data + 0x30);
    r.end();
}

void check_statics_of_f(const std::vector<cml::pdb::ProcStatic>& s) {
    REQUIRE(s.size() == 6);
    const char* const names[] = {"a", "names", "deep", "names", "inl", "tail"};
    const std::vector<std::uint32_t> paths[] = {{}, {0}, {0, 0}, {1}, {}, {}};
    for (std::size_t i = 0; i < 6; ++i) {
        CAPTURE(i);
        CHECK(s[i].name == names[i]);
        CHECK(s[i].block_path == paths[i]);
        CHECK(s[i].plain_scopes == (i != 4));  // `inl` is inside an inline site
        CHECK(s[i].thread_storage == (i == 3));
    }
    CHECK(s[1].type_index == kTDouble);
    CHECK(s[0].type_index == kTInt);
}

}  // namespace

TEST_CASE("statics: the data records of a procedure and the blocks they sit in, read alike from an object and from a PDB") {
    SUBCASE("an object's .debug$S") {
        Records r;
        r.data(0x110C, "file_scope", 0, 0);  // not in a procedure: nobody's static
        write_two_procs(r, 0, 0, 0);
        std::uint32_t base = 0;
        const Bytes section = debug_s_section(r.bytes(), &base);
        auto procs = cml::pdb::read_object_procs(span_of(section));
        REQUIRE(procs.has_value());
        REQUIRE(procs->size() == 2);
        CHECK((*procs)[0].name == "f");
        check_statics_of_f((*procs)[0].statics);
        CHECK((*procs)[0].statics_complete);
        REQUIRE((*procs)[1].statics.size() == 1);
        CHECK((*procs)[1].statics[0].name == "names");
        CHECK((*procs)[1].statics[0].block_path.empty());
        // The offset field of a data record is where the object has the SECREL relocation that names the variable:
        // record header (4), type index (4). The first record of f() follows the file-scope record and f's own.
        const std::uint32_t first = (*procs)[0].statics[0].offset_field;
        CHECK(first > base);
        CHECK(first < (*procs)[0].statics[1].offset_field);
        CHECK((*procs)[0].statics[0].rva == cml::pdb::kNoRva);
    }
    SUBCASE("the module stream of a PDB: the symbols of the compiland whose parent is the procedure, in the same order") {
        Spec s;
        s.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}, {".data", 0x400, 0x3000, 0x400, 0xC0000040}};
        Module m;
        m.name = "C:\\proj\\obj\\logic.cpp.obj";
        m.symbols.objname(m.name);
        write_two_procs(m.symbols, 1, 0x100, 0x40);
        s.modules.push_back(std::move(m));
        TempDir dir;
        const auto idx = open_spec(dir, s);
        const std::vector<SymbolView> procs = procs_of(*idx);
        REQUIRE(procs.size() == 2);
        bool complete = false;
        auto statics = idx->proc_statics(procs[0], &complete);
        REQUIRE_MESSAGE(statics.has_value(), (statics.has_value() ? std::string() : statics.error().format()));
        CHECK(complete);
        check_statics_of_f(*statics);
        // The same records as compiland_symbols() gives, by address.
        auto all = idx->compiland_symbols(0);
        REQUIRE(all.has_value());
        std::vector<std::uint32_t> rvas;
        for (const SymbolView& v : *all) {
            if (v.parent_proc == procs[0].index && cml::pdb::is_data_kind(v.kind)) {
                rvas.push_back(v.rva);
            }
        }
        REQUIRE(rvas.size() == statics->size());
        for (std::size_t i = 0; i < rvas.size(); ++i) {
            CHECK((*statics)[i].rva == rvas[i]);
        }
        CHECK((*statics)[0].rva == 0x3040);
        // g(): only its own.
        auto of_g = idx->proc_statics(procs[1]);
        REQUIRE(of_g.has_value());
        REQUIRE(of_g->size() == 1);
        CHECK((*of_g)[0].rva == 0x3070);
        // Not a procedure of this index.
        SymbolView not_a_proc = (*all)[0];
        not_a_proc.kind = cml::pdb::SymbolKind::LocalData;
        CHECK_FALSE(idx->proc_statics(not_a_proc).has_value());
    }
}

TEST_CASE("statics: nesting the reader does not follow, and too many statics, are reported, never guessed") {
    SUBCASE("65 nested blocks: the place of the record is unknown") {
        Records r;
        r.proc(0x1110, "f", 0, 0, 0x10);
        for (int i = 0; i < 65; ++i) {
            r.block();
        }
        r.data(0x110C, "bottom", 0, 0);
        for (int i = 0; i < 65; ++i) {
            r.end();
        }
        r.data(0x110C, "after", 0, 0);
        r.end();
        r.proc(0x1110, "g", 0, 0, 0x10);
        r.data(0x110C, "mine", 0, 0);
        r.end();
        auto procs = cml::pdb::read_object_procs(span_of(debug_s_section(r.bytes())));
        REQUIRE(procs.has_value());
        REQUIRE(procs->size() == 2);
        REQUIRE((*procs)[0].statics.size() == 2);
        CHECK((*procs)[0].statics[0].name == "bottom");
        CHECK_FALSE((*procs)[0].statics[0].plain_scopes);
        CHECK((*procs)[0].statics[1].name == "after");  // every scope was closed again: back in the procedure
        CHECK((*procs)[0].statics[1].plain_scopes);
        CHECK((*procs)[0].statics[1].block_path.empty());
        REQUIRE((*procs)[1].statics.size() == 1);
        CHECK((*procs)[1].statics[0].name == "mine");
    }
    SUBCASE("more statics than kMaxProcStatics") {
        Records r;
        r.proc(0x1110, "f", 0, 0, 0x10);
        for (std::size_t i = 0; i < cml::pdb::kMaxProcStatics + 3; ++i) {
            r.data(0x110C, "n", 0, 0);
        }
        r.end();
        auto procs = cml::pdb::read_object_procs(span_of(debug_s_section(r.bytes())));
        REQUIRE(procs.has_value());
        REQUIRE(procs->size() == 1);
        CHECK((*procs)[0].statics.size() == cml::pdb::kMaxProcStatics);
        CHECK_FALSE((*procs)[0].statics_complete);
    }
    SUBCASE("unbalanced records: more ends than scopes, a procedure record too short to read") {
        Records r;
        r.end();
        r.end();
        r.add(0x1110, Bytes(8, 0));  // a procedure record without its fields
        r.data(0x110C, "orphan", 0, 0);
        r.end();
        r.proc(0x1110, "f", 0, 0, 0x10);
        r.data(0x110C, "mine", 0, 0);
        auto procs = cml::pdb::read_object_procs(span_of(debug_s_section(r.bytes())));
        REQUIRE(procs.has_value());
        REQUIRE(procs->size() == 1);
        REQUIRE((*procs)[0].statics.size() == 1);
        CHECK((*procs)[0].statics[0].name == "mine");
    }
}

// ---- WP-46: the closure classes of the TPI (kept for WP-51, R-54) ------------------------------------------------------

TEST_CASE("types: closure_classes lists every lambda class of the TPI once, with its unique name without the unit's hash") {
    TempDir dir;
    SUBCASE("forward references and definitions are one class; other classes are left out; the list is kept") {
        TypeSet types;
        types.add(kLfClass, lf_udt("o2_step::<lambda_0>", ".?AV<lambda_0>@?0??o2_step@@YAXXZ@", true));
        types.add(kLfStructure, lf_udt("Vec2", ".?AUVec2@@"));
        types.add(kLfClass, lf_udt("o2_step::<lambda_0>", ".?AV<lambda_0>@?0??o2_step@@YAXXZ@", false, 1));
        types.add(kLfClass, lf_udt("`f'::`2'::<lambda_1>", ".?AV<lambda_1>@?1??f@@YAXXZ@`a8f45668", false, 1));  // cl's hash suffix
        types.add(kLfStructure, lf_udt("<lambda_0b7c44cbca6a0337bec371af34c9af94>", ".?AU<lambda_0b7c44cbca6a0337bec371af34c9af94>@@"));
        types.add(kLfClass, lf_udt("NoUnique", ""));
        Spec spec = spec_with({{0x110F, "f", 0x1000}});
        types.add_to(spec.tpi);
        const auto idx = open_spec(dir, spec);
        const auto list = idx->closure_classes();
        REQUIRE_MESSAGE(list.has_value(), (list.has_value() ? std::string() : list.error().format()));
        REQUIRE((*list)->size() == 3);
        CHECK((**list)[0].name == "o2_step::<lambda_0>");
        CHECK((**list)[0].unique == ".?AV<lambda_0>@?0??o2_step@@YAXXZ@");
        CHECK((**list)[0].definition == 0x1002);  // the definition, not the forward reference 0x1000
        CHECK((**list)[1].unique == ".?AV<lambda_1>@?1??f@@YAXXZ@");
        CHECK((**list)[2].unique == ".?AU<lambda_0b7c44cbca6a0337bec371af34c9af94>@@");
        const auto again = idx->closure_classes();
        REQUIRE(again.has_value());
        CHECK(*again == *list);  // built once
    }
    SUBCASE("a stream of many chunks: walked once, its chunks not kept; a lying offset table is an error, not a shorter list") {
        TypeSet types;
        for (int i = 0; i < 40; ++i) {
            types.add(kLfPointer, lf_pointer(kTChar));
            if (i % 10 == 9) {
                const std::string n = std::to_string(i);
                types.add(kLfClass, lf_udt("g" + n + "::<lambda_0>", ".?AV<lambda_0>@?0??g" + n + "@@YAXXZ@", false, 1));
            }
        }
        Spec spec = spec_with({{0x110F, "f", 0x1000}});
        spec.type_offsets_every = 4;
        types.add_to(spec.tpi);
        {
            const auto idx = open_spec(dir, spec, "chunks.pdb");
            const auto list = idx->closure_classes();
            REQUIRE(list.has_value());
            CHECK((*list)->size() == 4);
            CHECK((**list)[3].unique == ".?AV<lambda_0>@?0??g39@@YAXXZ@");
            const TypeStreamStats st = idx->type_stream_stats(true);
            CHECK(st.chunks > 4);
            CHECK(st.chunks_loaded == 0);
            CHECK(st.bytes_loaded == 0);  // (review: the walk read them for itself only)
            CHECK(st.bytes_copied == 0);
        }
        // The table claims a record boundary inside a record: that chunk is bad, so the walk is incomplete.
        Bytes lying = spec.tpi.index_offsets(4);
        REQUIRE(lying.size() >= 24);
        lying[12] = static_cast<std::uint8_t>(lying[12] + 2);  // the second entry's offset, moved into the middle of a record
        spec.tpi_offsets_override = lying;
        spec.tpi_offsets_use_override = true;
        const auto idx = open_spec(dir, spec, "lying.pdb");
        const auto list = idx->closure_classes();
        REQUIRE_FALSE(list.has_value());
        CHECK(list.error().code == dg::kBadTypeRecord);
    }
    SUBCASE("no TPI stream: an error (CML1430), never an empty list") {
        Spec spec = spec_with({{0x110F, "f", 0x1000}});
        const auto idx = open_spec(dir, spec);
        const auto list = idx->closure_classes();
        REQUIRE_FALSE(list.has_value());
        CHECK(list.error().code == dg::kBadTypeStream);
    }
}

// ---- WP-46 review: sizes, class layouts and the types of locals -------------------------------------------------------

TEST_CASE("types: type_size knows simple types, pointers, arrays and classes, a forward reference through its definition") {
    TempDir dir;
    TypeSet types;
    const std::uint32_t fwd = types.add(kLfClass, lf_udt("Rtti", ".?AVRtti@@", true));
    const std::uint32_t ptr = types.add(kLfPointer, lf_pointer(kTInt));
    const std::uint32_t arr = types.add(kLfArray, lf_array(kTInt, 16));
    const std::uint32_t cnst = types.add(kLfModifier, lf_modifier(fwd, 1));
    types.add(kLfClass, lf_udt("Rtti", ".?AVRtti@@", false, 24));
    const std::uint32_t lonely = types.add(kLfClass, lf_udt("Lonely", ".?AVLonely@@", true));
    Spec spec = spec_with({{0x110F, "f", 0x1000}});
    types.add_to(spec.tpi);
    const auto idx = open_spec(dir, spec);
    CHECK(idx->type_size(kTInt) == std::optional<std::uint64_t>(4));
    CHECK(idx->type_size(kTDouble) == std::optional<std::uint64_t>(8));
    CHECK(idx->type_size(ptr) == std::optional<std::uint64_t>(8));
    CHECK(idx->type_size(arr) == std::optional<std::uint64_t>(16));
    CHECK(idx->type_size(fwd) == std::optional<std::uint64_t>(24));
    CHECK(idx->type_size(cnst) == std::optional<std::uint64_t>(24));
    CHECK_FALSE(idx->type_size(lonely).has_value());  // no definition: unknown, never 0
}

TEST_CASE("types: udt_layout renders a definition's size and data members; a forward reference is unreadable") {
    TempDir dir;
    TypeSet types;
    Bytes fields;
    put16(fields, 0x150D);  // LF_MEMBER int at 0
    put16(fields, 3);
    put32(fields, kTInt);
    put16(fields, 0);
    putstr(fields, "a");
    while (fields.size() % 4 != 0) {
        fields.push_back(static_cast<std::uint8_t>(0xF0 + (4 - fields.size() % 4)));
    }
    put16(fields, 0x150D);  // LF_MEMBER double at 8
    put16(fields, 3);
    put32(fields, kTDouble);
    put16(fields, 8);
    putstr(fields, "b");
    while (fields.size() % 4 != 0) {
        fields.push_back(static_cast<std::uint8_t>(0xF0 + (4 - fields.size() % 4)));
    }
    const std::uint32_t list = types.add(0x1203, fields);
    Bytes def;
    put16(def, 2);
    put16(def, 0x200);
    put32(def, list);
    put32(def, 0);
    put32(def, 0);
    put16(def, 16);
    putstr(def, "<lambda_0>");
    putstr(def, ".?AV<lambda_0>@?0??f@@YAXXZ@");
    const std::uint32_t defined = types.add(kLfClass, def);
    const std::uint32_t fwd = types.add(kLfClass, lf_udt("<lambda_0>", ".?AV<lambda_0>@?0??f@@YAXXZ@", true));
    Spec spec = spec_with({{0x110F, "f", 0x1000}});
    types.add_to(spec.tpi);
    const auto idx = open_spec(dir, spec);
    const std::optional<std::string> layout = cml::pdb::udt_layout(idx->types(), defined);
    REQUIRE(layout.has_value());
    CHECK(*layout == "size=16;0:t74;8:t41");
    std::string why;
    CHECK_FALSE(cml::pdb::udt_layout(idx->types(), fwd, &why).has_value());
    CHECK(why.find("forward reference") != std::string::npos);
    CHECK(cml::pdb::class_unique_name_of(idx->types(), fwd) == ".?AV<lambda_0>@?0??f@@YAXXZ@");
}

TEST_CASE("types: compiland_local_types lists each procedure's own locals, not an inlined call's") {
    TempDir dir;
    Spec s;
    s.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}};
    Module m;
    m.name = "C:\\proj\\obj\\logic.cpp.obj";
    m.symbols.objname(m.name);
    const auto local = [&](std::uint32_t type, const std::string& name) {
        Bytes b;
        put32(b, type);
        put16(b, 0);
        putstr(b, name);
        m.symbols.add(0x113E, b);
    };
    m.symbols.proc(0x1110, "f", 1, 0x100, 0x20, 0, 0);
    local(0x1234, "lambda");
    m.symbols.block();
    local(0x1235, "inner");
    m.symbols.end();
    m.symbols.add(0x114D, Bytes(12, 0));  // S_INLINESITE
    local(0x9999, "inlinee_local");
    m.symbols.add(0x114E, Bytes());       // S_INLINESITE_END
    m.symbols.end();
    m.symbols.proc(0x1110, "g", 1, 0x200, 0x20, 0, 0);
    Bytes regrel;
    put32(regrel, 0x20);
    put32(regrel, 0x1236);
    put16(regrel, 335);
    putstr(regrel, "x");
    m.symbols.add(0x1111, regrel);  // S_REGREL32 (cl at /Od)
    m.symbols.end();
    s.modules.push_back(std::move(m));
    const auto idx = open_spec(dir, s);
    const auto uses = idx->compiland_local_types(0);
    REQUIRE_MESSAGE(uses.has_value(), (uses.has_value() ? std::string() : uses.error().format()));
    REQUIRE(uses->size() == 3);
    CHECK((*uses)[0].proc_rva == 0x1100);
    CHECK((*uses)[0].type_index == 0x1234);
    CHECK((*uses)[1].type_index == 0x1235);
    CHECK((*uses)[2].proc_rva == 0x1200);
    CHECK((*uses)[2].type_index == 0x1236);
}

TEST_CASE("types: type_size keys definitions by the exact unique name; definitions of one name that disagree are unknown") {
    TempDir dir;
    TypeSet types;
    // Two units' `namespace { struct State; }`: the same name, different discriminators, different sizes.
    types.add(kLfStructure, lf_udt("`anonymous namespace'::State", ".?AUState@?A0x11111111@@", false, 8));
    types.add(kLfStructure, lf_udt("`anonymous namespace'::State", ".?AUState@?A0x22222222@@", false, 32));
    const std::uint32_t small = types.add(kLfStructure, lf_udt("`anonymous namespace'::State", ".?AUState@?A0x11111111@@", true));
    const std::uint32_t large = types.add(kLfStructure, lf_udt("`anonymous namespace'::State", ".?AUState@?A0x22222222@@", true));
    // One exact name defined twice with different sizes (a PDB that merged two definitions).
    types.add(kLfStructure, lf_udt("Odd", ".?AUOdd@@", false, 4));
    types.add(kLfStructure, lf_udt("Odd", ".?AUOdd@@", false, 12));
    const std::uint32_t odd = types.add(kLfStructure, lf_udt("Odd", ".?AUOdd@@", true));
    Spec spec = spec_with({{0x110F, "f", 0x1000}});
    types.add_to(spec.tpi);
    const auto idx = open_spec(dir, spec);
    CHECK(idx->type_size(small) == std::optional<std::uint64_t>(8));
    CHECK(idx->type_size(large) == std::optional<std::uint64_t>(32));  // 4092e8b: 8, the first definition without the discriminator
    CHECK_FALSE(idx->type_size(odd).has_value());
}

// ---- WP-51 (R-54): the inline sites of a procedure (PdbIndex::proc_inlinees) -------------------------------------------

namespace {

Bytes inline_site(std::uint32_t inlinee, bool second_form = false) {
    Bytes b;
    put32(b, 0);  // parent
    put32(b, 0);  // end
    put32(b, inlinee);
    if (second_form) {
        put32(b, 3);  // S_INLINESITE2: invocations
    }
    b.push_back(0x0B);  // an annotation (code offset): read past
    b.push_back(0x04);
    return b;
}

// What clang 18 -O2 writes for o2_step (planner_pairing_o2, docs/wp/WP-51.md): the statics first, then an inline site of the
// closure's call operator with a nested site of a free function; a second procedure inlined nothing.
struct InlineWorld {
    TypeSet types;
    std::uint32_t closure = 0, call = 0, proc_void = 0, rtti = 0, rtti_ctor = 0;
    std::uint32_t id_call = 0, id_make = 0, id_ctor = 0, id_string = 0;
    Spec spec;
};

InlineWorld inline_world() {
    InlineWorld w;
    const std::uint32_t args = w.types.add(kLfArgList, lf_arglist({}));
    w.proc_void = w.types.add(kLfProcedure, lf_procedure(kTVoid, args, 0));
    w.closure = w.types.add(kLfClass, lf_udt("o2_step::<lambda_0>", ".?AV<lambda_0>@?0??o2_step@@YAXXZ@", true));
    const std::uint32_t this_ptr = w.types.add(kLfPointer, lf_pointer(w.closure, kPtr64 | 0x400));
    w.call = w.types.add(kLfMFunction, lf_mfunction(kTInt, w.closure, this_ptr, args, 0));
    w.rtti = w.types.add(kLfClass, lf_udt("JPH::RTTI", ".?AVRTTI@JPH@@", true));
    const std::uint32_t rtti_this = w.types.add(kLfPointer, lf_pointer(w.rtti, kPtr64 | 0x400));
    w.rtti_ctor = w.types.add(kLfMFunction, lf_mfunction(kTVoid, w.rtti, rtti_this, args, 0));
    w.id_call = w.spec.ipi.record(kLfMFuncId, lf_mfunc_id(w.closure, w.call, "operator()"));
    w.id_make = w.spec.ipi.record(kLfFuncId, lf_func_id(w.proc_void, "o2_make"));
    w.id_ctor = w.spec.ipi.record(kLfMFuncId, lf_mfunc_id(w.rtti, w.rtti_ctor, "RTTI"));
    w.id_string = w.spec.ipi.string_id("not a function");
    w.spec.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}, {".data", 0x400, 0x3000, 0x400, 0xC0000040}};
    return w;
}

Module inline_module() {
    Module m;
    m.name = "C:\\proj\\obj\\lambdas.cpp.obj";
    m.symbols.objname(m.name);
    return m;
}

}  // namespace

TEST_CASE("inlinees: every inline site of a procedure, at any depth, resolved through the IPI and the TPI") {
    TempDir dir;
    InlineWorld w = inline_world();
    Module m = inline_module();
    m.symbols.proc(0x1110, "o2_step", 1, 0x100, 0x80, 0, w.proc_void);
    m.symbols.data(0x110C, "sname", 2, 0x10);
    m.symbols.data(0x110C, "sname", 2, 0x20);
    m.symbols.add(0x114D, inline_site(w.id_call));
    m.symbols.add(0x114D, inline_site(w.id_make));        // nested: o2_make inlined into the call operator
    m.symbols.add(0x114E, Bytes());
    m.symbols.add(0x114E, Bytes());
    m.symbols.add(0x115D, inline_site(w.id_call, true));  // the second lambda's site, S_INLINESITE2 form
    m.symbols.add(0x114E, Bytes());
    m.symbols.block();
    m.symbols.add(0x114D, inline_site(w.id_ctor));        // inside a block, still the procedure's
    m.symbols.add(0x114E, Bytes());
    m.symbols.end();
    m.symbols.end();
    m.symbols.proc(0x1110, "plain", 1, 0x200, 0x20, 0, w.proc_void);
    m.symbols.data(0x110C, "own", 2, 0x30);
    m.symbols.end();
    w.spec.modules.push_back(std::move(m));
    w.types.add_to(w.spec.tpi);
    const auto idx = open_spec(dir, w.spec);
    const std::vector<SymbolView> procs = procs_of(*idx);
    REQUIRE(procs.size() == 2);

    const auto got = idx->proc_inlinees(procs[0]);
    REQUIRE_MESSAGE(got.has_value(), (got.has_value() ? std::string() : got.error().format()));
    CHECK(got->sites == 4);
    CHECK(got->complete);
    REQUIRE(got->inlinees.size() == 3);
    const PdbIndex::Inlinee& call = got->inlinees[0];
    CHECK(call.id == w.id_call);
    CHECK(call.readable);
    CHECK(call.member);
    CHECK(call.name == "operator()");
    CHECK(call.class_name == "o2_step::<lambda_0>");
    CHECK(call.class_unique == ".?AV<lambda_0>@?0??o2_step@@YAXXZ@");
    CHECK(call.sites == 2);
    CHECK(call.depth == 1);
    CHECK(call.signature_known);
    CHECK(call.signature.find("<lambda_0>") != std::string::npos);
    const PdbIndex::Inlinee& make = got->inlinees[1];
    CHECK(make.readable);
    CHECK_FALSE(make.member);
    CHECK(make.name == "o2_make");
    CHECK(make.class_unique.empty());
    CHECK(make.depth == 2);
    CHECK(make.sites == 1);
    const PdbIndex::Inlinee& ctor = got->inlinees[2];
    CHECK(ctor.name == "RTTI");
    CHECK(ctor.class_unique == ".?AVRTTI@JPH@@");
    CHECK(ctor.depth == 1);

    const auto none = idx->proc_inlinees(procs[1]);
    REQUIRE(none.has_value());
    CHECK(none->sites == 0);
    CHECK(none->inlinees.empty());
}

TEST_CASE("inlinees: an inlinee that cannot be read is listed as unreadable, never left out") {
    TempDir dir;
    InlineWorld w = inline_world();
    const std::uint32_t lost_class = w.spec.ipi.record(kLfMFuncId, lf_mfunc_id(0x7FFF, w.call, "operator()"));
    Module m = inline_module();
    m.symbols.proc(0x1110, "f", 1, 0x100, 0x80, 0, w.proc_void);
    m.symbols.add(0x114D, inline_site(0x1FFF));       // not in the IPI
    m.symbols.add(0x114E, Bytes());
    m.symbols.add(0x114D, inline_site(w.id_string));  // an LF_STRING_ID, not a function id
    m.symbols.add(0x114E, Bytes());
    m.symbols.add(0x114D, inline_site(lost_class));   // its class is not in the TPI
    m.symbols.add(0x114E, Bytes());
    m.symbols.add(0x114D, Bytes(8, 0));               // too short to name an inlinee
    m.symbols.add(0x114E, Bytes());
    m.symbols.end();
    w.spec.modules.push_back(std::move(m));
    w.types.add_to(w.spec.tpi);
    const auto idx = open_spec(dir, w.spec);
    const auto got = idx->proc_inlinees(procs_of(*idx)[0]);
    REQUIRE(got.has_value());
    CHECK(got->sites == 4);
    REQUIRE(got->inlinees.size() == 4);
    for (const PdbIndex::Inlinee& e : got->inlinees) {
        CAPTURE(e.why);
        CHECK_FALSE(e.readable);
        CHECK_FALSE(e.why.empty());
    }
    CHECK(got->inlinees[0].why.find("does not hold") != std::string::npos);
    CHECK(got->inlinees[1].why.find("neither LF_FUNC_ID nor LF_MFUNC_ID") != std::string::npos);
    CHECK(got->inlinees[2].why.find("cannot be read from the TPI") != std::string::npos);
    CHECK(got->inlinees[3].why.find("too short") != std::string::npos);
}

TEST_CASE("inlinees (WP-60): a function's parent scope is read from its LF_STRING_ID; one that cannot be read leaves it readable") {
    TempDir dir;
    InlineWorld w = inline_world();
    const std::uint32_t anon = w.spec.ipi.string_id("`anonymous namespace'");
    const std::uint32_t in_anon = w.spec.ipi.record(kLfFuncId, lf_func_id(w.proc_void, "hs", anon));
    const std::uint32_t odd_scope = w.spec.ipi.record(kLfFuncId, lf_func_id(w.proc_void, "odd", w.id_make));  // a function id, not a string
    const std::uint32_t lost_scope = w.spec.ipi.record(kLfFuncId, lf_func_id(w.proc_void, "lost", 0x1FFF));   // not in the IPI
    // (Review of 8609b97) A string with a substring list is the tail of a longer one: its scope is not known.
    Bytes tail;
    put32(tail, anon);  // a (bogus) substring list
    for (const char ch : std::string("Inner")) {
        tail.push_back(static_cast<std::uint8_t>(ch));
    }
    tail.push_back(0);
    const std::uint32_t long_scope = w.spec.ipi.record(0x1605, tail);
    const std::uint32_t in_long = w.spec.ipi.record(kLfFuncId, lf_func_id(w.proc_void, "deep", long_scope));
    Module m = inline_module();
    m.symbols.proc(0x1110, "f", 1, 0x100, 0x80, 0, w.proc_void);
    for (const std::uint32_t id : {w.id_make, in_anon, odd_scope, lost_scope, w.id_ctor, in_long}) {
        m.symbols.add(0x114D, inline_site(id));
        m.symbols.add(0x114E, Bytes());
    }
    m.symbols.end();
    w.spec.modules.push_back(std::move(m));
    w.types.add_to(w.spec.tpi);
    const auto idx = open_spec(dir, w.spec);
    const auto got = idx->proc_inlinees(procs_of(*idx)[0]);
    REQUIRE(got.has_value());
    REQUIRE(got->inlinees.size() == 6);
    CHECK_FALSE(got->inlinees[5].scope_known);
    for (const PdbIndex::Inlinee& e : got->inlinees) {
        CAPTURE(e.name);
        CHECK(e.readable);
        CHECK(e.signature_known);
    }
    CHECK(got->inlinees[0].scope_known);  // no scope
    CHECK(got->inlinees[0].scope.empty());
    CHECK(got->inlinees[1].scope_known);
    CHECK(got->inlinees[1].scope == "`anonymous namespace'");
    CHECK_FALSE(got->inlinees[2].scope_known);
    CHECK_FALSE(got->inlinees[3].scope_known);
    CHECK(got->inlinees[4].scope_known);  // a member: its class names it, no scope is read
    CHECK(got->inlinees[4].scope.empty());
}

TEST_CASE("inlinee lines (WP-60): the source file checksum of an inlinee, from C13 DEBUG_S_INLINEELINES and DEBUG_S_FILECHKSMS") {
    const auto subsection = [](Bytes& out, std::uint32_t kind, const Bytes& body) {
        put32(out, kind);
        put32(out, static_cast<std::uint32_t>(body.size()));
        out.insert(out.end(), body.begin(), body.end());
        while (out.size() % 4 != 0) {
            out.push_back(0);
        }
    };
    // Checksums: file 0 (MD5, 16 bytes of 0xA1) at offset 0, file 1 (no checksum) at 24, file 2 (MD5 of 0xB2) at 32.
    Bytes sums;
    const auto entry = [&](std::uint8_t kind, std::uint8_t size, std::uint8_t fill) {
        put32(sums, 0);
        sums.push_back(size);
        sums.push_back(kind);
        for (std::uint8_t i = 0; i < size; ++i) {
            sums.push_back(fill);
        }
        while (sums.size() % 4 != 0) {
            sums.push_back(0);
        }
    };
    entry(1, 16, 0xA1);
    entry(0, 0, 0);
    entry(1, 16, 0xB2);
    REQUIRE(sums.size() == 56);
    Bytes lines;  // signature 0: (inlinee, file id, line)
    put32(lines, 0);
    for (const std::uint32_t v : {0x1001u, 0u, 3u, 0x1002u, 24u, 7u, 0x1003u, 0u, 1u, 0x1003u, 32u, 2u}) {
        put32(lines, v);
    }
    Bytes ex;  // signature 1: (inlinee, file id, line, extra count, extra file ids)
    put32(ex, 1);
    for (const std::uint32_t v : {0x1004u, 32u, 9u, 1u, 0u, 0x1005u, 0u, 4u, 0u, 0x1006u, 0u, 5u, 1u, 24u}) {
        put32(ex, v);
    }
    Bytes all;
    subsection(all, 0xF1, Bytes(6, 0));  // symbols: skipped
    subsection(all, 0xF6, lines);
    subsection(all, 0xF4, sums);
    subsection(all, 0xF6, ex);
    cml::pdb::C13Inlinees c13;
    REQUIRE(cml::pdb::read_c13_inlinees(std::span<const std::uint8_t>(all.data(), all.size()), c13).has_value());
    CHECK(c13.checksum_tables == 1);
    CHECK(c13.lines.size() == 7);
    const auto f1 = cml::pdb::inlinee_files(c13, 0x1001);
    REQUIRE(f1.has_value());
    REQUIRE(f1->size() == 1);
    CHECK((*f1)[0].kind == 1);
    CHECK((*f1)[0].bytes == std::vector<std::uint8_t>(16, 0xA1));
    CHECK_FALSE(cml::pdb::inlinee_files(c13, 0x1002).has_value());  // recorded without a checksum
    CHECK_FALSE(cml::pdb::inlinee_files(c13, 0x1003).has_value());  // two entries naming different files
    const auto f4 = cml::pdb::inlinee_files(c13, 0x1004);           // the extra-files form: its file, then the extra one
    REQUIRE(f4.has_value());
    REQUIRE(f4->size() == 2);
    CHECK((*f4)[0].bytes == std::vector<std::uint8_t>(16, 0xB2));
    CHECK((*f4)[1].bytes == std::vector<std::uint8_t>(16, 0xA1));
    CHECK((cml::pdb::inlinee_files(c13, 0x1005) == cml::pdb::inlinee_files(c13, 0x1001)));  // no extra files
    CHECK_FALSE(cml::pdb::inlinee_files(c13, 0x1006).has_value());  // an extra file without a checksum
    CHECK_FALSE(cml::pdb::inlinee_files(c13, 0x1FFF).has_value());  // no entry

    cml::pdb::C13Inlinees twice = c13;  // a second checksum table: the file ids are no longer unambiguous
    Bytes more;
    subsection(more, 0xF4, sums);
    REQUIRE(cml::pdb::read_c13_inlinees(std::span<const std::uint8_t>(more.data(), more.size()), twice).has_value());
    CHECK(twice.checksum_tables == 2);
    CHECK_FALSE(cml::pdb::inlinee_files(twice, 0x1001).has_value());

    Bytes cut;
    put32(cut, 0xF6);
    put32(cut, 400);  // longer than the span
    put32(cut, 0);
    cml::pdb::C13Inlinees bad;
    CHECK_FALSE(cml::pdb::read_c13_inlinees(std::span<const std::uint8_t>(cut.data(), cut.size()), bad).has_value());

    // (Review of 5624b28) A 10-byte block: one header with len 2, rounded up past the end; nothing is read past the span.
    Bytes ten;
    put32(ten, 0xF6);
    put32(ten, 2);
    ten.push_back(0);
    ten.push_back(0);
    cml::pdb::C13Inlinees small;
    CHECK(cml::pdb::read_c13_inlinees(std::span<const std::uint8_t>(ten.data(), ten.size()), small).has_value());
    CHECK(small.lines.empty());

    // Extra files that leave their subsection.
    Bytes long_ex;
    put32(long_ex, 1);
    for (const std::uint32_t v : {0x1007u, 0u, 1u, 1000u}) {
        put32(long_ex, v);
    }
    Bytes overrun;
    subsection(overrun, 0xF6, long_ex);
    cml::pdb::C13Inlinees over;
    CHECK_FALSE(cml::pdb::read_c13_inlinees(std::span<const std::uint8_t>(overrun.data(), overrun.size()), over).has_value());
}

TEST_CASE("inlinees: a procedure that is not in its module, or a module without an end record, is an error") {
    TempDir dir;
    InlineWorld w = inline_world();
    Module m = inline_module();
    m.symbols.proc(0x1110, "f", 1, 0x100, 0x80, 0, w.proc_void);
    m.symbols.add(0x114D, inline_site(w.id_call));
    m.symbols.add(0x114E, Bytes());
    // no S_END
    w.spec.modules.push_back(std::move(m));
    w.types.add_to(w.spec.tpi);
    const auto idx = open_spec(dir, w.spec);
    const std::vector<SymbolView> procs = procs_of(*idx);
    REQUIRE(procs.size() == 1);
    const auto open_ended = idx->proc_inlinees(procs[0]);
    REQUIRE_FALSE(open_ended.has_value());
    CHECK(open_ended.error().code == dg::kBadModuleSymbols);
    SymbolView elsewhere = procs[0];
    elsewhere.offset = 0x900;  // no procedure record there
    const auto missing = idx->proc_inlinees(elsewhere);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == dg::kBadModuleSymbols);
    SymbolView data = procs[0];
    data.kind = cml::pdb::SymbolKind::LocalData;
    const auto not_proc = idx->proc_inlinees(data);
    REQUIRE_FALSE(not_proc.has_value());
    CHECK(not_proc.error().code == dg::kCompilandOutOfRange);
}

TEST_CASE("inlinees: a procedure nested in another owns its inline sites; several procedures are read in one walk") {
    TempDir dir;
    InlineWorld w = inline_world();
    Module m = inline_module();
    m.symbols.proc(0x1110, "outer", 1, 0x100, 0x80, 0, w.proc_void);
    m.symbols.add(0x114D, inline_site(w.id_make));
    m.symbols.add(0x114E, Bytes());
    m.symbols.proc(0x110F, "nested", 1, 0x140, 0x20, 0, w.proc_void);  // review of 2a3580d: was counted as outer's
    m.symbols.add(0x114D, inline_site(w.id_call));
    m.symbols.add(0x114D, inline_site(w.id_ctor));
    m.symbols.add(0x114E, Bytes());
    m.symbols.add(0x114E, Bytes());
    m.symbols.end();
    m.symbols.add(0x115D, inline_site(w.id_make, true));  // outer's again, after the nested procedure
    m.symbols.add(0x114E, Bytes());
    m.symbols.end();
    m.symbols.proc(0x1110, "later", 1, 0x200, 0x20, 0, w.proc_void);
    m.symbols.add(0x114D, inline_site(w.id_call));
    m.symbols.add(0x114E, Bytes());
    m.symbols.end();
    w.spec.modules.push_back(std::move(m));
    w.types.add_to(w.spec.tpi);
    const auto idx = open_spec(dir, w.spec);
    const std::vector<SymbolView> procs = procs_of(*idx);
    REQUIRE(procs.size() == 3);

    const auto outer = idx->proc_inlinees(procs[0]);
    REQUIRE(outer.has_value());
    CHECK(outer->sites == 2);
    REQUIRE(outer->inlinees.size() == 1);
    CHECK(outer->inlinees[0].name == "o2_make");
    CHECK(outer->inlinees[0].sites == 2);
    CHECK(outer->inlinees[0].depth == 1);

    const auto nested = idx->proc_inlinees(procs[1]);
    REQUIRE(nested.has_value());
    CHECK(nested->sites == 2);
    REQUIRE(nested->inlinees.size() == 2);
    CHECK(nested->inlinees[0].name == "operator()");
    CHECK(nested->inlinees[0].depth == 1);
    CHECK(nested->inlinees[1].name == "RTTI");
    CHECK(nested->inlinees[1].depth == 2);

    // One walk for all of them (in any order, one asked twice), with the same answers.
    const auto all = idx->procs_inlinees({procs[2], procs[0], procs[1], procs[2]});
    REQUIRE_MESSAGE(all.has_value(), (all.has_value() ? std::string() : all.error().format()));
    REQUIRE(all->size() == 4);
    CHECK((*all)[0].sites == 1);
    REQUIRE((*all)[0].inlinees.size() == 1);
    CHECK((*all)[0].inlinees[0].class_unique == ".?AV<lambda_0>@?0??o2_step@@YAXXZ@");
    CHECK((*all)[1].sites == 2);
    CHECK((*all)[1].inlinees.size() == 1);
    CHECK((*all)[2].sites == 2);
    CHECK((*all)[2].inlinees.size() == 2);
    CHECK((*all)[3].sites == 1);
    CHECK((*all)[3].inlinees.size() == 1);

    // Procedures of two compilands, or one that is not in its module, fail the call.
    SymbolView other = procs[0];
    other.compiland = 5;
    CHECK_FALSE(idx->procs_inlinees({procs[0], other}).has_value());
    SymbolView elsewhere = procs[0];
    elsewhere.offset = 0x900;
    const auto missing = idx->procs_inlinees({procs[1], elsewhere});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == dg::kBadModuleSymbols);
    const auto none = idx->procs_inlinees({});
    REQUIRE(none.has_value());
    CHECK(none->empty());
}
