# Chameleon — Progress

Status values: `todo`, `in progress`, `in review`, `blocked`, `done`. "Done" = acceptance green
on every CI matrix cell + reviewed + `docs/wp/WP-NN.md` present + merged.

| WP | Title | Wave | Status | Branch | PR | Blockers |
|---|---|---|---|---|---|---|
| WP-01 | Repository skeleton and CI | 0 | done | wp/01-skeleton-ci | #2 | — |
| WP-02 | cml_coff and cml_pe readers | 1 | done | wp/02-coff-pe-readers | #4 | — |
| WP-04 | cml_pdb index | 1 | done | wp/04-pdb-index | #5 | — |
| WP-11 | Agent skeleton, public header and IPC | 1 | in review (fixing 10 high-effort review findings) | wp/11-agent-ipc | #6 | — |
| WP-03 | End-to-end spike (M1 gate) | 2 | done — **gate passed** (verified by orchestrator from CI logs, all 8 cells) | wp/03-e2e-spike | #10 | — |
| WP-05 | Toolchain command parsing and rewriting | 2 | in progress | wp/05-toolchain | — | — |
| WP-06 | cml_coff writer: anchors and binding object | 2 | in progress | wp/06-coff-writer | — | — |
| WP-13 | Test harness cmltest | 2 | in review (fixing 10 review findings) | wp/13-cmltest | #12 | — |
| WP-07..WP-10 | Wave 3 | 3 | todo | — | — | wave 2 merge |
| WP-12 | Broker core (orchestrator integration) | 4 | todo | — | — | wave 3 |

## Orchestrator integration work

| Item | Status | PR |
|---|---|---|
| Housekeeping docs (DECISIONS, KNOWN_LIMITATIONS, PROGRESS) | done | #1 |
| Shared fixture `basic` + `cml_add_fixture`; flaky log test fix | done | #3 |
| Make `extras.cpp` opaque so constructs survive clang-cl /O2 (WP-02 note) | in review | #8 |
