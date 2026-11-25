// WP-36 (ADR-0042): image-relative (ADDR32NB) references to data of the old image.
//
//   * the site prover (imgrel_site.h) on raw bytes: the instruction forms cl emits, and every way a site is refused;
//   * the planner: a proved site becomes an Addr32NbDisp fix-up, everything else stays CML3101 with the bytes;
//   * the triples (value = S + A - patch_base, CML3550 out of range) and their wire form.
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "cml/core/imgrel_site.h"
#include "cml/core/patch_image.h"
#include "cml/core/symbol_registry.h"
#include "cml/ipc/messages.h"
#include "doctest.h"
#include "synth.h"
#include "worlds.h"

using namespace cml_bind_test;
using namespace cml::core;
using namespace cml::core::bind;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// A section under test: bytes, relocation fields, function starts.
struct Code {
    std::vector<std::byte> data;
    std::vector<RelocField> fields;
    std::vector<std::uint32_t> starts{0};

    // Appends bytes; returns the offset they start at.
    std::uint32_t emit(std::initializer_list<int> list) {
        const std::uint32_t at = static_cast<std::uint32_t>(data.size());
        for (const int v : list) {
            data.push_back(static_cast<std::byte>(v));
        }
        return at;
    }
    // Appends bytes whose 4 bytes at `field_at` (relative to the first byte) are a relocation; returns the site.
    std::uint32_t emit_reloc(std::initializer_list<int> list, std::uint32_t field_at, std::uint32_t width = 4) {
        const std::uint32_t at = emit(list);
        fields.push_back(RelocField{at + field_at, width});
        return at + field_at;
    }
    SiteCheck check(std::uint32_t site, bool is_code = true) const {
        SiteProver p(is_code, std::span<const std::byte>(data.data(), data.size()), fields, starts);
        return p.check(site);
    }
};

// lea rdi, [rip + __ImageBase]: every cl function that addresses image-relatively starts by loading the base.
void lea_image_base(Code& c) { c.emit_reloc({0x48, 0x8D, 0x3D, 0, 0, 0, 0}, 3); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// The prover
// ---------------------------------------------------------------------------------------------

TEST_CASE("imgrel prover: the instruction forms of the cl evidence are displacements") {
    Code c;
    lea_image_base(c);
    // The bytes of spike run 45402685481 (cl 19.44 /O2 /Ob0 /Gy), displacements zero as in the object.
    const std::uint32_t mov_load = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);                  // mov eax, [rdi+rax*4+d32]
    const std::uint32_t mov_byte = c.emit_reloc({0xC6, 0x84, 0x3B, 0, 0, 0, 0, 0x00}, 3);            // mov byte ptr [rbx+rdi+d32], 0
    const std::uint32_t cmp_byte = c.emit_reloc({0x80, 0xBC, 0x3B, 0x00, 0x02, 0, 0, 0x00}, 3);      // cmp byte ptr [rbx+rdi+d32], 0 (A = 0x200)
    const std::uint32_t mov_store = c.emit_reloc({0x89, 0x9C, 0x87, 0x00, 0x04, 0, 0}, 3);           // mov [rdi+rax*4+d32], ebx
    const std::uint32_t mov_q = c.emit_reloc({0x48, 0x8B, 0x84, 0xC7, 0, 0, 0, 0}, 4);               // mov rax, [rdi+rax*8+d32]
    const std::uint32_t cvt = c.emit_reloc({0xF3, 0x0F, 0x2C, 0x84, 0x87, 0x44, 0x04, 0, 0}, 5);     // cvttss2si eax, [rdi+rax*4+d32]
    const std::uint32_t lea = c.emit_reloc({0x48, 0x8D, 0x84, 0x0F, 0, 0, 0, 0}, 4);                 // lea rax, [rdi+rcx+d32]
    const std::uint32_t rbp_base = c.emit_reloc({0x8B, 0x84, 0x85, 0, 0, 0, 0}, 3);                  // mov eax, [rbp+rax*4+d32]
    const std::uint32_t mod10 = c.emit_reloc({0x8B, 0x87, 0, 0, 0, 0}, 2);                           // mov eax, [rdi+d32]
    c.emit({0xC3});
    for (const std::uint32_t site : {mov_load, mov_byte, cmp_byte, mov_store, mov_q, cvt, lea, rbp_base, mod10}) {
        const SiteCheck k = c.check(site);
        CHECK_MESSAGE(k.proved(), "site +" << site << ": " << to_string(k.verdict) << " " << k.reason);
        CHECK(k.function_start == 0);
        CHECK(k.at < site);
        CHECK(k.at + k.length >= site + 4);
    }
    const SiteCheck k = c.check(mov_byte);
    CHECK(k.bytes == "c6 84 3b 00 00 00 00 00");
    CHECK(k.length == 8);
}

TEST_CASE("imgrel prover: an immediate is not a displacement") {
    {
        Code c;
        const std::uint32_t site = c.emit_reloc({0xB8, 0, 0, 0, 0}, 1);  // mov eax, imm32: zero-extended
        c.emit({0xC3});
        const SiteCheck k = c.check(site);
        CHECK(k.verdict == SiteVerdict::NotDisplacement);
        CHECK(contains(k.reason, "immediate"));
        CHECK(k.bytes == "b8 00 00 00 00");
        CHECK(k.at == 0);
    }
    {
        Code c;
        lea_image_base(c);
        const std::uint32_t site = c.emit_reloc({0x48, 0x05, 0, 0, 0, 0}, 2);  // add rax, imm32
        const SiteCheck k = c.check(site);
        CHECK(k.verdict == SiteVerdict::NotDisplacement);
    }
    {
        // An instruction with a displacement AND an immediate: the immediate is refused, the displacement accepted.
        Code c;
        const std::uint32_t at = c.emit({0xC7, 0x84, 0x87, 0, 0, 0, 0, 0, 0, 0, 0});  // mov dword ptr [rdi+rax*4+d32], imm32
        c.fields.push_back(RelocField{at + 3, 4});
        c.fields.push_back(RelocField{at + 7, 4});
        CHECK(c.check(at + 3).proved());
        const SiteCheck k = c.check(at + 7);
        CHECK(k.verdict == SiteVerdict::NotDisplacement);
        CHECK(contains(k.reason, "immediate"));
    }
    {
        Code c;
        const std::uint32_t site = c.emit_reloc({0xE8, 0, 0, 0, 0}, 1);  // call rel32: a branch target is an immediate here
        CHECK(c.check(site).verdict == SiteVerdict::NotDisplacement);
    }
}

TEST_CASE("imgrel prover: RIP-relative, 67-prefixed and fs/gs-relative operands are refused") {
    {
        Code c;
        const std::uint32_t site = c.emit_reloc({0x8B, 0x05, 0, 0, 0, 0}, 2);  // mov eax, [rip+d32]
        const SiteCheck k = c.check(site);
        CHECK(k.verdict == SiteVerdict::RipRelative);
        CHECK(contains(k.reason, "8b 05 00 00 00 00"));
    }
    {
        Code c;
        lea_image_base(c);
        const std::uint32_t site = c.emit_reloc({0x67, 0x8B, 0x84, 0x87, 0, 0, 0, 0}, 4);  // mov eax, [edi+eax*4+d32]
        const SiteCheck k = c.check(site);
        CHECK(k.verdict == SiteVerdict::Address32);
        CHECK(contains(k.reason, "67 8b 84 87"));
    }
    {
        Code c;
        lea_image_base(c);
        const std::uint32_t site = c.emit_reloc({0x65, 0x8B, 0x84, 0x87, 0, 0, 0, 0}, 4);  // mov eax, gs:[rdi+rax*4+d32]
        const SiteCheck k = c.check(site);
        CHECK(k.verdict == SiteVerdict::SegmentFsGs);
        CHECK(contains(k.reason, "65 8b 84 87"));
    }
    {
        Code c;
        const std::uint32_t site = c.emit_reloc({0x64, 0x48, 0x8B, 0x84, 0xC7, 0, 0, 0, 0}, 5);  // mov rax, fs:[rdi+rax*8+d32]
        CHECK(c.check(site).verdict == SiteVerdict::SegmentFsGs);
    }
}

