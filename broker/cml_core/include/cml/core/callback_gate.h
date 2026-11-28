#pragma once

// WP-62: the end of the callbacks an object hands to threads it does not own (CallbackGate).
//
// An object that gives a callback capturing `this` to another thread (a module index's worker, say) cannot rely on reference counts to
// outlive it: whoever still holds the other side keeps that thread running after the object is gone. The object keeps a
// std::shared_ptr<CallbackGate>, each callback holds a copy and runs its body only inside a Scope, and the object's destructor calls
// close() before it tears anything down. close() waits for the callbacks in flight to return; from then on a Scope is closed and the
// callback does nothing, whoever still holds it.
//
// close() must not be called from inside a Scope of the same gate (it would wait for itself).

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace cml::core {

class CallbackGate {
public:
    class Scope {
    public:
        explicit Scope(CallbackGate& gate) : gate_(&gate) {
            std::lock_guard<std::mutex> lock(gate.mutex_);
            if (gate.closed_) {
                gate_ = nullptr;
            } else {
                ++gate.in_flight_;
            }
        }
        ~Scope() {
            if (gate_ != nullptr) {
                std::lock_guard<std::mutex> lock(gate_->mutex_);
                if (--gate_->in_flight_ == 0) {
                    gate_->idle_.notify_all();
                }
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        // The owner is alive until this scope ends.
        explicit operator bool() const noexcept { return gate_ != nullptr; }

    private:
        CallbackGate* gate_;
    };

    // No Scope opens after this returns, and every Scope that was open has ended.
    void close() {
        std::unique_lock<std::mutex> lock(mutex_);
        closed_ = true;
        idle_.wait(lock, [this] { return in_flight_ == 0; });
    }
    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    std::size_t in_flight_ = 0;
    bool closed_ = false;
};

}  // namespace cml::core
