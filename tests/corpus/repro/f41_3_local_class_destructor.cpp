// WP-41, finding F-41.3 (clang-cl, CML3214): the implicit virtual destructor of a local class defined in a member function
// (Jolt Physics v5.7.0, Jolt/Physics/PhysicsSystem.cpp, PhysicsSystem::ProcessBodyPair: `class
// ReductionCollideShapeCollector : public CollideShapeCollector`, whose base declares `virtual ~CollisionCollector() =
// default;` and which has a member with a non-trivial destructor). The old compiland has two procedure records with the
// destructor's name and signature, the recompiled object one.
//
// Reduced from the corpus diagnostic, NOT yet run on Windows (the corpus host is Linux): build with clang-cl and the flags
// of docs/BUILD_REQUIREMENTS.md, start under `cml_broker --headless`, change the marked line and `reload`.
// Expected today (corpus, clang-cl, e43d5c7, run 45524632801, job 138906069101):
//   CML3214 ambiguous overloads of static 'JPH::PhysicsSystem::ProcessBodyPair::ReductionCollideShapeCollector::
//   ~ReductionCollideShapeCollector' (??1ReductionCollideShapeCollector@?1??ProcessBodyPair@PhysicsSystem@JPH@@...@UEAA@XZ)
//   in unity_12_cxx.obj; cannot tell which old function each new one replaces (1 in the patch, 2 records named
//   '~ReductionCollideShapeCollector' in compiland 4): 1 new and 2 old functions have the same signature
// What to find out first: what the second old record is (cv dump of the old compiland), e.g. a second emission of the
// destructor for the scalar deleting destructor, or a copy from another place.
#include <cstdio>
#include <thread>
#include <chrono>
#include <string>

struct Collector {
    virtual ~Collector() = default;
    virtual void AddHit(int value) = 0;
};

struct System {
    int sum = 0;
    void Process(int value);
};

void System::Process(int value) {
    class LocalCollector : public Collector {
    public:
        explicit LocalCollector(System* system) : mSystem(system) {}
        void AddHit(int v) override { mSystem->sum += v; mNames += 'x'; }
    private:
        System* mSystem;
        std::string mNames;   // a member with a non-trivial destructor
    };
    LocalCollector collector(this);
    collector.AddHit(value);
}

int edit_me() {
    return 1;   // edit: return 2; then reload
}

int main() {
    System system;
    for (;;) {
        system.Process(edit_me());
        std::printf("REPRO value=%d sum=%d\n", edit_me(), system.sum);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
