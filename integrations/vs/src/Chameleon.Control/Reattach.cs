using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace Chameleon.Control
{
    /// <summary>A process the debugger is attached to, with the debug engines it runs under ("Native", "Managed (.NET Core, .NET 5+)", ...).</summary>
    public sealed class DebuggedProcess
    {
        public DebuggedProcess(int pid, IEnumerable<string> engines)
        {
            Pid = pid;
            Engines = (engines ?? Enumerable.Empty<string>()).ToArray();
        }

        public int Pid { get; }
        public IReadOnlyList<string> Engines { get; }
    }

    /// <summary>What the re-attach logic needs from the debugger. The Visual Studio shell implements it; the tests use a fake.</summary>
    public interface IDebuggerAttach
    {
        /// <summary>The processes the debugger is attached to now. Safe to call from any thread.</summary>
        IReadOnlyList<DebuggedProcess> GetDebuggedProcesses();

        /// <summary>
        /// Attaches the debugger to <paramref name="pid"/> with <paramref name="engines"/> (empty: let the debugger choose). Throws when it
        /// cannot; the message goes into CML5997.
        /// </summary>
        Task AttachAsync(int pid, IReadOnlyList<string> engines, CancellationToken cancellationToken);
    }

    public enum ReattachOutcome
    {
        /// <summary>The setting is off.</summary>
        Disabled,

        /// <summary>The debugger was not attached to the old process: nothing to do.</summary>
        NotDebugged,

        /// <summary>The debugger is attached to the new process already.</summary>
        AlreadyAttached,

        /// <summary>This (old, new) pair was handled before (the event and the command answer both report it).</summary>
        Duplicate,

        Attached,

        /// <summary>CML5997 was reported.</summary>
        Failed,
    }

    /// <summary>
    /// After a hot-restart (event `restarted {old_pid, new_pid}`) attaches the debugger to the new process when it was attached to the old
    /// one, with the same engines.
    /// <para>
    /// The broker terminates the old process before it announces the new one, and Visual Studio ends the debug session of a process that
    /// exits. "Was attached to the old pid" therefore cannot be read at the time of the event: <see cref="Observe"/> remembers every
    /// debugged process while it is alive (the shell calls it on debugger events and before sending a restart), and a process that
    /// disappears stays remembered for <see cref="ChameleonSettings.GoneProcessMemory"/>.
    /// </para>
    /// </summary>
    public sealed class ReattachCoordinator
    {
        private sealed class Known
        {
            public string[] Engines;
            public DateTime? GoneAt;
        }

        private readonly IDebuggerAttach _debugger;
        private readonly ChameleonSettings _settings;
        private readonly Action<ChameleonDiagnostic> _report;
        private readonly Action<string> _log;
        private readonly Func<DateTime> _now;
        private readonly Func<TimeSpan, CancellationToken, Task> _delay;
        private readonly Func<TimeSpan, CancellationToken, Task> _attachTimeout;
        private readonly Func<TimeSpan, CancellationToken, Task> _resumeGrace;
        private readonly object _gate = new object();
        private readonly List<HoldTicket> _expectedHolds = new List<HoldTicket>();   // restarts this extension asked to hold (WP-34) whose `restarting` event has not arrived yet
        private readonly Dictionary<int, Known> _known = new Dictionary<int, Known>();
        private readonly HashSet<string> _handled = new HashSet<string>();
        private readonly Queue<string> _reported = new Queue<string>();   // WP-43 (R-40): the restarts (old>new) a CML5997 was reported for, the last 64
        private readonly SemaphoreSlim _serial = new SemaphoreSlim(1, 1);

        public ReattachCoordinator(
            IDebuggerAttach debugger,
            ChameleonSettings settings,
            Action<ChameleonDiagnostic> report,
            Action<string> log,
            Func<DateTime> now = null,
            Func<TimeSpan, CancellationToken, Task> delay = null,
            Func<TimeSpan, CancellationToken, Task> attachTimeout = null,
            Func<TimeSpan, CancellationToken, Task> resumeGrace = null)
        {
            _debugger = debugger;
            _settings = settings;
            _report = report ?? (_ => { });
            _log = log ?? (_ => { });
            _now = now ?? (() => DateTime.UtcNow);
            _delay = delay ?? ((span, ct) => Task.Delay(span, ct));
            _attachTimeout = attachTimeout ?? ((span, ct) => Task.Delay(span, ct));
            _resumeGrace = resumeGrace ?? ((span, ct) => Task.Delay(span, ct));
        }

        // ---- WP-34: early attach to a process the broker holds suspended ----------------------------------------------------------

        /// <summary>
        /// Whether `restart` should ask the broker to hold the new process: automatic re-attach is on, the broker has the capability
        /// `restart-hold`, and the debugger is attached to the process to be restarted, <paramref name="oldPid"/> (WP-43, R-40: without a
        /// debugger on it there is nothing to attach early, and holding the new process only delays it). When the pid is not known (the
        /// broker picks the only process) the restart is not held. Otherwise the restart is the one of old, and the debugger attaches after
        /// `restarted` if it was attached.
        /// </summary>
        public bool ShouldHoldNewProcess(bool brokerHasRestartHold, int? oldPid)
        {
            if (!_settings.ReattachOnRestart || !brokerHasRestartHold || !oldPid.HasValue)
            {
                return false;
            }

            Observe();
            return IsDebugged(oldPid.Value);
        }

        /// <summary>
        /// WP-43 (R-40): reports CML5997 for the restart old -> new at most once. An early attach that fails, the late attach that follows it on
        /// `restarted`, and a resume that does not get through are one problem of one restart for the user; the later ones are only logged.
        /// </summary>
        public void ReportRestartProblem(int oldPid, int newPid, ChameleonDiagnostic diagnostic)
        {
            string key = oldPid + ">" + newPid;
            bool first;
            lock (_gate)
            {
                first = !_reported.Contains(key);
                if (first)
                {
                    _reported.Enqueue(key);
                    while (_reported.Count > 64)
                    {
                        _reported.Dequeue();
                    }
                }
            }

            if (first)
            {
                _report(diagnostic);
            }
            else
            {
                _log("reattach: (reported for this restart already) " + diagnostic);
            }
        }

        /// <summary>
        /// A restart this extension asked the broker to hold. Its <see cref="Token"/> travels in the request (`token`) and comes back in the
        /// `restarting` event, so the event is matched to this restart and to no other client's.
        /// </summary>
        public sealed class HoldTicket
        {
            public string Token { get; } = Guid.NewGuid().ToString("N");
        }

        /// <summary>
        /// Call right before sending `restart {hold_new, token}`: the `restarting` event that carries the ticket's token is ours to answer.
        /// Pass the ticket to <see cref="RestartingNoLongerExpected"/> when that `restart` was answered or failed.
        /// </summary>
        public HoldTicket ExpectRestarting()
        {
            var ticket = new HoldTicket();
            lock (_gate)
            {
                _expectedHolds.Add(ticket);
            }

            return ticket;
        }

        /// <summary>The `restart` was answered or failed: an event that never came must not linger. No effect when it was claimed.</summary>
        public void RestartingNoLongerExpected(HoldTicket ticket)
        {
            lock (_gate)
            {
                _expectedHolds.Remove(ticket);
            }
        }

        /// <summary>
        /// True once per ticket: the `restarting` event carries the token of a restart this extension asked for. A held process another
        /// client asked for (another token, or none) is that client's to resume, never ours.
        /// </summary>
        public bool TryClaimRestarting(string token)
        {
            if (string.IsNullOrEmpty(token))
            {
                return false;
            }

            lock (_gate)
            {
                int i = _expectedHolds.FindIndex(t => t.Token == token);
                if (i < 0)
                {
                    return false;
                }

                _expectedHolds.RemoveAt(i);
                return true;
            }
        }

        /// <summary>
        /// The broker holds the new process (pid <paramref name="newPid"/>, it replaced <paramref name="oldPid"/>) suspended: starts the
        /// debugger's attach and calls <paramref name="resume"/> (`restart-resume`) as soon as the debugger has the pid (it is listed among its
        /// debugged processes), the attach returned, or <see cref="ChameleonSettings.HoldResumeGraceMs"/> passed.
        /// <para>
        /// The resume does not wait for the attach call to <em>return</em>: attaching to a process whose first thread has not run can block until
        /// the process runs (the debugger's break-in thread cannot start before the loader). Once the debug port is set the module-load events
        /// bind start-up breakpoints after the resume. The process is resumed on every path (attach ok, not needed, failed, slow, cancelled,
        /// threw). A resume that does not get through is reported (CML5997, "could not tell the broker to resume"); the broker resumes the
        /// process by itself when its hold time passes. After the resume the attach is awaited up to
        /// <see cref="ChameleonSettings.HoldAttachTimeoutMs"/>; a real failure (after the usual attempts) is CML5997, a hung attach too.
        /// </para>
        /// </summary>
        public async Task<ReattachOutcome> OnRestartingAsync(
            int oldPid,
            int newPid,
            Func<Task<ControlResponse>> resume,
            CancellationToken cancellationToken = default(CancellationToken))
        {
            Observe();
            string[] oldEngines = null;
            lock (_gate)
            {
                if (_known.TryGetValue(oldPid, out Known oldKnown))
                {
                    oldEngines = oldKnown.Engines;
                }
            }

            Task<ReattachOutcome> attach = null;
            try
            {
                attach = OnRestartedAsync(oldPid, newPid, cancellationToken);
                await WaitForDebuggerOrGraceAsync(attach, newPid).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                _log("reattach: the attach to the held pid " + newPid + " failed: " + ex.Message);
                ReportRestartProblem(oldPid, newPid, ClientDiagnostics.ReattachFailed(oldPid, newPid, ex.Message));
                AllowLateAttach(oldPid, newPid, oldEngines);
                attach = null;
            }
            finally
            {
                await ResumeAsync(oldPid, newPid, resume).ConfigureAwait(false);
            }

            if (attach == null)
            {
                return ReattachOutcome.Failed;
            }

            try
            {
                int waitMs = Math.Max(1, _settings.HoldAttachTimeoutMs);
                var stopWaiting = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                Task timeout = _attachTimeout(TimeSpan.FromMilliseconds(waitMs), stopWaiting.Token);
                Task first = await Task.WhenAny(attach, timeout).ConfigureAwait(false);
                if (first == attach)
                {
                    stopWaiting.Cancel();
                    ReattachOutcome result = await attach.ConfigureAwait(false);
                    if (result == ReattachOutcome.Failed)
                    {
                        AllowLateAttach(oldPid, newPid, oldEngines);
                    }

                    return result;
                }

                // The process runs; the debugger still has not finished. Stop *waiting* only: the attach itself is not cancelled (it may finish
                // later, and then the process is attached after all). `restarted` may still try a normal late attach, queued behind it.
                _log("reattach: the debugger did not finish attaching to pid " + newPid + " within " + waitMs + " ms");
                ReportRestartProblem(oldPid, newPid, ClientDiagnostics.AttachTooSlow(oldPid, newPid, waitMs));
                AllowLateAttach(oldPid, newPid, oldEngines);
                var ignored = attach.ContinueWith(t => { var unobserved = t.Exception; }, TaskContinuationOptions.OnlyOnFaulted);
            }
            catch (OperationCanceledException)
            {
                _log("reattach: the attach to pid " + newPid + " was cancelled");
                ReportRestartProblem(oldPid, newPid, ClientDiagnostics.ReattachFailed(oldPid, newPid, "the attach was cancelled"));
                AllowLateAttach(oldPid, newPid, oldEngines);
            }
            catch (Exception ex)
            {
                _log("reattach: the attach to pid " + newPid + " failed: " + ex.Message);
                ReportRestartProblem(oldPid, newPid, ClientDiagnostics.ReattachFailed(oldPid, newPid, ex.Message));
                AllowLateAttach(oldPid, newPid, oldEngines);
            }

            return ReattachOutcome.Failed;
        }

        // The early attach did not work (failed, or has not finished in time): the `restarted` event must still get a normal late attach.
        // OnRestartedAsync marked the pair handled and forgot the old pid before it tried; take both back.
        private void AllowLateAttach(int oldPid, int newPid, string[] oldEngines)
        {
            lock (_gate)
            {
                _handled.Remove(oldPid + ">" + newPid);
                if (oldEngines != null && !_known.ContainsKey(oldPid))
                {
                    _known[oldPid] = new Known { Engines = oldEngines, GoneAt = _now() };
                }
            }
        }

        // Returns when the attach task finished (either way), the debugger lists the pid, or the grace period is over.
        private async Task WaitForDebuggerOrGraceAsync(Task attach, int newPid)
        {
            var stop = new CancellationTokenSource();
            try
            {
                Task grace = _resumeGrace(TimeSpan.FromMilliseconds(Math.Max(0, _settings.HoldResumeGraceMs)), stop.Token);
                while (!attach.IsCompleted && !grace.IsCompleted)
                {
                    if (IsDebugged(newPid))
                    {
                        _log("reattach: the debugger has pid " + newPid + "; resuming it");
                        return;
                    }

                    await Task.WhenAny(attach, grace, Task.Delay(50)).ConfigureAwait(false);
                }

                if (!attach.IsCompleted)
                {
                    _log("reattach: the debugger's attach to the held pid " + newPid + " has not returned within " + _settings.HoldResumeGraceMs +
                         " ms (it may wait for the process to run); resuming it");
                }
            }
            finally
            {
                stop.Cancel();
            }
        }

        private bool IsDebugged(int pid)
        {
            try
            {
                return _debugger.GetDebuggedProcesses().Any(p => p.Pid == pid);
            }
            catch (Exception)
            {
                return false;
            }
        }

        private async Task ResumeAsync(int oldPid, int newPid, Func<Task<ControlResponse>> resume)
        {
            try
            {
                ControlResponse answer = await resume().ConfigureAwait(false);
                if (answer == null || answer.Ok)
                {
                    _log("reattach: pid " + newPid + " resumed");
                }
                else if (answer.Error != null && answer.Error.Id == "CML5814")
                {
                    _log("reattach: the broker had resumed pid " + newPid + " already (" + answer.Error.Message + ")");   // its hold time passed first: harmless
                }
                else if (answer.Error != null && answer.Error.Id == "CML5820")
                {
                    // WP-43 (R-40): the broker took the request but did not confirm the resume in time (an older broker says CML5817 for this,
                    // which the branch below reports as a failed resume, as before). Nothing is known to have failed.
                    _log("reattach: the broker did not confirm in time that pid " + newPid + " runs: " + answer.Error.ToString());
                    ReportRestartProblem(oldPid, newPid, ClientDiagnostics.ResumeUnconfirmed(newPid, answer.Error.Message, _settings.RestartHoldMs));
                }
                else
                {
                    _log("reattach: restart-resume for pid " + newPid + " was refused: " + (answer.Error != null ? answer.Error.ToString() : "no detail"));
                    ReportRestartProblem(oldPid, newPid,
                        ClientDiagnostics.ResumeFailed(newPid, answer.Error != null ? answer.Error.Id + " " + answer.Error.Message : "the broker refused", _settings.RestartHoldMs));
                }
            }
            catch (ControlException ex) when (ex.Diagnostic.Id == ClientDiagIds.PipeLost && ex.TimedOut)
            {
                // WP-43 (R-40): no answer in time is not a dropped connection. The broker did not resume the process for this client's
                // disconnect (the connection is there), so it may stay suspended until the hold time passes: the user has to know.
                _log("reattach: restart-resume for pid " + newPid + " got no answer in time: " + ex.Message);
                ReportRestartProblem(oldPid, newPid, ClientDiagnostics.ResumeFailed(newPid, "the broker did not answer restart-resume in time; the process may stay suspended", _settings.RestartHoldMs));
            }
            catch (ControlException ex) when (ex.Diagnostic.Id == ClientDiagIds.PipeLost)
            {
                // The connection dropped: the broker notices at once and resumes the process (CML5816). Nothing to tell the user.
                _log("reattach: the connection dropped while resuming pid " + newPid + " (the broker resumes a held process when its client disconnects): " + ex.Message);
            }
            catch (Exception ex)
            {
                _log("reattach: restart-resume for pid " + newPid + " failed: " + ex.Message);
                ReportRestartProblem(oldPid, newPid, ClientDiagnostics.ResumeFailed(newPid, ex.Message, _settings.RestartHoldMs));
            }
        }

        /// <summary>Reads the debugger's processes and updates what is remembered. Cheap; call it whenever the debugger's state changes.</summary>
        public void Observe()
        {
            IReadOnlyList<DebuggedProcess> now;
            try
            {
                now = _debugger.GetDebuggedProcesses();
            }
            catch (Exception ex)
            {
                _log("reattach: the debugger's process list is not available: " + ex.Message);
                return;
            }

            lock (_gate)
            {
                DateTime t = _now();
                var present = new HashSet<int>();
                foreach (DebuggedProcess p in now)
                {
                    present.Add(p.Pid);
                    _known[p.Pid] = new Known { Engines = p.Engines.ToArray(), GoneAt = null };
                }

                foreach (KeyValuePair<int, Known> pair in _known.ToList())
                {
                    if (present.Contains(pair.Key))
                    {
                        continue;
                    }

                    if (pair.Value.GoneAt == null)
                    {
                        pair.Value.GoneAt = t;
                    }
                    else if (t - pair.Value.GoneAt.Value > _settings.GoneProcessMemory)
                    {
                        _known.Remove(pair.Key);
                    }
                }
            }
        }

        public async Task<ReattachOutcome> OnRestartedAsync(int oldPid, int newPid, CancellationToken cancellationToken = default(CancellationToken))
        {
            await _serial.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                string key = oldPid + ">" + newPid;
                if (!_settings.ReattachOnRestart)
                {
                    _log("reattach: pid " + oldPid + " was restarted as " + newPid + "; automatic re-attach is off");
                    return ReattachOutcome.Disabled;
                }

                lock (_gate)
                {
                    if (_handled.Contains(key))
                    {
                        return ReattachOutcome.Duplicate;
                    }
                }

                Observe();
                string[] engines;
                lock (_gate)
                {
                    if (_known.TryGetValue(newPid, out Known already) && already.GoneAt == null)
                    {
                        _handled.Add(key);
                        _log("reattach: the debugger is attached to the new process " + newPid + " already");
                        return ReattachOutcome.AlreadyAttached;
                    }

                    if (!_known.TryGetValue(oldPid, out Known old))
                    {
                        _log("reattach: the debugger was not attached to pid " + oldPid + "; nothing to attach to " + newPid);
                        return ReattachOutcome.NotDebugged;
                    }

                    engines = old.Engines;
                    _handled.Add(key);
                    _known.Remove(oldPid);
                }

                int attempts = Math.Max(1, _settings.ReattachAttempts);
                string lastError = "unknown error";
                for (int attempt = 1; attempt <= attempts; attempt++)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    try
                    {
                        _log("reattach: attaching the debugger to pid " + newPid + " (engines: " + (engines.Length == 0 ? "automatic" : string.Join(", ", engines)) +
                             "), attempt " + attempt + " of " + attempts);
                        await _debugger.AttachAsync(newPid, engines, cancellationToken).ConfigureAwait(false);
                        Observe();
                        _log("reattach: attached to pid " + newPid);
                        return ReattachOutcome.Attached;
                    }
                    catch (OperationCanceledException)
                    {
                        throw;
                    }
                    catch (Exception ex)
                    {
                        lastError = ex.Message;
                        _log("reattach: attempt " + attempt + " failed: " + ex.Message);
                    }

                    if (attempt < attempts)
                    {
                        await _delay(TimeSpan.FromMilliseconds(Math.Max(0, _settings.ReattachRetryDelayMs)), cancellationToken).ConfigureAwait(false);
                        Observe();
                        lock (_gate)
                        {
                            if (_known.TryGetValue(newPid, out Known attachedMeanwhile) && attachedMeanwhile.GoneAt == null)
                            {
                                return ReattachOutcome.AlreadyAttached;
                            }
                        }
                    }
                }

                ReportRestartProblem(oldPid, newPid, ClientDiagnostics.ReattachFailed(oldPid, newPid, lastError));
                return ReattachOutcome.Failed;
            }
            finally
            {
                _serial.Release();
            }
        }
    }
}
