// Code for cml_lde_avx_module (see CMakeLists.txt in this directory): functions that make a compiler emit a wide range
// of VEX-encoded instructions (AVX, AVX2, FMA, F16C, BMI1, BMI2, AES, PCLMULQDQ as /arch:AVX2 implies them). The
// module is never executed; cml_lde_probe plans a hook for every function entry and cml_lde_diff compares every
// instruction with the reference disassembler (WP-38, ADR-0042). Compiled twice with different CML_AVX_VARIANT names:
// optimised, and with the configuration's own flags.
#include <immintrin.h>

#include <cstddef>
#include <cstdint>

#if !defined(CML_AVX_VARIANT)
#error "CML_AVX_VARIANT names the namespace of this copy"
#endif

#if defined(_MSC_VER)
#define CML_AVX_NOINLINE __declspec(noinline)
#else
#define CML_AVX_NOINLINE __attribute__((noinline))
#endif
// clang (clang-cl too) wants the AES and PCLMULQDQ features named for the function that uses them: /arch:AVX2 selects a
// processor model that does not promise them. cl needs nothing.
#if defined(__clang__) || defined(__GNUC__)
#define CML_AVX_CRYPTO __attribute__((target("aes,pclmul")))
#else
#define CML_AVX_CRYPTO
#endif

namespace cml_lde_avx {
namespace CML_AVX_VARIANT {

// ---- scalar floating point: the VEX forms of the SSE scalar instructions -------------------------------------------

CML_AVX_NOINLINE float scalar_float(float a, float b, float c) {
    float r = a * b + c;
    r = r / (b + 1.5f);
    if (r < a) {
        r = a - r;
    }
    return r > c ? r : c;
}

CML_AVX_NOINLINE double scalar_double(double a, double b, int i, long long q) {
    double r = a * static_cast<double>(i) + b / static_cast<double>(q);
    r += static_cast<double>(static_cast<float>(a));
    return r < b ? b - r : r;
}

CML_AVX_NOINLINE int scalar_convert(float a, double b) {
    const int x = static_cast<int>(a);
    const long long y = static_cast<long long>(b);
    const unsigned z = static_cast<unsigned>(a + 0.5f);
    return x + static_cast<int>(y) + static_cast<int>(z);
}

CML_AVX_NOINLINE float scalar_sqrt(float a, double b) {
    const __m128 s = _mm_sqrt_ss(_mm_set_ss(a));
    const __m128d d = _mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(b));
    const __m128 r = _mm_rsqrt_ss(s);
    const __m128 p = _mm_rcp_ss(r);
    return _mm_cvtss_f32(_mm_add_ss(p, _mm_cvtsd_ss(s, d)));
}

CML_AVX_NOINLINE int scalar_compare(float a, float b, double c, double d) {
    int r = 0;
    r += _mm_comilt_ss(_mm_set_ss(a), _mm_set_ss(b));
    r += _mm_ucomieq_sd(_mm_set_sd(c), _mm_set_sd(d));
    r += _mm_cvtss_si32(_mm_max_ss(_mm_set_ss(a), _mm_set_ss(b)));
    r += static_cast<int>(_mm_cvttsd_si64(_mm_min_sd(_mm_set_sd(c), _mm_set_sd(d))));
    return r;
}

CML_AVX_NOINLINE float scalar_fma(float a, float b, float c, double x, double y, double z) {
    const __m128 f = _mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c));
    const __m128 g = _mm_fnmsub_ss(f, _mm_set_ss(b), _mm_set_ss(a));
    const __m128d h = _mm_fmsub_sd(_mm_set_sd(x), _mm_set_sd(y), _mm_set_sd(z));
    return _mm_cvtss_f32(g) + static_cast<float>(_mm_cvtsd_f64(h));
}

// ---- loops a compiler vectorises ------------------------------------------------------------------------------------

CML_AVX_NOINLINE float loop_sum(const float* p, std::size_t n) {
    float s = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        s += p[i];
    }
    return s;
}

