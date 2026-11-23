using System;

namespace Chameleon.Control
{
    /// <summary>
    /// The extension's settings. The Visual Studio shell copies its Tools &gt; Options page into one instance; the logic reads the
    /// current values at the moment it needs them, so a change applies without reconnecting.
    /// </summary>
    public sealed class ChameleonSettings
    {
        /// <summary>Full control pipe name (\\.\pipe\...). Empty: CML_CONTROL_PIPE, else the default for this logon session.</summary>
        public string PipeName { get; set; } = "";

        /// <summary>Connect to the broker as soon as Visual Studio starts, and keep waiting for one to appear.</summary>
        public bool AutoConnect { get; set; } = true;

        /// <summary>Connect again after the connection was lost.</summary>
        public bool AutoReconnect { get; set; } = true;

        /// <summary>After a hot-restart, attach the debugger to the new process when it was attached to the old one.</summary>
        public bool ReattachOnRestart { get; set; } = true;

        /// <summary>Attach attempts before CML5997 is reported (the new process may not be attachable for a moment).</summary>
        public int ReattachAttempts { get; set; } = 3;

        /// <summary>Pause between two attach attempts.</summary>
        public int ReattachRetryDelayMs { get; set; } = 300;

        /// <summary>
        /// WP-34: how long the broker holds the new process suspended while the debugger attaches (`hold_ms` of `restart`, at most 60000).
        /// It must be longer than <see cref="HoldAttachTimeoutMs"/> so that this extension resumes the process before the broker does.
        /// </summary>
        public int RestartHoldMs { get; set; } = 30000;

        /// <summary>
        /// WP-34: the extension resumes the held process as soon as the debugger lists it, or after this long at the latest, even if the
        /// debugger's attach call has not returned (a real attach to a process that has not run can block until it runs).
        /// </summary>
        public int HoldResumeGraceMs { get; set; } = 3000;

        /// <summary>WP-34: how long the extension waits for the debugger to attach to the held process before it resumes it anyway (CML5997).</summary>
        public int HoldAttachTimeoutMs { get; set; } = 20000;

        /// <summary>
        /// How long a process that left the debugger is still remembered as "was debugged", so that a hot-restart (which terminates the
        /// old process before the new one is announced) still finds it.
        /// </summary>
        public TimeSpan GoneProcessMemory { get; set; } = TimeSpan.FromMinutes(5);

        /// <summary>
        /// Also subscribe to the broker's `exception` events. Off by default: while a client listens for them the agents hold a faulting
        /// thread until somebody answers with `exception-action`, which would stall the debuggee under Visual Studio.
        /// </summary>
        public bool ShowExceptionEvents { get; set; }

        /// <summary>Client-side wait for `reload` (the broker's own default budget is 60 s).</summary>
        public int ReloadTimeoutMs { get; set; } = 120000;

        /// <summary>Client-side wait for `restart` (the broker budgets about 60 s before and 30 s after terminating the process).</summary>
        public int RestartTimeoutMs { get; set; } = 300000;

        /// <summary>Wait for one connection attempt.</summary>
        public int ConnectTimeoutMs { get; set; } = 500;
    }
}
