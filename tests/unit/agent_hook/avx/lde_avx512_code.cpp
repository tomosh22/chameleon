// Code for cml_lde_avx_module, compiled with /arch:AVX512 (F, CD, BW, DQ, VL): EVEX-encoded instructions and the
// VEX-encoded opmask instructions as a compiler emits them. Never executed (see lde_avx2_code.cpp).
#include <immintrin.h>

#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER)
#define CML_AVX_NOINLINE __declspec(noinline)
#else
#define CML_AVX_NOINLINE __attribute__((noinline))
#endif

namespace cml_lde_avx {
namespace avx512 {

// ---- loops a compiler vectorises with 512-bit or masked 256-bit code ------------------------------------------------

CML_AVX_NOINLINE float loop_sum(const float* p, std::size_t n) {
    float s = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        s += p[i] * 1.5f;
    }
    return s;
}

CML_AVX_NOINLINE void loop_select(std::int32_t* out, const std::int32_t* a, const std::int32_t* b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = a[i] > b[i] ? a[i] - b[i] : b[i] * 5;
    }
}

CML_AVX_NOINLINE void loop_convert(double* out, const std::uint64_t* in, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<double>(in[i]);  // vcvtuqq2pd / vcvtusi2sd
    }
}

CML_AVX_NOINLINE unsigned scalar_unsigned(float a, double b) {
    return static_cast<unsigned>(a) + static_cast<unsigned>(b);  // vcvttss2usi, vcvttsd2usi
}

struct Block {
    double values[32];
};

CML_AVX_NOINLINE void copy_block(Block* to, const Block* from) {
    *to = *from;
}

// ---- 512-bit floating point -----------------------------------------------------------------------------------------

