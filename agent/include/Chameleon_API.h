/*
 * Chameleon_API.h - public C API of cml_agent.dll.
 *
 * Stub for WP-01. The surface is C so that it can be consumed from C and C++ projects built
 * with any compiler. Every function is callable from any thread.
 */
#ifndef CHAMELEON_API_H
#define CHAMELEON_API_H

#if defined(_WIN32)
#if defined(CML_AGENT_BUILD)
#define CML_API __declspec(dllexport)
#else
#define CML_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define CML_API __attribute__((visibility("default")))
#else
#define CML_API
#endif

/* Bumped on any incompatible change to this header. */
#define CML_AGENT_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

/* Returns CML_AGENT_ABI_VERSION as compiled into the loaded agent. */
CML_API int cml_agent_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CHAMELEON_API_H */
