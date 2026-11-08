// PdbIndex::proc_info (WP-23) on synthetic PDBs: the parameter types through TPI/IPI and the frame/variable records of a function,
// written the way cl (S_REGREL32) and clang-cl (S_LOCAL + S_DEFRANGE_*) write them. Portable. The real compilers' records are
// read by test_fixture_pdb.cpp.
#include <cstdint>
#include <memory>
#include <string>

#include "cml/pdb/pdb.h"
#include <ostream>  // doctest streams string_view operands; MSVC needs the full definition

#include "../pdb/test_util.h"
#include "doctest.h"

using namespace cml;
using namespace cml::testpdb;

namespace {

constexpr std::uint16_t S_FRAMEPROC = 0x1012;
constexpr std::uint16_t S_REGREL32 = 0x1111;
constexpr std::uint16_t S_LOCAL = 0x113E;

// Basic types.
constexpr std::uint32_t T_INT4 = 0x74;
constexpr std::uint32_t T_REAL32 = 0x40;
constexpr std::uint32_t T_REAL64 = 0x41;
constexpr std::uint32_t T_64PINT4 = 0x674;
constexpr std::uint32_t T_VOID = 0x03;
constexpr std::uint32_t T_BOOL08 = 0x30;
constexpr std::uint32_t T_UQUAD = 0x23;

struct Types {
    Ipi tpi;
    Ipi ipi;

    std::uint32_t arglist(const std::vector<std::uint32_t>& args) {
        Bytes b;
        put32(b, static_cast<std::uint32_t>(args.size()));
        for (std::uint32_t a : args) {
            put32(b, a);
        }
        return tpi.record(0x1201, b);
    }
    std::uint32_t procedure(std::uint32_t ret, std::uint32_t args_id, std::uint16_t count) {
        Bytes b;
        put32(b, ret);
        b.push_back(0);
        b.push_back(0);
        put16(b, count);
        put32(b, args_id);
        return tpi.record(0x1008, b);
    }
    std::uint32_t mfunction(std::uint32_t ret, std::uint32_t klass, std::uint32_t this_type, std::uint32_t args_id, std::uint16_t count) {
        Bytes b;
        put32(b, ret);
        put32(b, klass);
        put32(b, this_type);
        b.push_back(0);
        b.push_back(0);
        put16(b, count);
        put32(b, args_id);
        put32(b, 0);
        return tpi.record(0x1009, b);
    }
    std::uint32_t pointer(std::uint32_t to, std::uint32_t attr = 0x0C | (8u << 13)) {
        Bytes b;
        put32(b, to);
        put32(b, attr);
        return tpi.record(0x1002, b);
    }
    std::uint32_t modifier(std::uint32_t of) {
        Bytes b;
        put32(b, of);
        put16(b, 1);
        return tpi.record(0x1001, b);
    }
    std::uint32_t enumeration(std::uint32_t underlying) {
        Bytes b;
        put16(b, 0);
        put16(b, 0);
        put32(b, underlying);
        put32(b, 0);
        putstr(b, "Color");
        return tpi.record(0x1507, b);
    }
    std::uint32_t klass(std::uint16_t kind = 0x1504) {
        Bytes b;
        put16(b, 0);
        put16(b, 0);
        put32(b, 0);
        put32(b, 0);
        put32(b, 0);
        put16(b, 16);
        putstr(b, "Widget");
        return tpi.record(kind, b);
    }
    std::uint32_t func_id(std::uint32_t function_type, const std::string& name, std::uint16_t kind = 0x1601) {
        Bytes b;
        put32(b, 0);
        put32(b, function_type);
        putstr(b, name);
        return ipi.record(kind, b);
    }
};

void add_proc(Records& r, std::uint16_t kind, const std::string& name, std::uint32_t off, std::uint32_t len, std::uint32_t type) {
    Bytes b;
    put32(b, 0);
    put32(b, 0);
    put32(b, 0);
    put32(b, len);
    put32(b, 0);
    put32(b, len);
    put32(b, type);
    put32(b, off);
    put16(b, 1);
    b.push_back(0);
    putstr(b, name);
    r.add(kind, b);
}

void add_frameproc(Records& r, std::uint32_t cb_frame, std::uint32_t flags) {
    Bytes b;
    put32(b, cb_frame);
    put32(b, 0);
    put32(b, 0);
    put32(b, 0);
    put32(b, 0);
    put16(b, 0);
    put32(b, flags);
    r.add(S_FRAMEPROC, b);
}

void add_regrel(Records& r, const std::string& name, std::int32_t off, std::uint32_t type, std::uint16_t reg) {
    Bytes b;
    put32(b, static_cast<std::uint32_t>(off));
    put32(b, type);
    put16(b, reg);
    putstr(b, name);
    r.add(S_REGREL32, b);
}

void add_local(Records& r, const std::string& name, std::uint32_t type, std::uint16_t flags) {
    Bytes b;
    put32(b, type);
    put16(b, flags);
    putstr(b, name);
    r.add(S_LOCAL, b);
}

void add_fp_full(Records& r, std::int32_t off) {
    Bytes b;
    put32(b, static_cast<std::uint32_t>(off));
    r.add(0x1144, b);
}

void add_fp_range(Records& r, std::int32_t off, std::uint32_t start, std::uint16_t len) {
    Bytes b;
    put32(b, static_cast<std::uint32_t>(off));
    put32(b, start);
    put16(b, 1);
    put16(b, len);
    r.add(0x1142, b);
}

void add_reg_rel_range(Records& r, std::uint16_t reg, std::int32_t off, std::uint32_t start, std::uint16_t len) {
    Bytes b;
    put16(b, reg);
    put16(b, 0);
    put32(b, static_cast<std::uint32_t>(off));
    put32(b, start);
    put16(b, 1);
    put16(b, len);
    r.add(0x1145, b);
}

void add_reg_range(Records& r, std::uint16_t reg, std::uint32_t start, std::uint16_t len) {
    Bytes b;
    put16(b, reg);
    put16(b, 0);
    put32(b, start);
    put16(b, 1);
    put16(b, len);
    r.add(0x1141, b);
}

// A PDB with one compiland holding the procs `fill` writes.
template <class Fill>
std::shared_ptr<const pdb::PdbIndex> make_index(TempDir& dir, Types& types, Fill fill) {
    Spec spec;
    spec.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}};
    Module m;
    m.name = "C:\\proj\\obj\\step.cpp.obj";
    m.symbols.compile3(1, 0, 0xD0, "Test Compiler 19.44");
    m.symbols.envblock({{"cwd", "C:\\proj"}, {"cmd", "/c /Zi /Od step.cpp"}, {"src", "C:\\proj\\step.cpp"}});
    fill(m.symbols);
    m.files = {"C:\\proj\\step.cpp"};
    spec.modules.push_back(std::move(m));
    spec.contributions = {{1, 0x0, 0x800, 0}};
    spec.tpi = types.tpi;
    spec.ipi = types.ipi;
    const std::string path = dir.file("t.pdb");
    write_file(path, build(spec));
    auto opened = pdb::PdbIndex::open_pdb(path);
    if (!opened) {
        FAIL(opened.error().format());
    }
    return *opened;
}

