#include <cstdio>
#include <vector>
#include <string>

#include "cml/core/bind_plan.h"

namespace cml::core::bind {

namespace {

// A tiny deterministic JSON emitter: objects keep insertion order, arrays/objects are empty-safe.
class Json {
public:
    explicit Json(bool pretty) : pretty_(pretty) {}

    void begin_object() { open('{'); }
    void end_object() { close('}'); }
    void begin_array() { open('['); }
    void end_array() { close(']'); }

    void key(const char* k) {
        separator();
        append_string(k);
        out_ += pretty_ ? ": " : ":";
        after_key_ = true;
    }
    void str(const std::string& v) {
        value_prefix();
        append_string(v);
    }
    void u64(std::uint64_t v) {
        value_prefix();
        out_ += std::to_string(v);
    }
    void i64(std::int64_t v) {
        value_prefix();
        out_ += std::to_string(v);
    }
    void boolean(bool v) {
        value_prefix();
        out_ += v ? "true" : "false";
    }

    void field(const char* k, const std::string& v) {
        key(k);
        str(v);
    }
    void field(const char* k, const char* v) {
        key(k);
        str(v);
    }
    void field_u(const char* k, std::uint64_t v) {
        key(k);
        u64(v);
    }
    void field_i(const char* k, std::int64_t v) {
        key(k);
        i64(v);
    }
    void field_b(const char* k, bool v) {
        key(k);
        boolean(v);
    }

    std::string take() {
        if (pretty_) {
            out_ += '\n';
        }
        return std::move(out_);
    }

private:
    void newline() {
        if (pretty_) {
            out_ += '\n';
            out_.append(depth_ * 2, ' ');
        }
    }
    void separator() {
        if (!first_.empty()) {
            if (!first_.back()) {
                out_ += ',';
            }
            first_.back() = false;
            newline();
        }
    }
    void value_prefix() {
        if (after_key_) {
            after_key_ = false;
        } else {
            separator();
        }
    }
    void open(char c) {
        value_prefix();
        out_ += c;
        first_.push_back(true);
        ++depth_;
    }
    void close(char c) {
        const bool empty = first_.back();
        first_.pop_back();
        --depth_;
        if (!empty) {
            newline();
        }
        out_ += c;
    }
    void append_string(const std::string& s) {
        out_ += '"';
        for (const char ch : s) {
            const unsigned char c = static_cast<unsigned char>(ch);
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out_ += buf;
                    } else {
                        out_ += ch;
                    }
            }
        }
        out_ += '"';
    }

    bool pretty_;
    bool after_key_ = false;
    std::size_t depth_ = 0;
    std::vector<bool> first_;
    std::string out_;
};

void target_fields(Json& j, const TargetAddress& t, const JsonOptions& o) {
    if (!t.valid || !o.addresses) {
        return;
    }
    if (!t.module.empty()) {
        j.field("module", t.module);
    }
    if (t.rva != kNoRva) {
        j.field_u("rva", t.rva);
    }
    j.field_u("va", t.va);
}

}  // namespace

