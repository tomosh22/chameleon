// The prototype patch DLL of unit.agent_loader (WP-08).
//
// Linked `/NOENTRY /NODEFAULTLIB /FIXED /DYNAMICBASE:NO /BASE:0x6600000000` with no imports and
// no CRT, like a real patch: LoadLibrary runs no code. The tests copy the file and rewrite the
// ImageBase in the PE header, which is a valid way to place it at any 64 KiB aligned address
// because /FIXED images carry no relocations and this code is position independent (RIP-relative
// only). The one thing that is NOT position independent is the table of initializer pointers
// (absolute addresses, like .CRT$XCU); the tests rewrite those with ADDR64 fix-ups, which is
// also exactly what the agent's fix-up engine is for.
//
// Everything the tests need is exported by name:
//   cml_code_call     bytes in an execute-only-ish section: sub rsp,28h / call rel32 / add rsp,28h / ret.
//                     The rel32 sits at offset 5 and is zero until a REL32 fix-up points it somewhere.
//   cml_data          256 writable bytes, sites for REL32/ADDR64 fix-ups
//   cml_rodata        64 read-only bytes, a site in a read-only page
//   cml_slot_a/_b     two 8-byte thunk slots
//   cml_inits         three initializer pointers (table in a read-only section)
//   cml_init_fault    one initializer pointer whose function faults
//   cml_log/_len      what the initializers wrote
//   cml_patch_answer  a plain function returning 42

extern "C" {

// Supplies the symbol a /JMC build (Visual Studio generator) references; harmless otherwise.
void __CheckForDebuggerJustMyCode(unsigned char*) {}

__declspec(dllexport) volatile unsigned long long cml_slot_a = 0x1111111111111111ull;
__declspec(dllexport) volatile unsigned long long cml_slot_b = 0x2222222222222222ull;
__declspec(dllexport) unsigned char cml_data[256] = {1, 2, 3};
__declspec(dllexport) volatile unsigned long long cml_log_len = 0;
__declspec(dllexport) volatile char cml_log[16] = {};

__declspec(dllexport) int cml_patch_answer(void) {
    return 42;
}

}  // extern "C"

extern "C" __declspec(dllexport) const unsigned char cml_rodata[64] = {9, 8, 7};

#pragma section(".cmlx", read, execute)
extern "C" __declspec(allocate(".cmlx")) __declspec(dllexport) const unsigned char cml_code_call[16] = {
    0x48, 0x83, 0xEC, 0x28,  // sub rsp, 28h
    0xE8, 0x00, 0x00, 0x00, 0x00,  // call rel32   (offset 5)
    0x48, 0x83, 0xC4, 0x28,  // add rsp, 28h
    0xC3,                    // ret
    0xCC, 0xCC};

namespace {

void note(char c) {
    cml_log[cml_log_len] = c;
    cml_log_len = cml_log_len + 1;
}
void init_a() { note('A'); }
void init_b() { note('B'); }
void init_c() { note('C'); }
void init_fault() {
    *reinterpret_cast<volatile int*>(0) = 1;  // access violation, caught by the agent
}

}  // namespace

#pragma section(".cmlinit", read)
extern "C" __declspec(allocate(".cmlinit")) __declspec(dllexport) void (*const cml_inits[3])() = {init_a, init_b, init_c};
extern "C" __declspec(allocate(".cmlinit")) __declspec(dllexport) void (*const cml_init_fault[1])() = {init_fault};
