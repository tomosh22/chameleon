// Client-side diagnostics of the VS Code extension (ADR-0038 range CML5990-5993).
// Broker-side diagnostics (CML5300, CML5850, ...) pass through unchanged; these four are raised by the extension itself.
// docs/wp/WP-30.md repeats every id with its "what to do" sentence.

export interface Diag {
  id: string;
  message: string;
  action: string;
}

export const ids = {
  /** No broker answers on the control pipe (not started, wrong pipe name, other user). */
  noBroker: "CML5990",
  /** The control connection closed or failed after it was established. */
  pipeLost: "CML5991",
  /** The broker lacks a capability or does not know a command (unsupported broker version). */
  unsupportedBroker: "CML5992",
  /** The broker's answer is unusable (malformed line, wrong shape, no answer in time). */
  badAnswer: "CML5993",
} as const;

export function noBroker(pipe: string, detail: string): Diag {
  return {
    id: ids.noBroker,
    message: `cannot reach the Chameleon broker on ${pipe}: ${detail}`,
    action:
      "Start cml_broker (or cml_ui) as the same user, or set the 'chameleon.controlPipe' setting to the pipe the broker was started with (--control-pipe).",
  };
}

export function pipeLost(pipe: string, detail: string): Diag {
  return {
    id: ids.pipeLost,
    message: `the control pipe ${pipe} was lost: ${detail}`,
    action: "Check that the broker is still running; the extension reconnects by itself while 'chameleon.autoConnect' is on, or run 'Chameleon: Connect'.",
  };
}

export function unsupportedBroker(what: string, brokerVersion?: string): Diag {
  return {
    id: ids.unsupportedBroker,
    message: `this broker${brokerVersion ? ` (version ${brokerVersion})` : ""} does not provide ${what}`,
    action: "Use a Chameleon broker that advertises this capability (build the broker and the extension from the same release).",
  };
}

export function badAnswer(detail: string): Diag {
  return {
    id: ids.badAnswer,
    message: `the broker's answer cannot be used: ${detail}`,
    action: "Check the broker log; make sure the broker and the extension come from the same release.",
  };
}

/** An Error that carries a Diag (client-side or the broker's own, copied from an error response). */
export class DiagError extends Error {
  readonly diag: Diag;
  constructor(diag: Diag) {
    super(`${diag.id}: ${diag.message}`);
    this.name = "DiagError";
    this.diag = diag;
  }
}

export function formatDiag(d: Diag): string {
  return `${d.id}: ${d.message}${d.action ? ` What to do: ${d.action}` : ""}`;
}