CML_AVX_NOINLINE __m512 ps_arith(const float* a, const float* b, __mmask16 k) {
    const __m512 x = _mm512_loadu_ps(a);
    const __m512 y = _mm512_load_ps(b);
    __m512 r = _mm512_add_ps(_mm512_mul_ps(x, y), _mm512_sub_ps(x, y));
    r = _mm512_mask_div_ps(r, k, r, _mm512_sqrt_ps(y));
    r = _mm512_maskz_max_ps(k, r, _mm512_min_ps(x, y));
    r = _mm512_fmadd_ps(r, x, _mm512_fnmsub_ps(y, x, r));
    r = _mm512_add_round_ps(r, x, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    r = _mm512_and_ps(_mm512_or_ps(r, x), _mm512_xor_ps(_mm512_andnot_ps(y, r), x));
    return _mm512_add_ps(r, _mm512_set1_ps(a[3]));  // broadcast from memory
}

CML_AVX_NOINLINE __m512d pd_arith(const double* a, __m512d y, double* out, __mmask8 k) {
    const __m512d x = _mm512_maskz_loadu_pd(k, a);
    __m512d r = _mm512_scalef_pd(x, y);
    r = _mm512_roundscale_pd(r, 0x13);
    r = _mm512_getexp_pd(_mm512_getmant_pd(r, _MM_MANT_NORM_1_2, _MM_MANT_SIGN_src));
    r = _mm512_rsqrt14_pd(_mm512_rcp14_pd(r));
    r = _mm512_fixupimm_pd(r, x, _mm512_castpd_si512(y), 0x22);
    r = _mm512_range_pd(r, x, 0x5);
    r = _mm512_reduce_pd(r, 0x41);
    _mm512_mask_storeu_pd(out, k, r);
    _mm512_stream_pd(out + 8, r);
    _mm512_mask_compressstoreu_pd(out + 16, k, r);
    return _mm512_mask_expandloadu_pd(r, k, a + 8);
}

CML_AVX_NOINLINE int compares(__m512 a, __m512 b, __m512d c, __m512i i, __m512i j) {
    const __mmask16 lt = _mm512_cmp_ps_mask(a, b, _CMP_LT_OQ);
    const __mmask8 ge = _mm512_cmp_pd_mask(c, _mm512_castps_pd(a), _CMP_GE_OS);
    const __mmask16 eq = _mm512_cmpeq_epi32_mask(i, j);
    const __mmask8 gt = _mm512_mask_cmpgt_epi64_mask(ge, i, j);
    const __mmask64 bytes = _mm512_cmpeq_epi8_mask(i, j);
    const __mmask32 words = _mm512_cmplt_epu16_mask(i, j);
    const __mmask16 cls = _mm512_fpclass_ps_mask(a, 0x81);
    const __mmask16 test = _mm512_test_epi32_mask(i, j);
    const __mmask16 mixed = _kor_mask16(_kand_mask16(lt, eq), _kxor_mask16(_knot_mask16(cls), _kandn_mask16(test, lt)));
    return static_cast<int>(_cvtmask16_u32(mixed)) + static_cast<int>(_cvtmask8_u32(gt)) +
           static_cast<int>(_mm_popcnt_u64(_cvtmask64_u64(bytes))) + static_cast<int>(_cvtmask32_u32(words)) +
           _kortestz_mask16_u8(lt, eq) + static_cast<int>(_cvtmask16_u32(_kshiftli_mask16(lt, 3)));
}

CML_AVX_NOINLINE __m512 shuffles(__m512 a, __m512 b, __m512i control, __mmask16 k) {
    __m512 r = _mm512_shuffle_ps(a, b, 0x4E);
    r = _mm512_permute_ps(r, 0x1B);
    r = _mm512_permutexvar_ps(control, r);
    r = _mm512_permutex2var_ps(r, control, b);
    r = _mm512_shuffle_f32x4(r, a, 0x8D);
    r = _mm512_mask_blend_ps(k, r, b);
    r = _mm512_mask_mov_ps(r, k, a);
    r = _mm512_unpacklo_ps(r, _mm512_unpackhi_ps(a, b));
    r = _mm512_insertf32x4(r, _mm512_extractf32x4_ps(a, 2), 1);
    const __m512d wide = _mm512_insertf64x4(_mm512_castps_pd(r), _mm512_extractf64x4_pd(_mm512_castps_pd(b), 1), 0);
    r = _mm512_add_ps(r, _mm512_castpd_ps(wide));
    return _mm512_broadcast_f32x4(_mm512_castps512_ps128(r));
}

// ---- 512-bit integer ------------------------------------------------------------------------------------------------

CML_AVX_NOINLINE __m512i int_arith(const __m512i* a, __m512i y, __mmask16 k) {
    const __m512i x = _mm512_loadu_si512(a);
    __m512i r = _mm512_add_epi32(x, y);
    r = _mm512_mask_sub_epi64(r, static_cast<__mmask8>(k), r, y);
    r = _mm512_mullo_epi64(r, x);
    r = _mm512_maskz_mullo_epi32(k, r, y);
    r = _mm512_ternarylogic_epi32(r, x, y, 0x96);
    r = _mm512_rol_epi32(r, 7);
    r = _mm512_rorv_epi64(r, y);
    r = _mm512_srai_epi64(r, 9);
    r = _mm512_sllv_epi16(r, y);
    r = _mm512_max_epu64(r, _mm512_min_epi64(x, y));
    r = _mm512_abs_epi64(r);
    r = _mm512_lzcnt_epi32(r);
    r = _mm512_conflict_epi32(r);
    r = _mm512_add_epi8(r, _mm512_adds_epu16(x, y));
    r = _mm512_sad_epu8(r, _mm512_dbsad_epu8(x, y, 0x3));
    r = _mm512_xor_si512(r, _mm512_and_si512(_mm512_or_si512(x, y), _mm512_andnot_si512(x, r)));
    return _mm512_add_epi32(r, _mm512_set1_epi32(k));  // vpbroadcastd from a general register
}

// Short functions on purpose: cl 14.44 and 14.52 fail with C1001 (an internal error of the optimiser, at /O2) on
// longer chains of these, e.g. on _mm512_mask_blend_epi8 followed by _mm512_maskz_mov_epi8 with the same mask. Each of
// the functions below was compiled alone with both compilers before it was written this way.
CML_AVX_NOINLINE __m512i int_permutes(__m512i a, __m512i b, __m512i control) {
    __m512i r = _mm512_shuffle_epi8(a, control);
    r = _mm512_shuffle_epi32(r, _MM_PERM_BADC);
    r = _mm512_permutexvar_epi16(control, r);
    return _mm512_permutex2var_epi64(r, control, b);
}

CML_AVX_NOINLINE __m512i int_lanes(__m512i a, __m512i b) {
    __m512i r = _mm512_shuffle_i64x2(a, b, 0x4E);
    r = _mm512_alignr_epi32(r, b, 3);
    return _mm512_alignr_epi8(r, a, 5);
}

CML_AVX_NOINLINE __m512i int_blend(__m512i a, __m512i b, __mmask64 k) {
    return _mm512_mask_blend_epi8(k, a, b);
}

CML_AVX_NOINLINE __m512i int_masked_move(__m512i a, __mmask64 k) {
    return _mm512_maskz_mov_epi8(k, a);
}

CML_AVX_NOINLINE __m512i int_unpacks(__m512i a, __m512i b) {
    return _mm512_unpacklo_epi16(a, _mm512_unpackhi_epi64(a, b));
}

CML_AVX_NOINLINE __m512i int_packs(__m512i a, __m512i b) {
    return _mm512_packs_epi32(a, _mm512_packus_epi16(a, b));
}

CML_AVX_NOINLINE __m512i int_upper_half(__m512i a, __m512i b) {
    return _mm512_inserti32x8(a, _mm512_extracti64x4_epi64(b, 1), 0);
}

CML_AVX_NOINLINE __m512i int_quarter(__m512i a, __m512i b) {
    return _mm512_inserti64x2(a, _mm512_extracti32x4_epi32(b, 3), 2);
}

CML_AVX_NOINLINE __m512i conversions(__m512 f, __m512d d, __m512i q, __m256i* narrow, __m128i* bytes) {
    const __m512i a = _mm512_cvttps_epu32(f);
    const __m512i b = _mm512_cvtpd_epi64(d);
    const __m512d c = _mm512_cvtepu64_pd(q);
    const __m256 e = _mm512_cvtepi64_ps(q);
    _mm256_storeu_si256(narrow, _mm512_cvtepi64_epi32(q));
    _mm_storeu_si128(bytes, _mm512_cvtsepi32_epi8(a));
    _mm512_mask_cvtusepi64_storeu_epi16(bytes + 1, 0x5A, b);
    const __m512i wide = _mm512_cvtepu8_epi64(_mm_loadu_si128(bytes));
    const __m512i m = _mm512_movm_epi8(_mm512_movepi8_mask(q));
    return _mm512_add_epi64(_mm512_add_epi64(wide, m), _mm512_castpd_si512(_mm512_add_pd(c, _mm512_cvtps_pd(e))));
}

CML_AVX_NOINLINE __m512 gather_scatter(const float* base, float* to, __m512i index, __mmask16 k, __m512 src) {
    const __m512 a = _mm512_i32gather_ps(index, base, 4);
    const __m512 b = _mm512_mask_i32gather_ps(src, k, index, base + 64, 4);
    _mm512_i32scatter_ps(to, index, a, 4);
    _mm512_mask_i32scatter_ps(to + 64, k, index, b, 4);
    const __m512d c = _mm512_i64gather_pd(index, reinterpret_cast<const double*>(base), 8);
    _mm512_i64scatter_pd(reinterpret_cast<double*>(to), index, c, 8);
    return _mm512_add_ps(a, b);
}

// ---- AVX-512VL: EVEX with 128- and 256-bit vectors, registers 16..31 -------------------------------------------------

CML_AVX_NOINLINE __m256 vl_forms(__m256 a, __m256 b, __m128 c, __mmask8 k, const __m256i* p) {
    __m256 r = _mm256_mask_add_ps(a, k, a, b);
    r = _mm256_maskz_mul_ps(k, r, b);
    r = _mm256_mask_blend_ps(k, r, a);
    const __m256i i = _mm256_mask_loadu_epi32(_mm256_castps_si256(r), k, p);
    const __m256i j = _mm256_ternarylogic_epi64(i, _mm256_castps_si256(a), _mm256_castps_si256(b), 0xCA);
    const __mmask8 m = static_cast<__mmask8>(_mm256_cmp_ps_mask(r, b, _CMP_NEQ_UQ) | _mm_cmp_ps_mask(c, c, _CMP_ORD_Q));
    const __m128i t = _mm256_cvtepi32_epi16(j);
    const __m256i u = _mm256_rol_epi32(_mm256_abs_epi64(j), 3);
    const __m128 s = _mm_mask_fmadd_ss(c, m, c, _mm256_castps256_ps128(r));
    return _mm256_add_ps(_mm256_castsi256_ps(_mm256_add_epi32(u, _mm256_cvtepu16_epi32(t))), _mm256_broadcastss_ps(s));
}

std::uintptr_t anchor() {
#define CML_AVX_KEEP(fn) sum += reinterpret_cast<std::uintptr_t>(&fn)
    std::uintptr_t sum = 0;
    CML_AVX_KEEP(loop_sum);
    CML_AVX_KEEP(loop_select);
    CML_AVX_KEEP(loop_convert);
    CML_AVX_KEEP(scalar_unsigned);
    CML_AVX_KEEP(copy_block);
    CML_AVX_KEEP(ps_arith);
    CML_AVX_KEEP(pd_arith);
    CML_AVX_KEEP(compares);
    CML_AVX_KEEP(shuffles);
    CML_AVX_KEEP(int_arith);
    CML_AVX_KEEP(int_permutes);
    CML_AVX_KEEP(int_lanes);
    CML_AVX_KEEP(int_blend);
    CML_AVX_KEEP(int_masked_move);
    CML_AVX_KEEP(int_unpacks);
    CML_AVX_KEEP(int_packs);
    CML_AVX_KEEP(int_upper_half);
    CML_AVX_KEEP(int_quarter);
    CML_AVX_KEEP(conversions);
    CML_AVX_KEEP(gather_scatter);
    CML_AVX_KEEP(vl_forms);
#undef CML_AVX_KEEP
    return sum;
}

}  // namespace avx512
}  // namespace cml_lde_avx