CML_AVX_NOINLINE void loop_saxpy(float* y, const float* x, float a, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        y[i] = a * x[i] + y[i];
    }
}

CML_AVX_NOINLINE void loop_ints(std::int32_t* out, const std::int32_t* a, const std::int32_t* b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = (a[i] + b[i]) * 3 - (a[i] >> 2);
    }
}

CML_AVX_NOINLINE std::size_t loop_count_bytes(const std::uint8_t* p, std::size_t n, std::uint8_t value) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        count += p[i] == value ? 1u : 0u;
    }
    return count;
}

CML_AVX_NOINLINE void loop_widen(double* out, const std::int16_t* in, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<double>(in[i]) * 0.25;
    }
}

CML_AVX_NOINLINE void loop_min_max(const std::int32_t* p, std::size_t n, std::int32_t* lo, std::int32_t* hi) {
    std::int32_t a = p[0];
    std::int32_t b = p[0];
    for (std::size_t i = 1; i < n; ++i) {
        a = p[i] < a ? p[i] : a;
        b = p[i] > b ? p[i] : b;
    }
    *lo = a;
    *hi = b;
}

struct Block {
    double values[16];
    std::int64_t ids[8];
};

CML_AVX_NOINLINE void copy_block(Block* to, const Block* from) {
    *to = *from;  // ymm moves
}

CML_AVX_NOINLINE void zero_block(Block* b) {
    *b = Block{};
}

// ---- 256-bit floating point -----------------------------------------------------------------------------------------

CML_AVX_NOINLINE __m256 ps_arith(const float* a, const float* b) {
    const __m256 x = _mm256_loadu_ps(a);
    const __m256 y = _mm256_load_ps(b);
    __m256 r = _mm256_add_ps(_mm256_mul_ps(x, y), _mm256_sub_ps(x, y));
    r = _mm256_div_ps(r, _mm256_sqrt_ps(_mm256_max_ps(x, _mm256_min_ps(y, r))));
    r = _mm256_and_ps(_mm256_or_ps(r, x), _mm256_xor_ps(_mm256_andnot_ps(y, r), x));
    return _mm256_addsub_ps(_mm256_hadd_ps(r, x), _mm256_hsub_ps(r, y));
}

