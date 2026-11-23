// WP-26 corpus support code (Chameleon's own, MIT like the rest of this repository). The corpus driver copies
// chameleon_corpus.h / chameleon_corpus.cpp / Chameleon_API.h into the pinned third-party project it builds;
// the project's patch file (tests/corpus/<project>/chameleon.patch) adds the calls below. Nothing of this is
// vendored into the project's repository.
//
//   cml_corpus_init()    once at start-up: with CML_CORPUS_AGENT=1 creates the agent (cml_agent.dll from
//                        CML_CORPUS_AGENT_DIR) and enables the running module.
//   cml_corpus_frame()   once per frame on the main thread: synchronises with the agent and, about every 200 ms,
//                        prints   CORPUS frame=N inline=I tmpl=T markers=name:count,...
//                        to stdout and to the file CML_CORPUS_LOG.
//   cml_corpus_marker(s) called by the one-line edits the corpus driver makes to upstream functions: the name
//                        shows up in the markers list, which is how the driver sees that edited code runs.
//
// inline=, tmpl= come from the header inline function and the template below: edit targets in a header
// that only a few translation units include (the corpus driver edits them like any other source).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
void cml_corpus_init(void);
void cml_corpus_frame(void);
void cml_corpus_marker(const char* name);
#ifdef __cplusplus
}

inline int cml_corpus_inline_value() {
    return 1;
}

template <typename T>
T cml_corpus_scale(T value) {
    return value * static_cast<T>(2);
}
#endif