std::string to_json(const BindPlan& plan, const JsonOptions& o) {
    Json j(o.pretty);
    j.begin_object();
    j.field_u("version", 1);
    j.field("object", plan.object_name);
    j.field("module", plan.module_name);
    j.field("compiland", plan.compiland);
    if (o.addresses) {
        j.field_u("module_base", plan.module_base);
    }

    const PlanSummary s = plan.summary();
    j.key("summary");
    j.begin_object();
    j.field_u("hooks", s.hooks);
    j.field_u("bind_old_data", s.bind_old_data);
    j.field_u("thunks", s.thunks);
    j.field_u("new", s.new_symbols);
    j.field_u("fixups", s.fixups);
    j.end_object();

    j.key("decisions");
    j.begin_array();
    for (const SymbolDecision& d : plan.decisions) {
        j.begin_object();
        j.field("symbol", d.name);
        j.field("display", d.display);
        j.field("type", to_string(d.type));
        j.field_b("defined", d.defined);
        j.field_b("static", d.is_static);
        if (d.defined) {
            j.field("section", d.section_name);
        }
        j.field("decision", to_string(d.decision));
        j.field("source", to_string(d.source));
        target_fields(j, d.old_address, o);
        if (o.reasons) {
            j.field("reason", d.reason);
        }
        j.end_object();
    }
    j.end_array();

    j.key("hooks");
    j.begin_array();
    for (const HookEntry& h : plan.hooks) {
        j.begin_object();
        j.field("new_symbol", h.new_symbol);
        j.field("display", h.display);
        if (o.addresses) {
            if (h.old_rva != kNoRva) {
                j.field_u("old_rva", h.old_rva);
            }
            j.field_u("old_va", h.old_va);
            j.field_u("old_size", h.old_size);
        }
        j.field("source", to_string(h.source));
        j.end_object();
    }
    j.end_array();

    j.key("thunks");
    j.begin_array();
    for (const ThunkEntry& t : plan.thunks) {
        j.begin_object();
        j.field("name", t.name);
        j.field("display", t.display);
        j.field("slot", t.slot);
        j.field("source", to_string(t.source));
        target_fields(j, t.target, o);
        j.end_object();
    }
    j.end_array();

    j.key("placeholders");
    j.begin_array();
    for (const PlaceholderEntry& p : plan.placeholders) {
        j.begin_object();
        j.field("name", p.name);
        j.field("display", p.display);
        j.field("source", to_string(p.source));
        j.field_b("synthesized_import_slot", p.is_synthesized_slot);
        target_fields(j, p.target, o);
        j.end_object();
    }
    j.end_array();

    j.key("new_functions");
    j.begin_array();
    for (const NewFunction& n : plan.new_functions) {
        j.begin_object();
        j.field("name", n.name);
        j.field("display", n.display);
        j.field_b("static", n.is_static);
        j.end_object();
    }
    j.end_array();

    j.key("new_globals");
    j.begin_array();
    for (const NewGlobal& n : plan.new_globals) {
        j.begin_object();
        j.field("name", n.name);
        j.field("display", n.display);
        j.field_b("static", n.is_static);
        j.key("initialisers");
        j.begin_array();
        for (const std::string& i : n.initialisers) {
            j.str(i);
        }
        j.end_array();
        j.end_object();
    }
    j.end_array();

    j.key("initialisers");
    j.begin_array();
    for (const InitialiserEntry& e : plan.initialisers) {
        j.begin_object();
        j.field("function", e.function);
        j.field("display", e.display);
        j.field("table_section", e.table_section_name);
        j.field_u("table_offset", e.table_offset);
        j.field_b("tu_wide", e.tu_wide);
        j.key("globals");
        j.begin_array();
        for (const std::string& g : e.globals) {
            j.str(g);
        }
        j.end_array();
        j.end_object();
    }
    j.end_array();

    j.key("fixups");
    j.begin_array();
    for (const Fixup& f : plan.fixups) {
        j.begin_object();
        j.field("kind", to_string(f.kind));
        if (f.kind != FixupKind::Slot) {
            j.field_u("section", f.section_index);
            j.field("section_name", f.section_name);
            j.field_u("offset", f.offset);
            j.field_i("addend", f.addend);
        }
        j.field("symbol", f.symbol);
        if (o.addresses) {
            if (f.target_rva != kNoRva) {
                j.field_u("target_rva", f.target_rva);
            }
            if (f.thunk_fallback) {
                j.field_b("thunk_fallback", true);
            }
            if (f.kind == FixupKind::SecRel32) {
                j.field_u("target_section_offset", f.target_section_offset);
            }
            j.field_u("target_va", f.target_va);
        }
        j.end_object();
    }
    j.end_array();

    j.key("locator_sections");
    j.begin_array();
    for (const std::uint32_t sec : plan.locator_sections) {
        j.u64(sec);
    }
    j.end_array();
    j.key("associative_locator_sections");
    j.begin_array();
    for (const std::uint32_t sec : plan.associative_locator_sections) {
        j.u64(sec);
    }
    j.end_array();

    j.key("warnings");
    j.begin_array();
    for (const Diag& d : plan.warnings) {
        j.begin_object();
        j.field("id", d.id());
        j.field("message", d.message);
        j.end_object();
    }
    j.end_array();

    j.end_object();
    return j.take();
}

std::string to_json(std::span<const FixupTriple> triples) {
    Json j(true);
    j.begin_array();
    for (const FixupTriple& t : triples) {
        j.begin_object();
        j.field_u("rva", t.rva);
        j.field("kind", to_string(t.kind));
        j.field_u("value", t.value);
        j.end_object();
    }
    j.end_array();
    return j.take();
}

}  // namespace cml::core::bind