CML_AVX_NOINLINE __m256d pd_arith(const double* a, const double* b, double* out) {
    const __m256d x = _mm256_loadu_pd(a);
    const __m256d y = _mm256_broadcast_sd(b);
    const __m256d r = _mm256_fmadd_pd(x, y, _mm256_fnmadd_pd(y, x, _mm256_set1_pd(2.0)));
    _mm256_storeu_pd(out, _mm256_round_pd(r, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
    _mm256_stream_pd(out + 4, _mm256_floor_pd(r));
    return _mm256_fmsubadd_pd(r, x, y);
}

CML_AVX_NOINLINE int ps_compare(__m256 a, __m256 b) {
    const __m256 lt = _mm256_cmp_ps(a, b, _CMP_LT_OQ);
    const __m256 eq = _mm256_cmp_ps(a, b, _CMP_EQ_UQ);
    const int m = _mm256_movemask_ps(lt) | (_mm256_movemask_pd(_mm256_castps_pd(eq)) << 8);
    return m + _mm256_testz_ps(lt, eq) + _mm256_testc_pd(_mm256_castps_pd(lt), _mm256_castps_pd(eq));
}

CML_AVX_NOINLINE __m256 ps_shuffle(__m256 a, __m256 b, __m256i control) {
    __m256 r = _mm256_shuffle_ps(a, b, 0x4E);
    r = _mm256_permute_ps(r, 0x1B);
    r = _mm256_permutevar_ps(r, control);
    r = _mm256_permute2f128_ps(r, a, 0x21);
    r = _mm256_blend_ps(r, b, 0xA5);
    r = _mm256_blendv_ps(r, a, b);
    r = _mm256_unpacklo_ps(r, _mm256_unpackhi_ps(a, b));
    r = _mm256_permutevar8x32_ps(r, control);
    return _mm256_dp_ps(r, a, 0xF1);
}

CML_AVX_NOINLINE __m256d pd_shuffle(__m256d a, __m256d b) {
    __m256d r = _mm256_shuffle_pd(a, b, 0x5);
    r = _mm256_permute_pd(r, 0x6);
    r = _mm256_permute4x64_pd(r, 0x1B);
    r = _mm256_blend_pd(r, b, 0x9);
    r = _mm256_movedup_pd(r);
    return _mm256_unpackhi_pd(r, a);
}

CML_AVX_NOINLINE __m256 ps_halves(__m256 a, __m128 lo, float* out) {
    const __m128 hi = _mm256_extractf128_ps(a, 1);
    const __m256 r = _mm256_insertf128_ps(a, _mm_add_ps(lo, hi), 1);
    _mm_store_ss(out, _mm_movehdup_ps(hi));
    _mm_storeu_ps(out + 4, _mm_moveldup_ps(lo));
    return _mm256_add_ps(_mm256_add_ps(_mm256_broadcast_ps(&lo), r), _mm256_broadcast_ss(out));
}

CML_AVX_NOINLINE __m256d convert(__m256 a, __m128i i, __m256i wide) {
    const __m256d d = _mm256_cvtps_pd(_mm256_castps256_ps128(a));
    const __m256d e = _mm256_cvtepi32_pd(i);
    const __m128 f = _mm256_cvtpd_ps(d);
    const __m256i g = _mm256_cvttps_epi32(a);
    const __m256 h = _mm256_cvtepi32_ps(_mm256_add_epi32(g, wide));
    const __m128i t = _mm256_cvttpd_epi32(e);
    return _mm256_add_pd(_mm256_cvtps_pd(_mm_add_ps(f, _mm256_castps256_ps128(h))), _mm256_cvtepi32_pd(t));
}

CML_AVX_NOINLINE __m256 half_precision(__m128i halves, __m256 a, __m128i* out) {
    const __m256 wide = _mm256_cvtph_ps(halves);
    _mm_storeu_si128(out, _mm256_cvtps_ph(_mm256_add_ps(wide, a), _MM_FROUND_TO_NEAREST_INT));
    return wide;
}

CML_AVX_NOINLINE void masked_moves(float* f, double* d, std::int32_t* i, __m256i mask, __m256 a) {
    const __m256 x = _mm256_maskload_ps(f, mask);
    _mm256_maskstore_ps(f + 8, mask, _mm256_add_ps(x, a));
    const __m256d y = _mm256_maskload_pd(d, mask);
    _mm256_maskstore_pd(d + 4, mask, y);
    const __m256i z = _mm256_maskload_epi32(i, mask);
    _mm256_maskstore_epi32(i + 8, mask, z);
}

CML_AVX_NOINLINE __m256 gathers(const float* base, const double* dbase, const std::int32_t* ibase, __m256i index, __m256 mask) {
    const __m256 a = _mm256_i32gather_ps(base, index, 4);
    const __m256 b = _mm256_mask_i32gather_ps(a, base + 16, index, mask, 4);
    const __m256d c = _mm256_i32gather_pd(dbase, _mm256_castsi256_si128(index), 8);
    const __m256i d = _mm256_i32gather_epi32(ibase, index, 4);
    const __m256i e = _mm256_i64gather_epi64(reinterpret_cast<const long long*>(dbase), index, 8);
    return _mm256_add_ps(_mm256_add_ps(b, _mm256_castpd_ps(c)), _mm256_castsi256_ps(_mm256_add_epi32(d, e)));
}

CML_AVX_NOINLINE float upper_state(__m256 a) {
    const __m128 lo = _mm256_castps256_ps128(a);
    _mm256_zeroupper();
    const float r = _mm_cvtss_f32(lo);
    _mm256_zeroall();
    return r;
}

// ---- 256-bit integer ------------------------------------------------------------------------------------------------

CML_AVX_NOINLINE __m256i int_arith(const __m256i* a, const __m256i* b) {
    const __m256i x = _mm256_loadu_si256(a);
    const __m256i y = _mm256_load_si256(b);
    __m256i r = _mm256_add_epi8(x, y);
    r = _mm256_sub_epi16(r, _mm256_adds_epu8(x, y));
    r = _mm256_add_epi32(r, _mm256_mullo_epi32(x, y));
    r = _mm256_add_epi64(r, _mm256_mul_epu32(x, y));
    r = _mm256_xor_si256(r, _mm256_and_si256(_mm256_or_si256(x, y), _mm256_andnot_si256(x, r)));
    r = _mm256_max_epi16(r, _mm256_min_epu8(x, _mm256_abs_epi32(y)));
    r = _mm256_avg_epu8(r, _mm256_sign_epi8(x, y));
    r = _mm256_madd_epi16(r, _mm256_maddubs_epi16(x, y));
    r = _mm256_mulhi_epi16(r, _mm256_mulhrs_epi16(x, y));
    r = _mm256_sad_epu8(r, _mm256_hadd_epi32(x, y));
    return _mm256_mpsadbw_epu8(r, x, 0x12);
}

CML_AVX_NOINLINE __m256i int_shift(__m256i a, __m256i counts, __m128i count) {
    __m256i r = _mm256_slli_epi32(a, 5);
    r = _mm256_srli_epi16(r, 3);
    r = _mm256_srai_epi32(r, 7);
    r = _mm256_slli_si256(r, 4);
    r = _mm256_srli_si256(r, 2);
    r = _mm256_sll_epi64(r, count);
    r = _mm256_sra_epi16(r, count);
    r = _mm256_sllv_epi32(r, counts);
    r = _mm256_srlv_epi64(r, counts);
    return _mm256_srav_epi32(r, counts);
}

CML_AVX_NOINLINE int int_compare(__m256i a, __m256i b) {
    const __m256i eq = _mm256_cmpeq_epi8(a, b);
    const __m256i gt = _mm256_cmpgt_epi32(a, b);
    const __m256i eq64 = _mm256_cmpeq_epi64(a, b);
    return _mm256_movemask_epi8(eq) + _mm256_testz_si256(gt, eq64) + _mm256_testc_si256(a, b);
}

CML_AVX_NOINLINE __m256i int_shuffle(__m256i a, __m256i b, __m256i control) {
    __m256i r = _mm256_shuffle_epi8(a, control);
    r = _mm256_shuffle_epi32(r, 0x4E);
    r = _mm256_shufflehi_epi16(r, 0x1B);
    r = _mm256_shufflelo_epi16(r, 0xB1);
    r = _mm256_permute4x64_epi64(r, 0xD8);
    r = _mm256_permutevar8x32_epi32(r, control);
    r = _mm256_permute2x128_si256(r, b, 0x31);
    r = _mm256_blend_epi16(r, b, 0x5A);
    r = _mm256_blend_epi32(r, a, 0x3C);
    r = _mm256_blendv_epi8(r, a, control);
    r = _mm256_alignr_epi8(r, b, 5);
    r = _mm256_unpacklo_epi8(r, _mm256_unpackhi_epi16(a, b));
    r = _mm256_unpacklo_epi64(r, _mm256_unpackhi_epi32(a, b));
    r = _mm256_packs_epi16(r, _mm256_packus_epi32(a, b));
    return _mm256_packus_epi16(r, _mm256_packs_epi32(a, b));
}

CML_AVX_NOINLINE __m256i int_widen(__m128i a, std::int32_t scalar, std::int64_t wide) {
    __m256i r = _mm256_cvtepi8_epi16(a);
    r = _mm256_add_epi32(r, _mm256_cvtepu8_epi32(a));
    r = _mm256_add_epi64(r, _mm256_cvtepi16_epi64(a));
    r = _mm256_add_epi64(r, _mm256_cvtepu32_epi64(a));
    r = _mm256_add_epi32(r, _mm256_broadcastd_epi32(_mm_cvtsi32_si128(scalar)));
    r = _mm256_add_epi64(r, _mm256_broadcastq_epi64(_mm_cvtsi64_si128(wide)));
    r = _mm256_add_epi8(r, _mm256_broadcastb_epi8(a));
    r = _mm256_add_epi16(r, _mm256_broadcastw_epi16(a));
    return _mm256_add_epi64(r, _mm256_broadcastsi128_si256(a));
}

CML_AVX_NOINLINE std::int64_t int_extract(__m256i a, __m128i b, __m128i* out) {
    const __m128i hi = _mm256_extracti128_si256(a, 1);
    const __m256i r = _mm256_inserti128_si256(a, b, 1);
    _mm_storeu_si128(out, _mm_insert_epi32(_mm_insert_epi8(hi, 7, 3), 9, 2));
    _mm_storel_epi64(out + 1, _mm_insert_epi16(b, 5, 1));
    return _mm_extract_epi8(hi, 5) + _mm_extract_epi16(hi, 2) + _mm_extract_epi32(b, 1) + _mm_extract_epi64(b, 1) +
           _mm_cvtsi128_si64(_mm256_castsi256_si128(r)) + _mm256_extract_epi32(r, 6);
}

CML_AVX_NOINLINE void int_stream(__m256i* to, const __m256i* from, __m256i a) {
    _mm256_stream_si256(to, _mm256_add_epi8(a, _mm256_stream_load_si256(from)));
    _mm256_storeu_si256(to + 1, _mm256_lddqu_si256(from + 1));
    _mm256_store_si256(to + 2, a);
}

// ---- 128-bit forms, strings, AES, carry-less multiply ---------------------------------------------------------------

CML_AVX_NOINLINE int strings(__m128i a, __m128i b, int la, int lb) {
    int r = _mm_cmpestri(a, la, b, lb, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ORDERED);
    r += _mm_cmpistri(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY);
    r += _mm_cvtsi128_si32(_mm_cmpistrm(a, b, _SIDD_UBYTE_OPS | _SIDD_CMP_RANGES));
    r += _mm_cvtsi128_si32(_mm_minpos_epu16(a));
    return r + _mm_movemask_epi8(_mm_cmpeq_epi8(a, b));
}

CML_AVX_NOINLINE CML_AVX_CRYPTO __m128i crypto(__m128i state, __m128i key) {
    __m128i r = _mm_aesenc_si128(state, key);
    r = _mm_aesenclast_si128(r, key);
    r = _mm_aesdec_si128(r, key);
    r = _mm_aesdeclast_si128(r, _mm_aesimc_si128(key));
    r = _mm_xor_si128(r, _mm_aeskeygenassist_si128(key, 0x1B));
    return _mm_xor_si128(_mm_clmulepi64_si128(r, key, 0x10), _mm_clmulepi64_si128(r, key, 0x01));
}

CML_AVX_NOINLINE __m128 sse_forms(__m128 a, __m128 b, __m128d c, __m128i i) {
    __m128 r = _mm_insert_ps(a, b, 0x1D);
    r = _mm_blend_ps(r, b, 0x5);
    r = _mm_dp_ps(r, a, 0x71);
    r = _mm_round_ss(r, a, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    r = _mm_add_ps(r, _mm_cvtpd_ps(_mm_round_pd(c, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC)));
    r = _mm_add_ps(r, _mm_cvtepi32_ps(_mm_shuffle_epi32(i, 0x1B)));
    r = _mm_movehl_ps(r, _mm_movelh_ps(a, b));
    r = _mm_permute_ps(r, 0xC6);
    return _mm_add_ps(r, _mm_castsi128_ps(_mm_srli_si128(i, 3)));
}

CML_AVX_NOINLINE void mxcsr(unsigned* saved, unsigned value) {
    *saved = _mm_getcsr();
    _mm_setcsr(value);
}

// ---- BMI1, BMI2, LZCNT: VEX-encoded general-purpose instructions ----------------------------------------------------

CML_AVX_NOINLINE std::uint32_t bits32(std::uint32_t a, std::uint32_t b, std::uint32_t n) {
    std::uint32_t r = ~a & b;  // andn
    r += _blsi_u32(a) + _blsr_u32(b) + _blsmsk_u32(a ^ b);
    r += _bextr_u32(a, 3, 9);
    r += _bzhi_u32(b, n);
    r += _pdep_u32(a, b) ^ _pext_u32(b, a);
    r += _lzcnt_u32(a) + _tzcnt_u32(b);
    r += (a << (n & 31)) + (b >> (n & 31)) + static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> (n & 31));
    return r + ((a >> 7) | (a << 25));
}

CML_AVX_NOINLINE unsigned long long bits64(unsigned long long a, unsigned long long b, unsigned long long n, unsigned long long* high) {
    unsigned long long r = ~a & b;  // andn
    r += _blsi_u64(a) + _blsr_u64(b) + _blsmsk_u64(a ^ b);
    r += _bextr_u64(a, 5, 20);
    r += _bzhi_u64(b, static_cast<std::uint32_t>(n));
    r += _pdep_u64(a, b) ^ _pext_u64(b, a);
    r += _lzcnt_u64(a) + _tzcnt_u64(b);
    r += _mulx_u64(a, b, high);
    r += (a << (n & 63)) + (b >> (n & 63)) + static_cast<unsigned long long>(static_cast<long long>(a) >> (n & 63));
    return r + ((a >> 13) | (a << 51));
}

// Keeps every function of this copy alive and reachable from main().
std::uintptr_t anchor() {
#define CML_AVX_KEEP(fn) sum += reinterpret_cast<std::uintptr_t>(&fn)
    std::uintptr_t sum = 0;
    CML_AVX_KEEP(scalar_float);
    CML_AVX_KEEP(scalar_double);
    CML_AVX_KEEP(scalar_convert);
    CML_AVX_KEEP(scalar_sqrt);
    CML_AVX_KEEP(scalar_compare);
    CML_AVX_KEEP(scalar_fma);
    CML_AVX_KEEP(loop_sum);
    CML_AVX_KEEP(loop_saxpy);
    CML_AVX_KEEP(loop_ints);
    CML_AVX_KEEP(loop_count_bytes);
    CML_AVX_KEEP(loop_widen);
    CML_AVX_KEEP(loop_min_max);
    CML_AVX_KEEP(copy_block);
    CML_AVX_KEEP(zero_block);
    CML_AVX_KEEP(ps_arith);
    CML_AVX_KEEP(pd_arith);
    CML_AVX_KEEP(ps_compare);
    CML_AVX_KEEP(ps_shuffle);
    CML_AVX_KEEP(pd_shuffle);
    CML_AVX_KEEP(ps_halves);
    CML_AVX_KEEP(convert);
    CML_AVX_KEEP(half_precision);
    CML_AVX_KEEP(masked_moves);
    CML_AVX_KEEP(gathers);
    CML_AVX_KEEP(upper_state);
    CML_AVX_KEEP(int_arith);
    CML_AVX_KEEP(int_shift);
    CML_AVX_KEEP(int_compare);
    CML_AVX_KEEP(int_shuffle);
    CML_AVX_KEEP(int_widen);
    CML_AVX_KEEP(int_extract);
    CML_AVX_KEEP(int_stream);
    CML_AVX_KEEP(strings);
    CML_AVX_KEEP(crypto);
    CML_AVX_KEEP(sse_forms);
    CML_AVX_KEEP(mxcsr);
    CML_AVX_KEEP(bits32);
    CML_AVX_KEEP(bits64);
#undef CML_AVX_KEEP
    return sum;
}

}  // namespace CML_AVX_VARIANT
}  // namespace cml_lde_avx