pdb::ProcInfo info_of(const pdb::PdbIndex& index, const char* name) {
    auto syms = index.compiland_symbols(0);
    REQUIRE(syms.has_value());
    for (const pdb::SymbolView& s : *syms) {
        if (pdb::is_proc_kind(s.kind) && s.name == name) {
            auto info = index.proc_info(s);
            REQUIRE_MESSAGE(info.has_value(), info.error().format());
            return *info;
        }
    }
    FAIL("no proc " << name);
    return {};
}

}  // namespace

TEST_CASE("proc_info: step(int*, int, double) as MSVC /Od writes it (S_GPROC32_ID, S_FRAMEPROC, S_REGREL32)") {
    TempDir dir;
    Types t;
    const std::uint32_t args = t.arglist({T_64PINT4, T_INT4, T_REAL64});
    const std::uint32_t proc_type = t.procedure(T_INT4, args, 3);
    const std::uint32_t id = t.func_id(proc_type, "step");
    const auto index = make_index(dir, t, [&](Records& r) {
        add_proc(r, 0x1147, "step", 0x100, 0x60, id);
        add_frameproc(r, 0x28, (1u << 14) | (1u << 16));  // RSP-based locals and parameters
        add_regrel(r, "p", 0x30, T_64PINT4, 335);
        add_regrel(r, "k", 0x38, T_INT4, 335);
        add_regrel(r, "scale", 0x40, T_REAL64, 335);
        add_regrel(r, "v", 0x20, T_INT4, 335);
        r.add(0x114F, Bytes());
    });
    const pdb::ProcInfo info = info_of(*index, "step");
    CHECK(info.rva == 0x1100);
    CHECK(info.size == 0x60);
    CHECK(info.has_frameproc);
    CHECK(info.frame_bytes == 0x28);
    CHECK(info.param_base_register == pdb::kCvRegRsp);
    CHECK(info.local_base_register == pdb::kCvRegRsp);
    CHECK_FALSE(info.opt_for_speed);
    REQUIRE_MESSAGE(info.params_known, info.params_error);
    REQUIRE(info.params.size() == 3);
    CHECK(info.params[0].cls == pdb::ParamClass::Int);
    CHECK(info.params[0].size == 8);
    CHECK(info.params[1].cls == pdb::ParamClass::Int);
    CHECK(info.params[1].size == 4);
    CHECK(info.params[2].cls == pdb::ParamClass::Float);
    CHECK(info.params[2].size == 8);
    REQUIRE(info.variables.size() == 4);
    CHECK(info.variables[0].name == "p");
    CHECK(info.variables[0].from_regrel);
    CHECK_FALSE(info.variables[0].is_param);
    REQUIRE(info.variables[0].locations.size() == 1);
    CHECK(info.variables[0].locations[0].kind == pdb::VarLocationKind::RegisterRelative);
    CHECK(info.variables[0].locations[0].reg == 335);
    CHECK(info.variables[0].locations[0].offset == 0x30);
    CHECK(info.variables[0].locations[0].covers(0x1100));
    CHECK(info.variables[2].locations[0].offset == 0x40);
    CHECK(info.variables[2].type_index == T_REAL64);
}

