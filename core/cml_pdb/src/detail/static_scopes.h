#pragma once

// WP-37 (ADR-0042, amended): where inside a procedure a data record sits. Used for both sides of the planner's
// comparison of same-named function-local statics: the module stream of a linked PDB and the symbols of an object's
// `.debug$S`. One implementation, so "the block path" means the same on both sides.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cml::pdb::detail {

// The scopes that are open at one point of a walk over symbol records.
class ScopeStack {
public:
    enum class Kind : std::uint8_t { Proc, Block, Other };
    // Deeper nesting than this is not followed (the input is untrusted): a record found there has no usable position.
    static constexpr std::size_t kMaxDepth = 64;

    // `token` is the caller's name for a procedure (Kind::Proc).
    void open(Kind kind, std::uint32_t token = 0) {
        if (frames_.size() >= kMaxDepth) {
            ++overflow_;
            return;
        }
        Frame f;
        f.kind = kind;
        f.token = token;
        if (kind == Kind::Block && !frames_.empty()) {
            f.index = frames_.back().next_block++;
        }
        frames_.push_back(f);
    }
    void close() {
        if (overflow_ != 0) {
            --overflow_;
        } else if (!frames_.empty()) {
            frames_.pop_back();
        }
    }
    void clear() {
        frames_.clear();
        overflow_ = 0;
    }
    bool inside_proc() const noexcept {
        for (const Frame& f : frames_) {
            if (f.kind == Kind::Proc) {
                return true;
            }
        }
        return false;
    }
    // The innermost open procedure and the position of "here" inside it. `block_path`: for every S_BLOCK32 open between
    // the procedure and here, outermost first, its index among the blocks of its parent scope. `plain`: nothing but
    // blocks is open in between (no inline site, no S_WITH32, ...) and the nesting was followed completely. False when
    // no procedure is open.
    bool locate(std::uint32_t& token, std::vector<std::uint32_t>& block_path, bool& plain) const {
        for (std::size_t i = frames_.size(); i-- > 0;) {
            if (frames_[i].kind != Kind::Proc) {
                continue;
            }
            token = frames_[i].token;
            block_path.clear();
            plain = overflow_ == 0;
            for (std::size_t k = i + 1; k < frames_.size(); ++k) {
                if (frames_[k].kind == Kind::Block) {
                    block_path.push_back(frames_[k].index);
                } else {
                    plain = false;
                }
            }
            return true;
        }
        return false;
    }

private:
    struct Frame {
        Kind kind = Kind::Other;
        std::uint32_t token = 0;
        std::uint32_t index = 0;       // Block: its index among its parent's blocks
        std::uint32_t next_block = 0;  // how many blocks were opened directly inside this scope so far
    };
    std::vector<Frame> frames_;
    std::size_t overflow_ = 0;  // scopes opened beyond kMaxDepth and not closed yet
};

}  // namespace cml::pdb::detail