TEST_CASE("imgrel prover: an instruction the decoder does not know stops the sweep; sites before it stay proved") {
    Code c;
    lea_image_base(c);
    const std::uint32_t before = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
    const std::uint32_t unknown = c.emit({0x06});  // push es: invalid in 64-bit mode
    const std::uint32_t after = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
    c.emit({0xC3});
    CHECK(c.check(before).proved());
    const SiteCheck k = c.check(after);
    CHECK(k.verdict == SiteVerdict::Undecodable);
    CHECK(k.at == unknown);
    CHECK(k.length == 0);
    CHECK(contains(k.reason, "not known to the instruction decoder"));
    CHECK(contains(k.reason, "06 8b 84 87"));
}

TEST_CASE("imgrel prover: a data section, a site outside the section, a site before any function") {
    Code c;
    const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
    {
        const SiteCheck k = c.check(site, /*is_code=*/false);
        CHECK(k.verdict == SiteVerdict::NotCode);
        CHECK(contains(k.reason, "data section"));
    }
    CHECK(c.check(5).verdict == SiteVerdict::OutOfSection);  // 4 bytes from +5 leave the 7-byte section
    CHECK(c.check(100).verdict == SiteVerdict::OutOfSection);
    c.starts = {};
    CHECK(c.check(site).verdict == SiteVerdict::NoFunction);
    c.starts = {5};  // the only function starts behind the site
    CHECK(c.check(site).verdict == SiteVerdict::NoFunction);
}

TEST_CASE("imgrel prover: two functions in one section, the first with a jump table behind its code") {
    Code c;
    // f1: lea rdi,[__ImageBase]; mov eax,[rdi+rax*4+$table]; add rax,rdi; jmp rax; then its 2-entry jump table.
    lea_image_base(c);
    const std::uint32_t table_load = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
    const std::uint32_t data_ref = c.emit_reloc({0xC6, 0x84, 0x3B, 0, 0, 0, 0, 0x01}, 3);
    c.emit({0x48, 0x03, 0xC7, 0xFF, 0xE0});
    const std::uint32_t table = c.emit_reloc({0, 0, 0, 0}, 0);  // DD $LN4 (ADDR32NB)
    c.emit_reloc({0, 0, 0, 0}, 0);                              // DD $LN5
    // A site that would lie BEHIND the table but still belongs to f1's sweep.
    const std::uint32_t behind = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
    c.emit({0xCC});
    // f2 starts here.
    const std::uint32_t f2 = c.emit({0x48, 0x83, 0xEC, 0x28});  // sub rsp, 28h
    lea_image_base(c);
    const std::uint32_t f2_site = c.emit_reloc({0x8B, 0x84, 0x8F, 0, 0, 0, 0}, 3);
    c.emit({0x48, 0x83, 0xC4, 0x28, 0xC3});

    c.starts = {0, f2};
    CHECK(c.check(table_load).proved());
    CHECK(c.check(data_ref).proved());
    {
        // f2's site is proved by a sweep that starts at f2, not at the section: f1's jump table is never crossed.
        const SiteCheck k = c.check(f2_site);
        CHECK_MESSAGE(k.proved(), to_string(k.verdict) << " " << k.reason);
        CHECK(k.function_start == f2);
    }
    {
        // The table entries themselves and whatever follows them inside f1 are data as far as the proof goes.
        const SiteCheck k = c.check(table);
        CHECK(k.verdict == SiteVerdict::RanIntoData);
        CHECK(k.at == table);
        CHECK(contains(k.reason, "jump table"));
        CHECK(c.check(behind).verdict == SiteVerdict::RanIntoData);
    }
    {
        // Without f2's symbol the sweep would have to cross the table: refused, never guessed.
        c.starts = {0};
        const SiteCheck k = c.check(f2_site);
        CHECK(k.verdict == SiteVerdict::RanIntoData);
        CHECK(k.at == table);
    }
}

TEST_CASE("imgrel prover: a relocation that straddles an instruction boundary stops the sweep") {
    {
        Code c;
        const std::uint32_t at = c.emit({0xB8, 0, 0, 0, 0});  // mov eax, imm32
        c.fields.push_back(RelocField{at + 3, 4});            // starts in the immediate, ends inside the next instruction
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        const SiteCheck k = c.check(site);
        CHECK(k.verdict == SiteVerdict::RanIntoData);
        CHECK(k.at == at);
        CHECK(contains(k.reason, "b8 00 00 00 00"));
    }
    {
        // A relocation over opcode bytes (before the displacement) is no operand either.
        Code c;
        const std::uint32_t at = c.emit({0x8B, 0x84, 0x87, 0, 0, 0, 0});
        c.fields.push_back(RelocField{at + 1, 4});
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        CHECK(c.check(site).verdict == SiteVerdict::RanIntoData);
    }
    {
        // A relocation type of unknown width fits nothing.
        Code c;
        c.emit_reloc({0xB8, 0, 0, 0, 0}, 1, /*width=*/0);
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        CHECK(c.check(site).verdict == SiteVerdict::RanIntoData);
    }
    {
        // A relocation that reaches from before the function into it: the function does not start on a boundary.
        Code c;
        c.emit({0, 0, 0, 0});
        c.fields.push_back(RelocField{2, 4});
        const std::uint32_t f = c.emit({0x90, 0x90});
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        c.starts = {f};
        CHECK(c.check(site).verdict == SiteVerdict::RanIntoData);
    }
    {
        // Operands the sweep passes that DO fit (REL32 on a call, REL32_1 on a RIP-relative cmp, ADDR64 in mov r64, imm64,
        // SECREL in a displacement) do not disturb it.
        Code c;
        c.emit_reloc({0xE8, 0, 0, 0, 0}, 1);
        c.emit_reloc({0x83, 0x3D, 0, 0, 0, 0, 0x00}, 2);
        c.emit_reloc({0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0}, 2, 8);
        c.emit_reloc({0x8B, 0x80, 0, 0, 0, 0}, 2);
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        CHECK(c.check(site).proved());
    }
}