TEST_CASE("proc_info: the same function as clang-cl writes it (S_LOCAL parameters with S_DEFRANGE_* records)") {
    TempDir dir;
    Types t;
    const std::uint32_t args = t.arglist({T_64PINT4, T_INT4, T_REAL64});
    const std::uint32_t proc_type = t.procedure(T_INT4, args, 3);
    const std::uint32_t id = t.func_id(proc_type, "step");
    const auto index = make_index(dir, t, [&](Records& r) {
        add_proc(r, 0x1147, "step", 0x100, 0x60, id);
        add_frameproc(r, 0x40, (2u << 14) | (2u << 16) | (1u << 20));  // RBP-based, "optimised for speed" bit set for the test
        add_local(r, "p", T_64PINT4, 1);
        add_fp_full(r, -8);
        add_local(r, "k", T_INT4, 1);
        add_fp_range(r, -12, 0x1110, 0x40);
        add_local(r, "scale", T_REAL64, 1);
        add_reg_rel_range(r, 335, 0x20, 0x1110, 0x40);
        add_local(r, "v", T_INT4, 0);
        add_reg_range(r, 18, 0x1120, 0x10);
        add_local(r, "gone", T_INT4, 0x100);
        r.add(0x114F, Bytes());
    });
    const pdb::ProcInfo info = info_of(*index, "step");
    CHECK(info.param_base_register == pdb::kCvRegRbp);
    CHECK(info.opt_for_speed);
    REQUIRE(info.variables.size() == 5);
    const pdb::ProcVariable& p = info.variables[0];
    CHECK(p.is_param);
    CHECK_FALSE(p.from_regrel);
    REQUIRE(p.locations.size() == 1);
    CHECK(p.locations[0].kind == pdb::VarLocationKind::FramePointerRelative);
    CHECK(p.locations[0].offset == -8);
    CHECK(p.locations[0].covers(0xFFFFF));  // whole function
    const pdb::ProcVariable& k = info.variables[1];
    REQUIRE(k.locations.size() == 1);
    CHECK(k.locations[0].offset == -12);
    CHECK(k.locations[0].start_rva == 0x2110);  // section offset 0x1110 + section RVA 0x1000
    CHECK(k.locations[0].length == 0x40);
    CHECK_FALSE(k.locations[0].covers(0x2110 - 1));
    CHECK(k.locations[0].covers(0x2110));
    CHECK(k.locations[0].covers(0x2110 + 0x3F));
    CHECK_FALSE(k.locations[0].covers(0x2110 + 0x40));
    const pdb::ProcVariable& scale = info.variables[2];
    REQUIRE(scale.locations.size() == 1);
    CHECK(scale.locations[0].kind == pdb::VarLocationKind::RegisterRelative);
    CHECK(scale.locations[0].reg == 335);
    CHECK(scale.locations[0].offset == 0x20);
    const pdb::ProcVariable& v = info.variables[3];
    CHECK_FALSE(v.is_param);
    REQUIRE(v.locations.size() == 1);
    CHECK(v.locations[0].kind == pdb::VarLocationKind::Register);
    CHECK(v.locations[0].reg == 18);
    CHECK(info.variables[4].optimized_out);
    CHECK(info.variables[4].locations.empty());
    CHECK(info.params_known);
}

