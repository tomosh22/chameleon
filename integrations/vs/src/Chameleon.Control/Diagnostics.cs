using System;

namespace Chameleon.Control
{
    /// <summary>
    /// Diagnostic ids of the Visual Studio extension (ADR-0038: CML5994-5997). Every user-visible failure of the extension
    /// carries one of them, a one-line message and a "what to do" sentence.
    /// </summary>
    public static class ClientDiagIds
    {
        /// <summary>The control pipe cannot be reached: no broker is running, or it serves another pipe name.</summary>
        public const string NoBroker = "CML5994";

        /// <summary>The connection to the broker was lost, or a request got no answer.</summary>
        public const string PipeLost = "CML5995";

        /// <summary>The broker lacks a capability the extension needs for a feature, or is not a Chameleon broker.</summary>
        public const string Unsupported = "CML5996";

        /// <summary>The debugger could not be re-attached to the restarted process (or, WP-34, the held process could not be resumed).</summary>
        public const string ReattachFailed = "CML5997";
    }

    /// <summary>A diagnostic the extension itself reports (as opposed to <see cref="BrokerError"/>, which the broker sent).</summary>
    public sealed class ChameleonDiagnostic
    {
        public ChameleonDiagnostic(string id, string message, string action, bool isError = true)
        {
            Id = id;
            Message = message;
            Action = action;
            IsError = isError;
        }

        public string Id { get; }
        public string Message { get; }
        public string Action { get; }
        public bool IsError { get; }

        public override string ToString()
        {
            return Id + ": " + Message + " What to do: " + Action;
        }
    }

    /// <summary>The factories for the four client-side diagnostics, so every message and its advice live in one place.</summary>
    public static class ClientDiagnostics
    {
        public static ChameleonDiagnostic NoBroker(string pipeName, string detail)
        {
            return new ChameleonDiagnostic(
                ClientDiagIds.NoBroker,
                "Cannot reach the Chameleon broker on the control pipe '" + pipeName + "': " + detail + ".",
                "Start the application with the Chameleon agent (it starts the broker) or run 'cml_broker.exe --headless', then choose Tools > Chameleon > Connect; " +
                "if the broker serves another pipe name, enter it in Tools > Options > Chameleon > General.");
        }

        public static ChameleonDiagnostic PipeLost(string pipeName, string detail)
        {
            return new ChameleonDiagnostic(
                ClientDiagIds.PipeLost,
                "The connection to the Chameleon broker on '" + pipeName + "' was lost: " + detail + ".",
                "Chameleon reconnects by itself while 'Reconnect automatically' is on in Tools > Options > Chameleon > General; " +
                "otherwise choose Tools > Chameleon > Connect once the broker runs again.");
        }

        public static ChameleonDiagnostic Unsupported(string reason, bool isError = false)
        {
            return new ChameleonDiagnostic(
                ClientDiagIds.Unsupported,
                "The broker " + reason + ".",
                "Use cml_broker.exe and cml_agent.dll from one Chameleon release; the Visual Studio features that need the missing capability stay disabled.",
                isError);
        }

        public static ChameleonDiagnostic ReattachFailed(int oldPid, int newPid, string reason)
        {
            return new ChameleonDiagnostic(
                ClientDiagIds.ReattachFailed,
                "Could not attach the debugger to the restarted process (pid " + newPid + ", it replaced pid " + oldPid + "): " + reason + ".",
                "Attach by hand with Debug > Attach to Process (pid " + newPid + "); to stop Chameleon re-attaching, clear Tools > Chameleon > " +
                "Re-attach debugger after hot-restart (or the same setting in Tools > Options > Chameleon > General).");
        }

        /// <summary>WP-34: the new process was held for the debugger and `restart-resume` did not get through.</summary>
        public static ChameleonDiagnostic ResumeFailed(int newPid, string reason, int holdMs)
        {
            return new ChameleonDiagnostic(
                ClientDiagIds.ReattachFailed,
                "Could not tell the broker to resume the restarted process (pid " + newPid + "): " + reason + ".",
                "Nothing else to do: the broker resumes the process by itself when the hold time (" + (Math.Min(60000, holdMs) / 1000) + " s) has passed. " +
                "If the application stays frozen, run Tools > Chameleon > Hot-restart again.");
        }

        /// <summary>WP-34: the debugger attached too slowly (or not at all) to the held process.</summary>
        public static ChameleonDiagnostic AttachTooSlow(int oldPid, int newPid, int waitedMs)
        {
            return ReattachFailed(oldPid, newPid, "the debugger did not finish attaching within " + (waitedMs / 1000) + " s; the process was resumed without waiting");
        }
    }

    /// <summary>Thrown by the control client and the broker session for a failure that has a client-side diagnostic.</summary>
    public sealed class ControlException : Exception
    {
        public ControlException(ChameleonDiagnostic diagnostic, bool timedOut = false)
            : base(diagnostic.ToString())
        {
            Diagnostic = diagnostic;
            TimedOut = timedOut;
        }

        public ChameleonDiagnostic Diagnostic { get; }

        /// <summary>WP-43 (R-40): the request got no answer in time; the connection is still open (a dropped connection is false).</summary>
        public bool TimedOut { get; }
    }
}