TEST_CASE("imgrel prover (WP-40, R-50): every relocation just before a function start is looked at, one of unknown width reaches in") {
    // main looked only at the LAST relocation before the start, and one of unknown width (0) counted as reaching nowhere.
    {
        // A relocation that reaches into the function, followed by one of unknown width that starts before the function.
        Code c;
        c.emit({0, 0, 0, 0, 0, 0});
        c.fields.push_back(RelocField{3, 4});  // 3..7: reaches 1 byte into the function at 6
        c.fields.push_back(RelocField{5, 1});  // the last one before the start, wholly before it
        const std::uint32_t f = c.emit({0x90, 0x90});
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        c.starts = {f};
        const SiteCheck k = c.check(site);
        CHECK(k.verdict == SiteVerdict::RanIntoData);
        CHECK(contains(k.reason, "the relocation at +0x3 reaches into the function"));
    }
    {
        // A relocation of unknown width right before the start.
        Code c;
        c.emit({0, 0, 0, 0});
        c.fields.push_back(RelocField{3, 0});
        const std::uint32_t f = c.emit({0x90});
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        c.starts = {f};
        const SiteCheck k = c.check(site);
        CHECK(k.verdict == SiteVerdict::RanIntoData);
        CHECK(contains(k.reason, "of a type whose width is not known"));
    }
    {
        // Relocations 8 bytes or more before the start (and of known width, ending before it) do not matter.
        Code c;
        c.emit({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        c.fields.push_back(RelocField{0, 0});
        c.fields.push_back(RelocField{4, 4});
        const std::uint32_t f = c.emit({0x90});
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        c.starts = {f};
        CHECK(c.check(site).proved());
    }
}

TEST_CASE("imgrel layout (WP-40, R-50): a VEX or EVEX prefix after any REX or after 66/F2/F3/F0 is not located") {
    const auto layout = [](std::initializer_list<int> insn, int disp_offset, int disp_size) {
        std::vector<std::uint8_t> bytes;
        for (const int v : insn) {
            bytes.push_back(static_cast<std::uint8_t>(v));
        }
        return parse_layout(bytes.data(), bytes.size(), static_cast<std::uint8_t>(disp_offset), static_cast<std::uint8_t>(disp_size));
    };
    // vmovd xmm0,[rdi+rax*4+d32] is C5 F9 6E 84 87 d32; ModRM at 3, displacement at 5.
    CHECK(layout({0xC5, 0xF9, 0x6E, 0x84, 0x87, 0, 0, 0, 0}, 5, 4).known);
    CHECK(layout({0x2E, 0xC5, 0xF9, 0x6E, 0x84, 0x87, 0, 0, 0, 0}, 6, 4).known);  // a segment prefix is allowed
    // main: a REX followed by a legacy prefix was forgotten ("rex" was reset), so these were located.
    CHECK_FALSE(layout({0x48, 0x2E, 0xC5, 0xF9, 0x6E, 0x84, 0x87, 0, 0, 0, 0}, 7, 4).known);
    CHECK_FALSE(layout({0x48, 0xC5, 0xF9, 0x6E, 0x84, 0x87, 0, 0, 0, 0}, 6, 4).known);
    CHECK_FALSE(layout({0x66, 0xC5, 0xF9, 0x6E, 0x84, 0x87, 0, 0, 0, 0}, 6, 4).known);
    CHECK_FALSE(layout({0xF3, 0xC5, 0xF9, 0x6E, 0x84, 0x87, 0, 0, 0, 0}, 6, 4).known);
    CHECK_FALSE(layout({0xF0, 0x62, 0xF1, 0x7D, 0x48, 0x6F, 0x84, 0x87, 0, 0, 0, 0}, 8, 4).known);
}

// ---- review of PR #67: what the first version accepted and must not ---------------------------------------------

TEST_CASE("imgrel prover: lea with a 32-bit or 16-bit destination truncates the address and is refused; lea r64 is proved") {
    // `lea r32,[index+sym@IMGREL]` computes an RVA in 32 bits, which is valid in the original code; retargeting the
    // displacement would turn it into low32(target - patch_base + index), which is not the old RVA. (WP-40, R-50: the
    // wording; the rule is unchanged. A 32-bit lea of the full address through a register that holds __ImageBase
    // truncates in the original code already.)
    const auto verdict_of = [](std::initializer_list<int> insn, std::uint32_t field_at) {
        Code c;
        lea_image_base(c);
        const std::uint32_t site = c.emit_reloc(insn, field_at);
        c.emit({0xC3});
        return c.check(site);
    };
    {
        const SiteCheck k = verdict_of({0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 3);  // lea eax,[rdi+rcx*4+d32]
        CHECK(k.verdict == SiteVerdict::LeaTruncates);
        CHECK(contains(k.reason, "8d 84 8f 00 00 00 00"));
        CHECK(contains(k.reason, "lea"));
    }
    CHECK(verdict_of({0x66, 0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 4).verdict == SiteVerdict::LeaTruncates);        // lea ax,[...]
    CHECK(verdict_of({0x41, 0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 4).verdict == SiteVerdict::LeaTruncates);        // REX.B only: lea eax,[r15+rcx*4+d32]
    CHECK(verdict_of({0x44, 0x8D, 0x87, 0, 0, 0, 0}, 3).verdict == SiteVerdict::LeaTruncates);              // REX.R only: lea r8d,[rdi+d32]
    CHECK(verdict_of({0x48, 0x66, 0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 5).verdict == SiteVerdict::LeaTruncates);  // a REX before a legacy prefix is ignored
    CHECK(verdict_of({0x48, 0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 4).proved());                                    // lea rax,[rdi+rcx*4+d32]
    CHECK(verdict_of({0x4C, 0x8D, 0x84, 0x0F, 0, 0, 0, 0}, 4).proved());                                    // lea r8,[rdi+rcx+d32]
    CHECK(verdict_of({0x48, 0x8D, 0x87, 0, 0, 0, 0}, 3).proved());                                          // lea rax,[rdi+d32]
    // Only lea computes an address into a register: a 32-bit LOAD through the same operand is fine.
    CHECK(verdict_of({0x8B, 0x84, 0x8F, 0, 0, 0, 0}, 3).proved());                                          // mov eax,[rdi+rcx*4+d32]
    CHECK(verdict_of({0x66, 0x8B, 0x84, 0x8F, 0, 0, 0, 0}, 4).proved());                                    // mov ax,[rdi+rcx*4+d32]
}

TEST_CASE("imgrel prover: an operand without a base register cannot hold the image base and is refused") {
    const auto verdict_of = [](std::initializer_list<int> insn, std::uint32_t field_at) {
        Code c;
        lea_image_base(c);
        const std::uint32_t site = c.emit_reloc(insn, field_at);
        c.emit({0xC3});
        return c.check(site);
    };
    {
        const SiteCheck k = verdict_of({0x8B, 0x04, 0x25, 0, 0, 0, 0}, 3);  // mov eax,[disp32]: absolute, no base, no index
        CHECK(k.verdict == SiteVerdict::NoBaseRegister);
        CHECK(contains(k.reason, "8b 04 25 00 00 00 00"));
        CHECK(contains(k.reason, "no base register"));
    }
    CHECK(verdict_of({0x8B, 0x04, 0x85, 0, 0, 0, 0}, 3).verdict == SiteVerdict::NoBaseRegister);        // mov eax,[rax*4+d32]
    CHECK(verdict_of({0x42, 0x8B, 0x04, 0x85, 0, 0, 0, 0}, 4).verdict == SiteVerdict::NoBaseRegister);  // mov eax,[r8*4+d32]
    CHECK(verdict_of({0x8B, 0x04, 0x3D, 0, 0, 0, 0}, 3).verdict == SiteVerdict::NoBaseRegister);        // mov eax,[rdi*1+d32]
    CHECK(verdict_of({0x8D, 0x04, 0x8D, 0, 0, 0, 0}, 3).verdict == SiteVerdict::NoBaseRegister);        // lea eax,[rcx*4+d32]
    CHECK(verdict_of({0x48, 0x8D, 0x04, 0x8D, 0, 0, 0, 0}, 4).verdict == SiteVerdict::NoBaseRegister);  // lea rax,[rcx*4+d32]
    CHECK(verdict_of({0xC6, 0x04, 0x25, 0, 0, 0, 0, 0x01}, 3).verdict == SiteVerdict::NoBaseRegister);  // mov byte ptr [disp32], 1
    // With a base (mod=10) the same shapes are proved: rbp/r13 as base need mod=01/10 and have a displacement.
    CHECK(verdict_of({0x8B, 0x84, 0x85, 0, 0, 0, 0}, 3).proved());                                      // mov eax,[rbp+rax*4+d32]
    CHECK(verdict_of({0x8B, 0x87, 0, 0, 0, 0}, 2).proved());                                            // mov eax,[rdi+d32]
}

TEST_CASE("imgrel prover: a relocation that ends with the instruction but covers prefix, opcode, ModRM or SIB bytes stops the sweep") {
    // Each of these was accepted as "the immediate" by a check that only asked for a field ending at the instruction's
    // end. That is how a sweep misaligned by data could carry on and later prove a site among real instruction bytes.
    const auto site_behind = [](std::initializer_list<int> insn, std::uint32_t field_at, std::uint32_t width) {
        Code c;
        const std::uint32_t at = c.emit(insn);
        c.fields.push_back(RelocField{at + field_at, width});
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        return c.check(site);
    };
    {
        const SiteCheck k = site_behind({0x66, 0x41, 0x81, 0xC0, 0, 0}, 2, 4);  // add r8w, imm16: the relocation covers 81 c0 too
        CHECK(k.verdict == SiteVerdict::RanIntoData);
        CHECK(k.at == 0);
        CHECK(contains(k.reason, "66 41 81 c0 00 00"));
    }
    CHECK(site_behind({0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x08}, 2, 4).verdict == SiteVerdict::RanIntoData);  // palignr: opcode, ModRM, imm8
    CHECK(site_behind({0xF3, 0x48, 0x0F, 0x2A, 0xC1}, 1, 4).verdict == SiteVerdict::RanIntoData);        // cvtsi2ss: REX, opcode, ModRM
    CHECK(site_behind({0x48, 0x0F, 0xAF, 0xC1, 0x90}, 0, 4).verdict == SiteVerdict::RanIntoData);        // imul rax, rcx: the whole instruction
    CHECK(site_behind({0x41, 0x83, 0xC0, 0x05}, 0, 4).verdict == SiteVerdict::RanIntoData);              // add r8d, 5 (imm8)
    CHECK(site_behind({0x48, 0x89, 0x44, 0x24, 0x08}, 1, 4).verdict == SiteVerdict::RanIntoData);        // mov [rsp+8], rax: opcode, ModRM, SIB, disp8
    CHECK(site_behind({0x48, 0xC7, 0xC0, 0, 0, 0, 0}, 2, 4).verdict == SiteVerdict::RanIntoData);        // mov rax, imm32: ModRM + 3 of 4 bytes, not at the end
    CHECK(site_behind({0x6A, 0x05, 0x90, 0x90}, 0, 2).verdict == SiteVerdict::RanIntoData);              // push 5: a 2-byte relocation
    CHECK(site_behind({0xEB, 0x00}, 1, 1).verdict == SiteVerdict::RanIntoData);                          // jmp short: a 1-byte relocation
    CHECK(site_behind({0xC2, 0x08, 0x00}, 1, 2).verdict == SiteVerdict::RanIntoData);                    // ret 8: a 2-byte relocation on imm16
    CHECK(site_behind({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3, 1).verdict == SiteVerdict::RanIntoData);        // inside the displacement but not the displacement
    CHECK(site_behind({0x8B, 0x44, 0x87, 0x10}, 3, 1).verdict == SiteVerdict::RanIntoData);              // a disp8 is never a relocation
    CHECK(site_behind({0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0}, 2, 4).verdict == SiteVerdict::RanIntoData);  // the low half of an imm64

    // The immediates a relocation really fills still pass, and the site behind them is proved.
    CHECK(site_behind({0xE8, 0, 0, 0, 0}, 1, 4).proved());                             // call rel32
    CHECK(site_behind({0xE9, 0, 0, 0, 0}, 1, 4).proved());                             // jmp rel32
    CHECK(site_behind({0x0F, 0x84, 0, 0, 0, 0}, 2, 4).proved());                       // je rel32
    CHECK(site_behind({0xB8, 0, 0, 0, 0}, 1, 4).proved());                             // mov eax, imm32
    CHECK(site_behind({0x41, 0xB8, 0, 0, 0, 0}, 2, 4).proved());                       // mov r8d, imm32
    CHECK(site_behind({0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0}, 2, 8).proved());           // mov rax, imm64 (ADDR64)
    CHECK(site_behind({0x68, 0, 0, 0, 0}, 1, 4).proved());                             // push imm32
    CHECK(site_behind({0x05, 0, 0, 0, 0}, 1, 4).proved());                             // add eax, imm32
    CHECK(site_behind({0x48, 0x3D, 0, 0, 0, 0}, 2, 4).proved());                       // cmp rax, imm32
    CHECK(site_behind({0xA9, 0, 0, 0, 0}, 1, 4).proved());                             // test eax, imm32
    CHECK(site_behind({0x48, 0x81, 0xC0, 0, 0, 0, 0}, 3, 4).proved());                 // add rax, imm32
    CHECK(site_behind({0xC7, 0x00, 0, 0, 0, 0}, 2, 4).proved());                       // mov dword ptr [rax], imm32
    CHECK(site_behind({0x48, 0xC7, 0xC0, 0, 0, 0, 0}, 3, 4).proved());                 // mov rax, imm32
    CHECK(site_behind({0xF7, 0xC0, 0, 0, 0, 0}, 2, 4).proved());                       // test eax, imm32 (F7 /0)
    CHECK(site_behind({0x69, 0xC0, 0, 0, 0, 0}, 2, 4).proved());                       // imul eax, eax, imm32
    CHECK(site_behind({0xC7, 0x84, 0x87, 0, 0, 0, 0, 0, 0, 0, 0}, 7, 4).proved());     // mov dword ptr [rdi+rax*4+d32], imm32: the immediate
    CHECK(site_behind({0xC7, 0x84, 0x87, 0, 0, 0, 0, 0, 0, 0, 0}, 3, 4).proved());     // ... and its displacement
    CHECK(site_behind({0x83, 0x3D, 0, 0, 0, 0, 0x00}, 2, 4).proved());                 // cmp dword ptr [rip+d32], 0 (REL32_1)
}

TEST_CASE("imgrel layout: ModRM, SIB, displacement and immediate start are derived from the bytes and checked against the decoder") {
    const auto layout = [](std::initializer_list<int> insn, int disp_offset, int disp_size) {
        std::vector<std::uint8_t> bytes;
        for (const int v : insn) {
            bytes.push_back(static_cast<std::uint8_t>(v));
        }
        return parse_layout(bytes.data(), bytes.size(), static_cast<std::uint8_t>(disp_offset), static_cast<std::uint8_t>(disp_size));
    };
    SUBCASE("legacy encodings") {
        InsnLayout l = layout({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3, 4);  // mov eax,[rdi+rax*4+d32]
        CHECK(l.known);
        CHECK(l.has_modrm);
        CHECK_FALSE(l.vex);
        CHECK_FALSE(l.no_base);
        CHECK_FALSE(l.lea);
        CHECK(l.immediate_start == 7);
        l = layout({0xC6, 0x84, 0x3B, 0, 0, 0, 0, 0x01}, 3, 4);  // mov byte ptr [rbx+rdi+d32], 1
        CHECK(l.known);
        CHECK(l.immediate_start == 7);
        l = layout({0xF3, 0x0F, 0x2C, 0x84, 0x87, 0, 0, 0, 0}, 5, 4);  // cvttss2si eax,[rdi+rax*4+d32]
        CHECK(l.known);
        CHECK(l.immediate_start == 9);
        l = layout({0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x08}, 0, 0);  // palignr xmm0, xmm1, 8 (three-byte map)
        CHECK(l.known);
        CHECK(l.immediate_start == 5);
        l = layout({0x48, 0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 4, 4);  // lea rax,[rdi+rcx*4+d32]
        CHECK(l.known);
        CHECK(l.lea);
        CHECK(l.rex_w);
        l = layout({0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 3, 4);  // lea eax,[...]
        CHECK(l.known);
        CHECK(l.lea);
        CHECK_FALSE(l.rex_w);
        l = layout({0x48, 0x66, 0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 5, 4);  // the REX is ignored: a legacy prefix follows it
        CHECK(l.known);
        CHECK(l.lea);
        CHECK_FALSE(l.rex_w);
        l = layout({0x8B, 0x04, 0x25, 0, 0, 0, 0}, 3, 4);  // mov eax,[disp32]
        CHECK(l.known);
        CHECK(l.no_base);
        l = layout({0x8B, 0x05, 0, 0, 0, 0}, 2, 4);  // mov eax,[rip+d32]: has no SIB; RIP-relative is the decoder's flag
        CHECK(l.known);
        CHECK_FALSE(l.no_base);
        l = layout({0x8B, 0x44, 0x24, 0x08}, 3, 1);  // mov eax,[rsp+8]
        CHECK(l.known);
        CHECK(l.immediate_start == 4);
        l = layout({0x8B, 0xC1}, 0, 0);  // mov eax, ecx
        CHECK(l.known);
        CHECK(l.has_modrm);
        CHECK(l.immediate_start == 2);
    }
    SUBCASE("opcodes without ModRM") {
        InsnLayout l = layout({0xE8, 0, 0, 0, 0}, 0, 0);  // call rel32
        CHECK(l.known);
        CHECK_FALSE(l.has_modrm);
        CHECK(l.immediate_start == 1);
        l = layout({0x0F, 0x84, 0, 0, 0, 0}, 0, 0);  // je rel32
        CHECK(l.known);
        CHECK(l.immediate_start == 2);
        l = layout({0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 0);  // mov rax, imm64
        CHECK(l.known);
        CHECK(l.rex_w);
        CHECK(l.immediate_start == 2);
        l = layout({0xC3}, 0, 0);  // ret: nothing behind the opcode
        CHECK(l.known);
        CHECK_FALSE(l.has_modrm);
        CHECK(l.immediate_start == 1);
        // An opcode without ModRM that is not on the list is read as if it had one. Here the pretended ModRM (05: a
        // RIP-relative operand) asks for a displacement the decoder did not report: the layout is unknown.
        l = layout({0x6A, 0x05}, 0, 0);  // push 5
        CHECK_FALSE(l.known);
    }
    SUBCASE("disagreement with the decoder is never resolved in favour of either") {
        CHECK_FALSE(layout({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 2, 4).known);  // displacement one byte early
        CHECK_FALSE(layout({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3, 1).known);  // another size
        CHECK_FALSE(layout({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 0, 0).known);  // none reported
        CHECK_FALSE(layout({0x8B, 0xC1}, 2, 4).known);                    // one reported for a register operand
        CHECK_FALSE(layout({0xE8, 0, 0, 0, 0}, 1, 4).known);              // a displacement on call rel32
        CHECK_FALSE(layout({0x8B, 0x84}, 0, 0).known);                    // cut off behind the ModRM that announces a SIB
        CHECK_FALSE(layout({0x66, 0x66}, 0, 0).known);                    // prefixes only
        CHECK_FALSE(parse_layout(nullptr, 4, 0, 0).known);
    }
    SUBCASE("VEX and EVEX: only the displacement is located") {
        InsnLayout l = layout({0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 5, 4);  // vmovss xmm0,[rdi+rax*4+d32]
        CHECK(l.known);
        CHECK(l.vex);
        CHECK(l.has_modrm);
        CHECK_FALSE(l.no_base);
        CHECK_FALSE(l.lea);
        CHECK(l.immediate_start == 9);  // = the length: no immediate is located
        l = layout({0xC4, 0xE2, 0x79, 0x18, 0x84, 0x87, 0, 0, 0, 0}, 6, 4);  // vbroadcastss xmm0,[rdi+rax*4+d32]
        CHECK(l.known);
        CHECK(l.vex);
        l = layout({0x62, 0xF1, 0x7C, 0x48, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 7, 4);  // vmovups zmm0,[rdi+rax*4+d32]
        CHECK(l.known);
        CHECK(l.vex);
        l = layout({0xC5, 0xFA, 0x10, 0x04, 0x85, 0, 0, 0, 0}, 5, 4);  // vmovss xmm0,[rax*4+d32]
        CHECK(l.known);
        CHECK(l.no_base);
        l = layout({0xC5, 0xF8, 0x77}, 0, 0);  // vzeroupper: no ModRM
        CHECK(l.known);
        CHECK_FALSE(l.has_modrm);
        CHECK_FALSE(layout({0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 4, 4).known);        // not where the decoder says
        CHECK_FALSE(layout({0x48, 0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 6, 4).known);  // a REX before a VEX prefix is invalid
    }
}

// ---- VEX / EVEX through the real decoder (WP-38 made it accept them) -------------------------------------------

TEST_CASE("imgrel prover: a VEX or EVEX instruction is proved at its displacement and nowhere else") {
    const auto verdict_of = [](std::initializer_list<int> insn, std::uint32_t field_at) {
        Code c;
        lea_image_base(c);
        const std::uint32_t site = c.emit_reloc(insn, field_at);
        c.emit({0xC3});
        return c.check(site);
    };
    {
        const SiteCheck k = verdict_of({0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 5);  // vmovss xmm0, dword ptr [rdi+rax*4+d32]
        CHECK_MESSAGE(k.proved(), to_string(k.verdict) << " " << k.reason);
        CHECK(k.at == 7);
        CHECK(k.length == 9);
        CHECK(k.bytes == "c5 fa 10 84 87 00 00 00 00");
    }
    CHECK(verdict_of({0xC5, 0xFA, 0x11, 0x84, 0x87, 0, 0, 0, 0}, 5).proved());                    // vmovss [rdi+rax*4+d32], xmm0 (C5: two-byte VEX)
    CHECK(verdict_of({0xC4, 0xE2, 0x79, 0x18, 0x84, 0x87, 0, 0, 0, 0}, 6).proved());              // vbroadcastss xmm0, [rdi+rax*4+d32] (C4: three-byte VEX)
    CHECK(verdict_of({0xC4, 0xE3, 0x79, 0x04, 0x84, 0x87, 0, 0, 0, 0, 0x1B}, 6).proved());        // vpermilps xmm0, [rdi+rax*4+d32], 1Bh (imm8 behind it)
    CHECK(verdict_of({0x62, 0xF1, 0x7C, 0x48, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 7).proved());        // vmovups zmm0, [rdi+rax*4+d32] (EVEX)
    CHECK(verdict_of({0xC5, 0xFA, 0x10, 0x87, 0, 0, 0, 0}, 4).proved());                          // vmovss xmm0, [rdi+d32]

    // The same refusals as for legacy encodings: the decoder's flags come through for VEX, and so does the base rule.
    CHECK(verdict_of({0xC5, 0xFA, 0x10, 0x05, 0, 0, 0, 0}, 4).verdict == SiteVerdict::RipRelative);                  // [rip+d32]
    CHECK(verdict_of({0x67, 0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 6).verdict == SiteVerdict::Address32);        // 67 prefix
    CHECK(verdict_of({0x65, 0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 6).verdict == SiteVerdict::SegmentFsGs);      // gs:
    CHECK(verdict_of({0x64, 0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 6).verdict == SiteVerdict::SegmentFsGs);      // fs:
    CHECK(verdict_of({0x65, 0x62, 0xF1, 0x7C, 0x48, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 8).verdict == SiteVerdict::SegmentFsGs);  // gs: on EVEX
    CHECK(verdict_of({0xC5, 0xFA, 0x10, 0x04, 0x85, 0, 0, 0, 0}, 5).verdict == SiteVerdict::NoBaseRegister);         // [rax*4+d32]
    CHECK(verdict_of({0xC5, 0xFA, 0x10, 0x04, 0x25, 0, 0, 0, 0}, 5).verdict == SiteVerdict::NoBaseRegister);         // [d32]
}

TEST_CASE("imgrel prover: a relocation anywhere but the displacement of a VEX instruction stops the sweep") {
    const auto site_behind = [](std::initializer_list<int> insn, std::uint32_t field_at, std::uint32_t width) {
        Code c;
        const std::uint32_t at = c.emit(insn);
        c.fields.push_back(RelocField{at + field_at, width});
        const std::uint32_t site = c.emit_reloc({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3);
        return c.check(site);
    };
    {
        // vshufps xmm0, xmm0, xmm1, 8 (c5 f8 c6 c1 08): a relocation over its last four bytes ends with the instruction.
        const SiteCheck k = site_behind({0xC5, 0xF8, 0xC6, 0xC1, 0x08}, 1, 4);
        CHECK(k.verdict == SiteVerdict::RanIntoData);
        CHECK(k.at == 0);
        CHECK(contains(k.reason, "c5 f8 c6 c1 08"));
    }
    CHECK(site_behind({0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 4, 4).verdict == SiteVerdict::RanIntoData);             // one byte early: SIB + 3 of the displacement
    CHECK(site_behind({0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 1, 4).verdict == SiteVerdict::RanIntoData);             // payload, opcode, ModRM, SIB
    CHECK(site_behind({0xC4, 0xE3, 0x79, 0x04, 0x84, 0x87, 0, 0, 0, 0, 0x1B}, 7, 4).verdict == SiteVerdict::RanIntoData); // 3 of the displacement + the imm8
    CHECK(site_behind({0xC4, 0xE3, 0x79, 0x04, 0x84, 0x87, 0, 0, 0, 0, 0x1B}, 10, 1).verdict == SiteVerdict::RanIntoData);  // the imm8 itself
    CHECK(site_behind({0x62, 0xF1, 0x7C, 0x48, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 3, 4).verdict == SiteVerdict::RanIntoData); // inside the EVEX payload
    CHECK(site_behind({0xC5, 0xFA, 0x10, 0x44, 0x87, 0x10}, 5, 1).verdict == SiteVerdict::RanIntoData);                   // a disp8 is never a relocation
    // At the displacement the sweep goes on and the site behind it is proved.
    CHECK(site_behind({0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 5, 4).proved());
    CHECK(site_behind({0xC5, 0xFA, 0x10, 0x05, 0, 0, 0, 0}, 4, 4).proved());                                                // vmovss xmm0, [rip+d32] (REL32)
    CHECK(site_behind({0x62, 0xF1, 0x7C, 0x48, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 7, 4).proved());
    CHECK(site_behind({0xC4, 0xE3, 0x79, 0x04, 0x05, 0, 0, 0, 0, 0x1B}, 5, 4).proved());                                    // REL32_1 on a RIP-relative VEX operand
    // VEX instructions without any relocation never stop it.
    {
        Code c;
        c.emit({0xC5, 0xF8, 0x77});              // vzeroupper
        c.emit({0xC5, 0xF8, 0x28, 0xC1});        // vmovaps xmm0, xmm1
        c.emit({0xC5, 0xF8, 0xC6, 0xC1, 0x08});  // vshufps xmm0, xmm0, xmm1, 8
        const std::uint32_t site = c.emit_reloc({0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 5);
        CHECK(c.check(site).proved());
    }
}

// ---------------------------------------------------------------------------------------------
// The planner
// ---------------------------------------------------------------------------------------------

namespace {

void put(SynthObj& o, std::uint32_t section, std::uint32_t offset, std::initializer_list<int> list) {
    coff::ModelSection& s = o.model().sections[section - 1];
    if (s.data.size() < offset + list.size()) {
        s.data.resize(offset + list.size(), std::byte{0});
    }
    std::uint32_t i = offset;
    for (const int v : list) {
        s.data[i++] = static_cast<std::byte>(v);
    }
}

const cml::Diag* with_code(const std::vector<cml::Diag>& v, std::uint32_t code) {
    for (const cml::Diag& d : v) {
        if (d.code == code) {
            return &d;
        }
    }
    return nullptr;
}

const Fixup* fixup_at(const BindPlan& p, std::uint32_t section, std::uint32_t offset) {
    for (const Fixup& f : p.fixups) {
        if (f.section_index == section && f.offset == offset && f.kind != FixupKind::Slot) {
            return &f;
        }
    }
    return nullptr;
}

// compute_value(): lea rdi,[__ImageBase]; <the instruction under test, ADDR32NB against g_tick at `field_at`>; ret.
struct OneSite {
    SynthWorld w = basic_world();
    SynthObj o;
    SynthObj::Func f;
    std::uint32_t site = 0;

    explicit OneSite(std::initializer_list<int> insn, std::uint32_t field_at, std::int64_t addend = 0, std::initializer_list<int> before = {}) {
        f = o.function("?compute_value@@YAHXZ", true, 0x40);
        const std::uint32_t base = o.undefined("__ImageBase", false);
        const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
        put(o, f.section, 0, {0x48, 0x8D, 0x3D, 0, 0, 0, 0});
        o.reloc(f.section, 3, base, kRel32);
        std::uint32_t at = 7;
        put(o, f.section, at, before);
        at += static_cast<std::uint32_t>(before.size());
        put(o, f.section, at, insn);
        site = at + field_at;
        o.reloc(f.section, site, tick, kAddr32Nb, addend);
        put(o, f.section, at + static_cast<std::uint32_t>(insn.size()), {0xC3});
        // The rest of the 0x40-byte section is int3 padding, as the compiler leaves it.
        coff::ModelSection& s = o.model().sections[f.section - 1];
        for (std::size_t i = at + insn.size() + 1; i < s.data.size(); ++i) {
            s.data[i] = std::byte{0xCC};
        }
    }
};

}  // namespace

TEST_CASE("imgrel plan: a proved displacement to old data becomes an Addr32NbDisp fix-up with the in-place addend") {
    OneSite s({0x80, 0xBC, 0x3B, 0, 0, 0, 0, 0x00}, 3, 0x200);  // cmp byte ptr g_tick[rbx+rdi+200h], 0
    Built b = s.o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(s.w.input(b), &errors);
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(r->imgrel_sites == 1);
    CHECK(r->imgrel_verified == 1);
    CHECK(r->imgrel_line("logic.cpp.obj") == "WP36-IMGREL sites=1 verified=1 object=logic.cpp.obj");
    const Fixup* fx = fixup_at(*r, s.f.section, s.site);
    REQUIRE(fx != nullptr);
    CHECK(fx->kind == FixupKind::Addr32NbDisp);
    CHECK(std::string(to_string(fx->kind)) == "ADDR32NB_DISP");
    CHECK(fx->addend == 0x200);
    CHECK(fx->target_va == SynthWorld::kBase + 0x3080);
    CHECK(fx->symbol == "?g_tick@@3HA");
    CHECK_FALSE(fx->thunk_fallback);
    // __ImageBase stays the patch's: linker-provided, no fix-up on the lea.
    CHECK(find(*r, "__ImageBase")->decision == DecisionKind::LinkerProvided);
    CHECK(fixup_at(*r, s.f.section, 3) == nullptr);
    // The section needs a locator like any section with a fix-up.
    CHECK(std::find(r->locator_sections.begin(), r->locator_sections.end(), s.f.section) != r->locator_sections.end());
}

TEST_CASE("imgrel plan: data the object defines itself (bound to the old image) and a negative addend") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t total_sec = o.section(".data", kData, 8);
    const std::uint32_t total = o.symbol("?total@Accum@@2HA", 0, static_cast<std::int32_t>(total_sec), false);
    put(o, f.section, 0, {0x8B, 0x84, 0x87, 0, 0, 0, 0, 0xC3});  // mov eax, total[rdi+rax*4-4]
    o.reloc(f.section, 3, total, kAddr32Nb, -4);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const Fixup* fx = fixup_at(*r, f.section, 3);
    REQUIRE(fx != nullptr);
    CHECK(fx->kind == FixupKind::Addr32NbDisp);
    CHECK(fx->addend == -4);
    CHECK(fx->target_va == SynthWorld::kBase + 0x3090);
    CHECK(r->imgrel_sites == 1);
    CHECK(r->imgrel_verified == 1);
}

TEST_CASE("imgrel plan: an immediate site is CML3101 and the message names the instruction bytes") {
    OneSite s({0xB8, 0, 0, 0, 0}, 1);  // mov eax, g_tick@IMGREL
    Built b = s.o.build();
    std::vector<cml::Diag> errors;
    BindPlan partial;
    auto r = plan_bind(s.w.input(b), &errors, &partial);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kAddr32NbToOldData);
    CHECK(errors[0].id() == "CML3101");
    CHECK(contains(errors[0].message, "ADDR32NB"));
    CHECK(contains(errors[0].message, "?g_tick@@3HA"));
    CHECK(contains(errors[0].message, "compute_value"));
    CHECK(contains(errors[0].message, "old.exe"));
    CHECK(contains(errors[0].message, "b8 00 00 00 00"));
    CHECK(contains(errors[0].message, "immediate"));
    CHECK(contains(errors[0].message, "+0x8"));
    CHECK_FALSE(errors[0].what_to_do.empty());
    CHECK(partial.imgrel_sites == 1);
    CHECK(partial.imgrel_verified == 0);
}

TEST_CASE("imgrel plan: an unknown instruction before the site is CML3101") {
    OneSite s({0x8B, 0x84, 0x87, 0, 0, 0, 0}, 3, 0, {0x06});  // push es (invalid in 64-bit mode) before the mov
    Built b = s.o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(s.w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kAddr32NbToOldData);
    CHECK(contains(errors[0].message, "not known to the instruction decoder"));
    CHECK(contains(errors[0].message, "06 8b 84 87"));
    CHECK(contains(errors[0].message, "+0x7"));
}

TEST_CASE("imgrel plan: a site in a data section is CML3101") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ");
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t rdata = o.section(".rdata", kRdata, 0x10);
    o.symbol("?table@@3QBHB", 0, static_cast<std::int32_t>(rdata), false);
    o.reloc(rdata, 4, tick, kAddr32Nb, 0x10);
    (void)f;
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kAddr32NbToOldData);
    CHECK(contains(errors[0].message, "data section"));
    CHECK(contains(errors[0].message, "10 00 00 00"));  // the bytes at the site
    CHECK(contains(errors[0].message, ".rdata"));
}

TEST_CASE("imgrel plan: 67-prefixed, gs-relative and RIP-relative sites are CML3101") {
    {
        OneSite s({0x67, 0x8B, 0x84, 0x87, 0, 0, 0, 0}, 4);
        Built b = s.o.build();
        std::vector<cml::Diag> errors;
        REQUIRE(!plan_bind(s.w.input(b), &errors).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAddr32NbToOldData);
        CHECK(contains(errors[0].message, "67 prefix"));
        CHECK(contains(errors[0].message, "67 8b 84 87 00 00 00 00"));
    }
    {
        OneSite s({0x65, 0x8B, 0x84, 0x87, 0, 0, 0, 0}, 4);
        Built b = s.o.build();
        std::vector<cml::Diag> errors;
        REQUIRE(!plan_bind(s.w.input(b), &errors).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAddr32NbToOldData);
        CHECK(contains(errors[0].message, "fs/gs"));
        CHECK(contains(errors[0].message, "65 8b 84 87 00 00 00 00"));
    }
    {
        OneSite s({0x8B, 0x05, 0, 0, 0, 0}, 2);
        Built b = s.o.build();
        std::vector<cml::Diag> errors;
        REQUIRE(!plan_bind(s.w.input(b), &errors).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAddr32NbToOldData);
        CHECK(contains(errors[0].message, "RIP-relative"));
    }
}

TEST_CASE("imgrel plan: two functions in one section, the first with a jump table; a site the sweep reaches only through the table is CML3101") {
    SynthWorld w = basic_world();
    const auto build = [&](bool behind_table, bool second_symbol, SynthObj& o, std::uint32_t* text_out, std::uint32_t* f1_site, std::uint32_t* f2_site,
                           std::uint32_t* behind_site) {
        const std::uint32_t text = o.section(".text$mn", kText, 0x60);
        *text_out = text;
        o.symbol("?compute_value@@YAHXZ", 0, static_cast<std::int32_t>(text), true);
        const std::uint32_t base = o.undefined("__ImageBase", false);
        const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
        const std::uint32_t total = o.undefined("?total@Accum@@2HA", false);
        // The case label the jump table points at: a code label of the section, as cl writes it (patch-local, never bound).
        auto label_symbol = o.model().add_symbol("$LN4@compute_value", 0x1B, static_cast<std::int32_t>(text), 0, coff::StorageClass::Label);
        REQUIRE_MESSAGE(label_symbol.has_value(), label_symbol.error().format());
        const std::uint32_t label = *label_symbol;
        // f1 at 0: lea rdi,[__ImageBase]; mov eax,[rdi+rax*4+$LN4-table]; mov byte ptr g_tick[rbx+rdi],1; add rax,rdi; jmp rax
        put(o, text, 0, {0x48, 0x8D, 0x3D, 0, 0, 0, 0});
        o.reloc(text, 3, base, kRel32);
        put(o, text, 7, {0x8B, 0x84, 0x87, 0, 0, 0, 0});
        o.reloc(text, 10, label, kAddr32Nb, 0x10);  // the table load: patch-local, left to the linker
        put(o, text, 14, {0xC6, 0x84, 0x3B, 0, 0, 0, 0, 0x01});
        o.reloc(text, 17, tick, kAddr32Nb);
        *f1_site = 17;
        put(o, text, 22, {0x48, 0x03, 0xC7, 0xFF, 0xE0, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC});  // ... $LN4 at 0x1B: ret
        // The jump table at 0x20: two entries.
        o.reloc(text, 0x20, label, kAddr32Nb);
        o.reloc(text, 0x24, label, kAddr32Nb);
        *behind_site = 0;
        if (behind_table) {
            // A reference to old data right behind the table, still inside f1.
            put(o, text, 0x28, {0x8B, 0x84, 0x87, 0, 0, 0, 0, 0xC3});
            o.reloc(text, 0x2B, total, kAddr32Nb);
            *behind_site = 0x2B;
        }
        // f2 at 0x30.
        if (second_symbol) {
            o.symbol("?second@@YAHXZ", 0x30, static_cast<std::int32_t>(text), true);
        }
        put(o, text, 0x30, {0x48, 0x8D, 0x3D, 0, 0, 0, 0});
        o.reloc(text, 0x33, base, kRel32);
        put(o, text, 0x37, {0x8B, 0x84, 0x8F, 0, 0, 0, 0, 0xC3});
        o.reloc(text, 0x3A, total, kAddr32Nb, 8);
        *f2_site = 0x3A;
        coff::ModelSection& s = o.model().sections[text - 1];
        for (std::size_t i = 0x3F; i < s.data.size(); ++i) {
            s.data[i] = std::byte{0xCC};
        }
    };
    {
        SynthObj o;
        std::uint32_t text = 0, f1 = 0, f2 = 0, behind = 0;
        build(false, true, o, &text, &f1, &f2, &behind);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE_MESSAGE(r.has_value(), err(r));
        CHECK(r->imgrel_sites == 2);
        CHECK(r->imgrel_verified == 2);
        const Fixup* a = fixup_at(*r, text, f1);
        REQUIRE(a != nullptr);
        CHECK(a->kind == FixupKind::Addr32NbDisp);
        CHECK(a->symbol == "?g_tick@@3HA");
        const Fixup* c = fixup_at(*r, text, f2);
        REQUIRE(c != nullptr);
        CHECK(c->kind == FixupKind::Addr32NbDisp);
        CHECK(c->symbol == "?total@Accum@@2HA");
        CHECK(c->addend == 8);
        // The table load and the table entries are the patch's own business.
        CHECK(fixup_at(*r, text, 10) == nullptr);
        CHECK(fixup_at(*r, text, 0x20) == nullptr);
    }
    {
        // The same object with a reference behind the jump table: the sweep from f1 crosses the table to get there.
        SynthObj o;
        std::uint32_t text = 0, f1 = 0, f2 = 0, behind = 0;
        build(true, true, o, &text, &f1, &f2, &behind);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        BindPlan partial;
        auto r = plan_bind(w.input(b), &errors, &partial);
        REQUIRE(!r.has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAddr32NbToOldData);
        CHECK(contains(errors[0].message, "?total@Accum@@2HA"));
        CHECK(contains(errors[0].message, "+0x2b"));
        CHECK(contains(errors[0].message, "the relocation at +0x20 does not fit"));
        CHECK(partial.imgrel_sites == 3);
        CHECK(partial.imgrel_verified == 2);
    }
    {
        // Without a function symbol for f2 its site can only be reached from f1, through the table. Refused.
        SynthObj o;
        std::uint32_t text = 0, f1 = 0, f2 = 0, behind = 0;
        build(false, false, o, &text, &f1, &f2, &behind);
        Built b = o.build();
        std::vector<cml::Diag> errors;
        auto r = plan_bind(w.input(b), &errors);
        REQUIRE(!r.has_value());
        REQUIRE(with_code(errors, diag::kAddr32NbToOldData) != nullptr);
        CHECK(contains(with_code(errors, diag::kAddr32NbToOldData)->message, "+0x3a"));
    }
}

TEST_CASE("imgrel plan: a relocation straddling an instruction boundary before the site is CML3101") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t note = o.undefined("fixture_note", true);
    put(o, f.section, 0, {0xB8, 0, 0, 0, 0, 0x8B, 0x84, 0x87, 0, 0, 0, 0, 0xC3});
    o.reloc(f.section, 3, note, kRel32);       // covers the last two bytes of `mov eax, imm32` and the first two of the next
    o.reloc(f.section, 8, tick, kAddr32Nb);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    const cml::Diag* d = with_code(errors, diag::kAddr32NbToOldData);
    REQUIRE(d != nullptr);
    CHECK(contains(d->message, "the relocation at +0x3 does not fit"));
    CHECK(contains(d->message, "b8 00 00 00 00"));
}

TEST_CASE("imgrel plan: a truncating lea and an operand without a base register are CML3101") {
    {
        OneSite s({0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 3);  // lea eax, g_tick[rdi+rcx*4]
        Built b = s.o.build();
        std::vector<cml::Diag> errors;
        REQUIRE(!plan_bind(s.w.input(b), &errors).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAddr32NbToOldData);
        CHECK(contains(errors[0].message, "lea with a 32-bit or 16-bit destination"));
        CHECK(contains(errors[0].message, "8d 84 8f 00 00 00 00"));
    }
    {
        OneSite s({0x8B, 0x04, 0x25, 0, 0, 0, 0}, 3);  // mov eax, dword ptr [g_tick@IMGREL]
        Built b = s.o.build();
        std::vector<cml::Diag> errors;
        REQUIRE(!plan_bind(s.w.input(b), &errors).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAddr32NbToOldData);
        CHECK(contains(errors[0].message, "no base register"));
        CHECK(contains(errors[0].message, "8b 04 25 00 00 00 00"));
    }
    {
        OneSite s({0x8B, 0x04, 0x85, 0, 0, 0, 0}, 3);  // mov eax, g_tick[rax*4]
        Built b = s.o.build();
        std::vector<cml::Diag> errors;
        REQUIRE(!plan_bind(s.w.input(b), &errors).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAddr32NbToOldData);
        CHECK(contains(errors[0].message, "no base register"));
    }
    {
        OneSite s({0x48, 0x8D, 0x84, 0x8F, 0, 0, 0, 0}, 4, 0x20);  // lea rax, g_tick[rdi+rcx*4+20h]: the full address
        Built b = s.o.build();
        auto r = plan_bind(s.w.input(b));
        REQUIRE_MESSAGE(r.has_value(), err(r));
        const Fixup* fx = fixup_at(*r, s.f.section, s.site);
        REQUIRE(fx != nullptr);
        CHECK(fx->kind == FixupKind::Addr32NbDisp);
        CHECK(fx->addend == 0x20);
    }
}

TEST_CASE("imgrel plan: a relocation over opcode bytes of an earlier instruction refuses the site behind it") {
    SynthWorld w = basic_world();
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t tick = o.undefined("?g_tick@@3HA", false);
    const std::uint32_t note = o.undefined("fixture_note", true);
    // add r8w, imm16 (66 41 81 c0 iw), then the load. A REL32 at +2 ends with the first instruction but covers 81 c0.
    put(o, f.section, 0, {0x66, 0x41, 0x81, 0xC0, 0, 0, 0x8B, 0x84, 0x87, 0, 0, 0, 0, 0xC3});
    o.reloc(f.section, 2, note, kRel32, 0xC081);  // the in-place addend IS the bytes 81 c0 00 00
    o.reloc(f.section, 9, tick, kAddr32Nb);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    const cml::Diag* d = with_code(errors, diag::kAddr32NbToOldData);
    REQUIRE(d != nullptr);
    CHECK(contains(d->message, "the relocation at +0x2 does not fit"));
    CHECK(contains(d->message, "66 41 81 c0 00 00"));
}

TEST_CASE("imgrel plan: a VEX-encoded load of old data is retargeted (the decoder knows VEX since WP-38)") {
    OneSite s({0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 5, 0x444);  // vmovss xmm0, dword ptr g_tick[rdi+rax*4+444h]
    Built b = s.o.build();
    auto r = plan_bind(s.w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(r->imgrel_sites == 1);
    CHECK(r->imgrel_verified == 1);
    const Fixup* fx = fixup_at(*r, s.f.section, s.site);
    REQUIRE(fx != nullptr);
    CHECK(fx->kind == FixupKind::Addr32NbDisp);
    CHECK(fx->addend == 0x444);
    {
        OneSite gs({0x65, 0xC5, 0xFA, 0x10, 0x84, 0x87, 0, 0, 0, 0}, 6);  // the same through gs:
        Built bg = gs.o.build();
        std::vector<cml::Diag> errors;
        REQUIRE(!plan_bind(gs.w.input(bg), &errors).has_value());
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].code == diag::kAddr32NbToOldData);
        CHECK(contains(errors[0].message, "fs/gs"));
        CHECK(contains(errors[0].message, "65 c5 fa 10 84 87"));
    }
}

TEST_CASE("imgrel plan: ADDR32NB to a thunk and to a synthesized import slot stay with the linker") {
    SynthWorld w = basic_world();
    InMemoryProcessModules mods;
    mods.add_export("kernel32.dll", "Beep", 0x7FFB00001000ull, true);
    w.modules = &mods;
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t note = o.undefined("fixture_note", true);
    const std::uint32_t imp = o.undefined("__imp_Beep", false);
    put(o, f.section, 0, {0x48, 0x8D, 0x84, 0x0F, 0, 0, 0, 0});  // lea rax, fixture_note[rdi+rcx]: through the thunk
    o.reloc(f.section, 4, note, kAddr32Nb);
    put(o, f.section, 8, {0xB8, 0, 0, 0, 0, 0xC3});              // mov eax, __imp_Beep@IMGREL: the placeholder is patch-local
    o.reloc(f.section, 9, imp, kAddr32Nb);
    Built b = o.build();
    auto r = plan_bind(w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    CHECK(r->imgrel_sites == 0);
    CHECK(fixup_at(*r, f.section, 4) == nullptr);
    CHECK(fixup_at(*r, f.section, 9) == nullptr);
    CHECK(find(*r, "__imp_Beep")->source == BindSource::SynthesizedImportSlot);
}

TEST_CASE("imgrel plan: old data no patch base can reach image-relatively is CML3550") {
    SynthWorld w = basic_world();
    InMemoryProcessModules mods;
    mods.add_export("other.dll", "far_counter", 0x7FFB00002000ull, false);
    w.modules = &mods;
    SynthObj o;
    auto f = o.function("?compute_value@@YAHXZ", true, 0x20);
    const std::uint32_t d = o.undefined("far_counter", false);
    put(o, f.section, 0, {0x8B, 0x84, 0x87, 0, 0, 0, 0, 0xC3});
    o.reloc(f.section, 3, d, kAddr32Nb);
    Built b = o.build();
    std::vector<cml::Diag> errors;
    auto r = plan_bind(w.input(b), &errors);
    REQUIRE(!r.has_value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == diag::kImgrelOutOfRange);
    CHECK(errors[0].id() == "CML3550");
    CHECK(contains(errors[0].message, "far_counter"));
    CHECK(contains(errors[0].message, "compute_value"));
    CHECK_FALSE(errors[0].what_to_do.empty());
}

// ---------------------------------------------------------------------------------------------
// Triples and the wire
// ---------------------------------------------------------------------------------------------

TEST_CASE("imgrel triples: value = int32(S + A - patch_base), below and above the module; CML3550 when it does not fit") {
    OneSite s({0x80, 0xBC, 0x3B, 0, 0, 0, 0, 0x00}, 3, 0x200);
    Built b = s.o.build();
    auto r = plan_bind(s.w.input(b));
    REQUIRE_MESSAGE(r.has_value(), err(r));
    const std::uint64_t target = SynthWorld::kBase + 0x3080 + 0x200;
    const auto triple = [&](std::uint64_t patch_base) {
        PatchLayout l;
        l.patch_base = patch_base;
        l.section_rva[s.f.section] = 0x5000;
        return make_fixup_triples(*r, l);
    };
    {
        // The patch above the module: a negative displacement.
        const std::uint64_t base = 0x180000000ull;
        auto t = triple(base);
        REQUIRE_MESSAGE(t.has_value(), err(t));
        REQUIRE(t->size() == 1);
        CHECK((*t)[0].rva == 0x5000 + s.site);
        CHECK((*t)[0].kind == FixupKind::Addr32NbDisp);
        const std::int64_t disp = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(base);
        CHECK(disp < 0);
        CHECK((*t)[0].value == static_cast<std::uint32_t>(static_cast<std::int32_t>(disp)));
        CHECK(static_cast<std::int64_t>(base) + static_cast<std::int32_t>(static_cast<std::uint32_t>((*t)[0].value)) == static_cast<std::int64_t>(target));

        // The wire carries the absolute target; the agent's arithmetic gives the same displacement back.
        auto wire = to_patch_fixups(*t, base);
        REQUIRE_MESSAGE(wire.has_value(), err(wire));
        REQUIRE(wire->wire.size() == 1);
        CHECK(wire->pre_applied.empty());
        CHECK(wire->wire[0].kind == cml::ipc::FixupKind::Addr32NbDisp);
        CHECK(wire->wire[0].n == 0);
        CHECK(wire->wire[0].rva == 0x5000 + s.site);
        CHECK(wire->wire[0].value == target);
        std::int32_t applied = 0;
        REQUIRE(cml::ipc::addr32nb_displacement(base, wire->wire[0].value, &applied));
        CHECK(static_cast<std::uint32_t>(applied) == (*t)[0].value);
    }
    {
        // The patch below the module: positive.
        const std::uint64_t base = 0x100000000ull;
        auto t = triple(base);
        REQUIRE_MESSAGE(t.has_value(), err(t));
        CHECK((*t)[0].value == target - base);
        auto wire = to_patch_fixups(*t, base);
        REQUIRE_MESSAGE(wire.has_value(), err(wire));
        CHECK(wire->wire[0].value == target);
    }
    {
        // Exactly at the limits.
        auto lo = triple(target - 0x7FFFFFFFull);
        REQUIRE_MESSAGE(lo.has_value(), err(lo));
        CHECK((*lo)[0].value == 0x7FFFFFFFu);
        auto hi = triple(target + 0x80000000ull);
        REQUIRE_MESSAGE(hi.has_value(), err(hi));
        CHECK((*hi)[0].value == 0x80000000u);
    }
    for (const std::uint64_t base : {target - 0x80000000ull, target + 0x80000001ull, 0x7FF000000000ull}) {
        auto t = triple(base);
        REQUIRE(!t.has_value());
        CHECK(t.error().code == diag::kImgrelOutOfRange);
        CHECK(t.error().id() == "CML3550");
        CHECK(contains(t.error().message, "?g_tick@@3HA"));
        CHECK(contains(t.error().message, "32 bits"));
    }
}