TEST_CASE("proc_info: a member function has `this` first; a static one does not") {
    TempDir dir;
    Types t;
    const std::uint32_t widget = t.klass();
    const std::uint32_t this_ptr = t.pointer(widget);
    const std::uint32_t args = t.arglist({T_INT4});
    const std::uint32_t method = t.mfunction(T_VOID, widget, this_ptr, args, 1);
    const std::uint32_t static_method = t.mfunction(T_VOID, widget, 0, args, 1);
    const std::uint32_t method_id = t.func_id(method, "set", 0x1602);
    const std::uint32_t static_id = t.func_id(static_method, "make", 0x1602);
    const auto index = make_index(dir, t, [&](Records& r) {
        add_proc(r, 0x1147, "Widget::set", 0x100, 0x20, method_id);
        r.add(0x114F, Bytes());
        add_proc(r, 0x1147, "Widget::make", 0x200, 0x20, static_id);
        r.add(0x114F, Bytes());
    });
    const pdb::ProcInfo set = info_of(*index, "Widget::set");
    REQUIRE_MESSAGE(set.params_known, set.params_error);
    REQUIRE(set.params.size() == 2);
    CHECK(set.params[0].cls == pdb::ParamClass::Int);
    CHECK(set.params[0].size == 8);
    CHECK(set.params[1].size == 4);
    const pdb::ProcInfo make = info_of(*index, "Widget::make");
    REQUIRE(make.params_known);
    REQUIRE(make.params.size() == 1);
    CHECK(make.params[0].size == 4);
}

TEST_CASE("proc_info: types that are classified - references, pointers to classes, enums, qualifiers, float, bool, 64-bit integers") {
    TempDir dir;
    Types t;
    const std::uint32_t widget = t.klass(0x1505);
    const std::uint32_t ptr_widget = t.pointer(widget);
    const std::uint32_t ref_widget = t.pointer(widget, 0x0C | (1u << 5) | (8u << 13));  // lvalue reference
    const std::uint32_t color = t.enumeration(T_INT4);
    const std::uint32_t const_int = t.modifier(T_INT4);
    const std::uint32_t args = t.arglist({ptr_widget, ref_widget, color, const_int, T_REAL32, T_BOOL08, T_UQUAD});
    const std::uint32_t proc_type = t.procedure(T_VOID, args, 7);
    const std::uint32_t id = t.func_id(proc_type, "many");
    const auto index = make_index(dir, t, [&](Records& r) {
        add_proc(r, 0x1147, "many", 0x100, 0x20, id);
        r.add(0x114F, Bytes());
    });
    const pdb::ProcInfo info = info_of(*index, "many");
    REQUIRE_MESSAGE(info.params_known, info.params_error);
    REQUIRE(info.params.size() == 7);
    const struct { pdb::ParamClass cls; std::uint8_t size; } expect[] = {
        {pdb::ParamClass::Int, 8}, {pdb::ParamClass::Int, 8}, {pdb::ParamClass::Int, 4}, {pdb::ParamClass::Int, 4},
        {pdb::ParamClass::Float, 4}, {pdb::ParamClass::Int, 1}, {pdb::ParamClass::Int, 8}};
    for (std::size_t i = 0; i < 7; ++i) {
        CHECK_MESSAGE(info.params[i].cls == expect[i].cls, "parameter " << i);
        CHECK_MESSAGE(info.params[i].size == expect[i].size, "parameter " << i);
    }
}

