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
        private readonly object _gate = new object();
        private readonly Dictionary<int, Known> _known = new Dictionary<int, Known>();
        private readonly HashSet<string> _handled = new HashSet<string>();
        private readonly SemaphoreSlim _serial = new SemaphoreSlim(1, 1);

        public ReattachCoordinator(
            IDebuggerAttach debugger,
            ChameleonSettings settings,
            Action<ChameleonDiagnostic> report,
            Action<string> log,
            Func<DateTime> now = null,
            Func<TimeSpan, CancellationToken, Task> delay = null)
        {
            _debugger = debugger;
            _settings = settings;
            _report = report ?? (_ => { });
            _log = log ?? (_ => { });
            _now = now ?? (() => DateTime.UtcNow);
            _delay = delay ?? ((span, ct) => Task.Delay(span, ct));
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

                _report(ClientDiagnostics.ReattachFailed(oldPid, newPid, lastError));
                return ReattachOutcome.Failed;
            }
            finally
            {
                _serial.Release();
            }
        }
    }
}
