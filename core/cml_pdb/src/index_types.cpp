// WP-37: the type streams of a PdbIndex (lazy, sparse; detail/type_stream.h) and TypeServerPdb, the compiler PDB of an
// object compiled with /Zi, opened for its types only.

#include <cstring>

#include "cml/pdb/diag_ids.h"
#include "cml/util/log.h"
#include "index_impl.h"

namespace cml::pdb {

using namespace detail;

namespace {

constexpr std::uint32_t kTpiStream = 2;
constexpr std::uint32_t kIpiStream = 4;

struct PdbTypeSource final : TypeSource {
    explicit PdbTypeSource(const PdbIndex::Impl& i) : impl(i) {}
    bool find_type(std::uint32_t index, TypeRecord& out) const override {
        const TypeStream* s = impl.type_stream(true);
        return s != nullptr && s->find(index, out);
    }
    bool find_id(std::uint32_t index, TypeRecord& out) const override {
        const TypeStream* s = impl.type_stream(false);
        return s != nullptr && s->find(index, out);
    }
    const PdbIndex::Impl& impl;
};

FunctionSignature unreadable(const Diag& d) {
    FunctionSignature s;
    s.diag_id = d.code;
    s.why = d.message;
    return s;
}

}  // namespace

const TypeStream* PdbIndex::Impl::type_stream(bool want_tpi, Diag* error) const {
    const int slot = want_tpi ? 1 : 0;
    std::lock_guard<std::mutex> lock(type_stream_mutex);
    if (!type_stream_tried[slot]) {
        type_stream_tried[slot] = true;
        Result<std::unique_ptr<TypeStream>> opened = TypeStream::open(*msf, want_tpi ? kTpiStream : kIpiStream, path, want_tpi ? "TPI" : "IPI");
        if (opened) {
            type_streams[slot] = std::move(opened).value();
            const TypeStreamStats st = type_streams[slot]->stats();
            CML_LOG(Debug, "cml_pdb: '%s': %s stream opened for types: %llu record bytes, %u chunk(s), %s", path.c_str(), want_tpi ? "TPI" : "IPI",
                    static_cast<unsigned long long>(st.record_bytes), st.chunks, st.sparse ? "sparse (type-index offset table)" : "one chunk (no usable offset table)");
        } else {
            type_stream_errors[slot] = opened.error();
            CML_LOG(Debug, "cml_pdb: %s", opened.error().format().c_str());
        }
    }
    if (type_streams[slot] == nullptr && error != nullptr && type_stream_errors[slot]) {
        *error = *type_stream_errors[slot];
    }
    return type_streams[slot].get();
}

const TypeSource& PdbIndex::types() const {
    std::lock_guard<std::mutex> lock(impl_->type_stream_mutex);
    if (impl_->type_source == nullptr) {
        impl_->type_source = std::make_unique<PdbTypeSource>(*impl_);
    }
    return *impl_->type_source;
}

FunctionSignature PdbIndex::proc_signature(const SymbolView& proc) const {
    if (!is_proc_kind(proc.kind)) {
        FunctionSignature s;
        s.diag_id = diag::kBadTypeRecord;
        s.why = "'" + std::string(proc.name) + "' is not a procedure symbol";
        return s;
    }
    Diag error;
    if (impl_->type_stream(true, &error) == nullptr) {
        return unreadable(error);
    }
    if (proc.type_is_id && impl_->type_stream(false, &error) == nullptr) {
        return unreadable(error);
    }
    return function_signature(types(), proc.type_index, proc.type_is_id);
}

TypeStreamStats PdbIndex::type_stream_stats(bool tpi) const {
    const int slot = tpi ? 1 : 0;
    std::lock_guard<std::mutex> lock(impl_->type_stream_mutex);
    return impl_->type_streams[slot] != nullptr ? impl_->type_streams[slot]->stats() : TypeStreamStats();
}

// ---- TypeServerPdb ---------------------------------------------------------------------------------------------------

struct TypeServerPdb::Impl {
    std::shared_ptr<const MappedFile> file;
    std::unique_ptr<Msf> msf;
    PdbId id;
    std::string path;
    std::unique_ptr<TypeStream> tpi;
    std::unique_ptr<TypeStream> ipi;  // absent in PDBs written by old compilers
};

TypeServerPdb::TypeServerPdb(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TypeServerPdb::~TypeServerPdb() = default;

Result<std::unique_ptr<TypeServerPdb>> TypeServerPdb::open(const std::string& pdb_path) {
    Result<PdbProbe> probe = probe_pdb(pdb_path);
    if (!probe) {
        return probe.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->file = std::move(probe->file);
    impl->msf = std::move(probe->msf);
    impl->id.guid = probe->id.guid;
    impl->id.age = probe->info_age;  // LF_TYPESERVER2 records the age of the PDB info stream
    impl->path = pdb_path;
    Result<std::unique_ptr<TypeStream>> tpi = TypeStream::open(*impl->msf, kTpiStream, pdb_path, "TPI");
    if (!tpi) {
        return tpi.error();
    }
    impl->tpi = std::move(tpi).value();
    Result<std::unique_ptr<TypeStream>> ipi = TypeStream::open(*impl->msf, kIpiStream, pdb_path, "IPI");
    if (ipi) {
        impl->ipi = std::move(ipi).value();
    } else {
        CML_LOG(Debug, "cml_pdb: type server '%s' has no usable IPI stream: %s", pdb_path.c_str(), ipi.error().format().c_str());
    }
    return std::unique_ptr<TypeServerPdb>(new TypeServerPdb(std::move(impl)));
}

bool TypeServerPdb::find_type(std::uint32_t index, TypeRecord& out) const {
    return impl_->tpi->find(index, out);
}

bool TypeServerPdb::find_id(std::uint32_t index, TypeRecord& out) const {
    return impl_->ipi != nullptr && impl_->ipi->find(index, out);
}

const PdbId& TypeServerPdb::id() const noexcept {
    return impl_->id;
}

const std::string& TypeServerPdb::path() const noexcept {
    return impl_->path;
}

}  // namespace cml::pdb
