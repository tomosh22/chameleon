# Chameleon — Progress

Status values: `todo`, `in progress`, `in review`, `blocked`, `done`. "Done" = acceptance green
on every CI matrix cell + reviewed + `docs/wp/WP-NN.md` present + merged.

| WP | Title | Wave | Status | Branch | PR | Blockers |
|---|---|---|---|---|---|---|
| WP-01 | Repository skeleton and CI | 0 | done | wp/01-skeleton-ci | #2 | — |
| WP-02 | cml_coff and cml_pe readers | 1 | todo | — | — | WP-01 |
| WP-04 | cml_pdb index | 1 | todo | — | — | WP-01 |
| WP-11 | Agent skeleton, public header and IPC | 1 | todo | — | — | WP-01 |
| WP-03 | End-to-end spike (M1 gate) | 2 | todo | — | — | WP-02 |
| WP-05 | Toolchain command parsing and rewriting | 2 | todo | — | — | WP-04; no merge before WP-03 |
| WP-06 | cml_coff writer: anchors and binding object | 2 | todo | — | — | WP-02; no merge before WP-03 |
| WP-13 | Test harness cmltest | 2 | todo | — | — | WP-11; no merge before WP-03 |
| WP-07..WP-10 | Wave 3 | 3 | todo | — | — | WP-03 gate |
| WP-12 | Broker core (orchestrator integration) | 4 | todo | — | — | wave 3 |
