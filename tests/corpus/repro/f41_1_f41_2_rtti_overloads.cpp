// WP-41, findings F-41.1 (cl and clang-cl, CML3207) and F-41.2 (cl, CML3214): the shape of Jolt Physics' RTTI macros
// (Jolt/Core/RTTI.h, JPH_IMPLEMENT_RTTI_VIRTUAL / JPH_IMPLEMENT_SERIALIZABLE_*). Each class gets an overload of one free
// function, GetRTTIOfType(Class *), that holds a guarded function-local static (`rtti`, guard `$TSS0`) constructed with
// two lambdas. A unity batch puts several of these overloads into one compiland (7 in Jolt's batch 8, SphereShape.cpp).
//
// Reduced from the corpus diagnostics, NOT yet run on Windows (the corpus host is Linux): build with the flags of
// docs/BUILD_REQUIREMENTS.md, start under `cml_broker --headless`, change the marked line and `reload`.
// Expected today (corpus, Jolt v5.7.0, e43d5c7, run 45524632801):
//   clang-cl, and cl when the batch has no lambda refused first:
//     CML3207 static data 'JPH::GetRTTIOfType::rtti' ... matches 7 records named 'rtti' in compiland N but the patch object
//     defines 7 of them: the statics named 'rtti' of 'JPH::GetRTTIOfType' cannot be matched by their place in the
//     function: the guard variable 'JPH::GetRTTIOfType::$TSS0' (?$TSS0@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@2@PEAV...@2@@Z@4HA)
//     of static 1 of 1 has no record in the old image
//   cl, every batch with lambdas that was edited:
//     CML3214 ambiguous overloads of static '<lambda_HASH>::operator()' ... the lambdas of the enclosing function are not
//     the same as in the old image (54 call operator(s) there, 4 in the patch)
// What to find out first (planner): whether the old image has the guard records under the decorated name (the overloads
// share the undecorated name `GetRTTIOfType`), and whether cl's <lambda_HASH> names change when another function of the
// translation unit changes.
#include <cstdio>
#include <thread>
#include <chrono>

struct Rtti {
    Rtti(const char* name, void* (*create)(), void (*destroy)(void*)) : name(name), create(create), destroy(destroy) {}
    const char* name;
    void* (*create)();
    void (*destroy)(void*);
};

struct ShapeSettings { int a = 1; };
struct SphereSettings { int b = 2; };
struct BoxSettings { int c = 3; };

Rtti* GetRTTIOfType(ShapeSettings*) {
    static Rtti rtti("ShapeSettings", []() -> void* { return new ShapeSettings; }, [](void* p) { delete (ShapeSettings*)p; });
    return &rtti;
}
Rtti* GetRTTIOfType(SphereSettings*) {
    static Rtti rtti("SphereSettings", []() -> void* { return new SphereSettings; }, [](void* p) { delete (SphereSettings*)p; });
    return &rtti;
}
Rtti* GetRTTIOfType(BoxSettings*) {
    static Rtti rtti("BoxSettings", []() -> void* { return new BoxSettings; }, [](void* p) { delete (BoxSettings*)p; });
    return &rtti;
}

int edit_me() {
    return 1;   // edit: return 2; then reload
}

int main() {
    for (;;) {
        std::printf("REPRO value=%d %s %s %s\n", edit_me(), GetRTTIOfType((ShapeSettings*)nullptr)->name,
                    GetRTTIOfType((SphereSettings*)nullptr)->name, GetRTTIOfType((BoxSettings*)nullptr)->name);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