TEST_CASE("proc_info: what shifts or hides the arguments is reported, not guessed") {
    TempDir dir;
    Types t;
    const std::uint32_t widget = t.klass();
    const std::uint32_t by_value_args = t.arglist({T_INT4, widget});
    const std::uint32_t by_value = t.procedure(T_VOID, by_value_args, 2);
    const std::uint32_t returns_class = t.procedure(widget, t.arglist({T_INT4}), 1);
    const std::uint32_t variadic = t.procedure(T_INT4, t.arglist({T_INT4, 0}), 2);
    const std::uint32_t member_ptr = t.pointer(widget, 0x0C | (2u << 5) | (8u << 13));
    const std::uint32_t with_member_pointer = t.procedure(T_VOID, t.arglist({member_ptr}), 1);
    const std::uint32_t wide = t.procedure(T_VOID, t.arglist({0x42 /* T_REAL80 */}), 1);
    const std::uint32_t ids[] = {t.func_id(by_value, "by_value"), t.func_id(returns_class, "returns_class"), t.func_id(variadic, "variadic"),
                                 t.func_id(with_member_pointer, "member_pointer"), t.func_id(wide, "wide")};
    const auto index = make_index(dir, t, [&](Records& r) {
        const char* names[] = {"by_value", "returns_class", "variadic", "member_pointer", "wide"};
        for (int i = 0; i < 5; ++i) {
            add_proc(r, 0x1147, names[i], 0x100 + 0x20 * i, 0x10, ids[i]);
            r.add(0x114F, Bytes());
        }
    });
    const pdb::ProcInfo by_value_info = info_of(*index, "by_value");
    REQUIRE(by_value_info.params_known);
    REQUIRE(by_value_info.params.size() == 2);
    CHECK(by_value_info.params[0].cls == pdb::ParamClass::Int);
    CHECK(by_value_info.params[1].cls == pdb::ParamClass::Unsupported);
    CHECK(by_value_info.params[1].why.find("by value") != std::string::npos);

    const pdb::ProcInfo ret = info_of(*index, "returns_class");
    CHECK_FALSE(ret.params_known);
    CHECK(ret.params_error.find("by value") != std::string::npos);

    const pdb::ProcInfo var = info_of(*index, "variadic");
    CHECK_FALSE(var.params_known);
    CHECK(var.params_error.find("variable argument") != std::string::npos);

    const pdb::ProcInfo mp = info_of(*index, "member_pointer");
    REQUIRE(mp.params_known);
    CHECK(mp.params[0].cls == pdb::ParamClass::Unsupported);
    CHECK(mp.params[0].why.find("member") != std::string::npos);

    const pdb::ProcInfo w = info_of(*index, "wide");
    REQUIRE(w.params_known);
    CHECK(w.params[0].cls == pdb::ParamClass::Unsupported);
}

TEST_CASE("proc_info: an old-style S_GPROC32 refers to its function type directly; a PDB without type streams still yields the records") {
    TempDir dir;
    {
        Types t;
        const std::uint32_t args = t.arglist({T_INT4});
        const std::uint32_t proc_type = t.procedure(T_INT4, args, 1);
        const auto index = make_index(dir, t, [&](Records& r) {
            add_proc(r, 0x1110, "plain", 0x100, 0x20, proc_type);
            add_regrel(r, "a", 0x20, T_INT4, 335);
            r.end();
        });
        const pdb::ProcInfo info = info_of(*index, "plain");
        REQUIRE_MESSAGE(info.params_known, info.params_error);
        REQUIRE(info.params.size() == 1);
        CHECK(info.variables.size() == 1);
    }
    TempDir other;
    Types none;  // empty TPI and IPI
    const auto index = make_index(other, none, [&](Records& r) {
        add_proc(r, 0x1110, "plain", 0x100, 0x20, 0x1000);
        add_regrel(r, "a", 0x20, T_INT4, 335);
        r.end();
    });
    const pdb::ProcInfo info = info_of(*index, "plain");
    CHECK_FALSE(info.params_known);
    CHECK_FALSE(info.params_error.empty());
    CHECK(info.variables.size() == 1);
}

TEST_CASE("proc_info: only the direct children of the function count, and only up to its end") {
    TempDir dir;
    Types t;
    const std::uint32_t proc_type = t.procedure(T_VOID, t.arglist({T_INT4}), 1);
    const std::uint32_t id = t.func_id(proc_type, "first");
    const auto index = make_index(dir, t, [&](Records& r) {
        add_proc(r, 0x1147, "first", 0x100, 0x40, id);
        add_regrel(r, "a", 0x20, T_INT4, 335);
        r.block();
        add_regrel(r, "inner", 0x24, T_INT4, 335);  // in a nested block
        r.end();
        r.add(0x114F, Bytes());
        add_proc(r, 0x1147, "second", 0x200, 0x40, id);
        add_regrel(r, "b", 0x20, T_INT4, 335);
        r.add(0x114F, Bytes());
    });
    const pdb::ProcInfo first = info_of(*index, "first");
    REQUIRE(first.variables.size() == 1);
    CHECK(first.variables[0].name == "a");
    const pdb::ProcInfo second = info_of(*index, "second");
    REQUIRE(second.variables.size() == 1);
    CHECK(second.variables[0].name == "b");
}
