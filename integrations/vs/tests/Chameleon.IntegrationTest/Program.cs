using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Chameleon.Control;
using Newtonsoft.Json.Linq;

namespace Chameleon.IntegrationTest
{
    /// <summary>
    /// WP-31 integration test: the control client and the re-attach logic against a real cml_broker and the `basic` fixture.
    ///
    ///   status, subscribe       the session connects, sees the fixture's process with its module indexed, and subscribes
    ///   reload                  a scripted edit of logic.cpp, `reload`, the patch_applied event, the fixture prints value=2
    ///   revert                  `revert last`, the patch_reverted event, value=1 again
    ///   restart                 the edit again, `reload`, `restart`: the restarted event with a new pid that is running
    ///   reattach                a fake debugger "attached" to the old pid is asked to attach the new pid, with the same engines
    ///
    /// Prints greppable lines that start with WP31-IT and ends with "WP31-IT result=ok ..." (exit code 0) or result=FAILED (exit code 1).
    /// </summary>
    internal static class Program
    {
        private static int _checks;
        private static readonly List<string> Failures = new List<string>();

        private static void Say(string line)
        {
            Console.WriteLine(line);
            Console.Out.Flush();
        }

        private static bool Check(bool condition, string what)
        {
            _checks++;
            if (!condition)
            {
                Failures.Add(what);
                Say("WP31-IT CHECK-FAILED " + what);
            }

            return condition;
        }

        private static int Main(string[] args)
        {
            try
            {
                return RunAsync(args).GetAwaiter().GetResult();
            }
            catch (Exception ex)
            {
                Say("WP31-IT result=FAILED unhandled=" + ex.GetType().Name + ": " + ex.Message);
                Say(ex.ToString());
                return 1;
            }
        }

        private static string Arg(string[] args, string name)
        {
            int i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
        }

        private static async Task<int> RunAsync(string[] args)
        {
            string binDir = Arg(args, "--bin-dir");          // directory of cml_broker.exe, cml_agent.dll and fixture_basic.exe
            string source = Arg(args, "--fixture-source");   // tests/fixtures/basic/logic.cpp of the tree the fixture was built from
            string workDir = Arg(args, "--work-dir") ?? Path.Combine(Path.GetTempPath(), "cml_vs_it_" + Guid.NewGuid().ToString("N").Substring(0, 8));
            if (binDir == null || source == null)
            {
                Say("usage: Chameleon.IntegrationTest --bin-dir DIR --fixture-source logic.cpp [--work-dir DIR]");
                return 2;
            }

            Directory.CreateDirectory(workDir);
            string broker = Path.Combine(binDir, "cml_broker.exe");
            string fixture = Path.Combine(binDir, "fixture_basic.exe");
            foreach (string f in new[] { broker, fixture, Path.Combine(binDir, "cml_agent.dll"), source })
            {
                if (!File.Exists(f))
                {
                    Say("WP31-IT result=FAILED missing=" + f);
                    return 2;
                }
            }

            string unique = Process.GetCurrentProcess().Id + "_" + Environment.TickCount64;
            string agentPipe = PipeNames.Prefix + "cml_vs_it_" + unique;
            string controlPipe = PipeNames.Prefix + "cml_vs_itc_" + unique;
            string originalSource = File.ReadAllText(source);
            var children = new List<Process>();
            int newPidToKill = 0;
            BrokerSession session = null;
            try
            {
                // ---- the broker ------------------------------------------------------------------------------------------------
                Child brokerProc = Child.Start(
                    broker,
                    "--headless --pipe \"" + agentPipe + "\" --control-pipe \"" + controlPipe + "\" --log \"" + Path.Combine(workDir, "broker.log") + "\" --startup-timeout 300000 --verbose",
                    binDir, Path.Combine(workDir, "broker.stdout.txt"),
                    new Dictionary<string, string> { ["CML_SESSION_DIR"] = Path.Combine(workDir, "broker-session") });
                children.Add(brokerProc.Process);

                // ---- the session (this is the code the extension runs) ------------------------------------------------------
                var settings = new ChameleonSettings { PipeName = controlPipe, ConnectTimeoutMs = 500 };
                var events = new EventLog();
                var diagnostics = new ConcurrentQueue<ChameleonDiagnostic>();
                var debugger = new FakeDebugger();
                var reattachReports = new ConcurrentQueue<ChameleonDiagnostic>();
                var coordinator = new ReattachCoordinator(debugger, settings, reattachReports.Enqueue, m => Say("  reattach: " + m));
                session = new BrokerSession(new NamedPipeConnector(), settings, m => Say("  session: " + m));
                session.DiagnosticRaised += diagnostics.Enqueue;
                session.EventReceived += ev =>
                {
                    events.Add(ev);
                    if (ev.TryGetRestarted(out int o, out int n))
                    {
                        Task.Run(() => coordinator.OnRestartedAsync(o, n));
                    }
                };
                session.StartAutoConnect();

                if (!Check(await WaitFor(() => session.State == ConnectionState.Connected, 60000), "the session connected to the broker's control pipe"))
                {
                    return Finish(brokerProc);
                }

                StatusInfo status = session.LastStatus;
                Say("WP31-IT status=ok version=" + status.Version + " capabilities=" + string.Join(",", status.Capabilities));
                foreach (string cap in new[] { "status", "subscribe", "reload", "restart", "revert" })
                {
                    Check(status.Has(cap), "the broker advertises capability '" + cap + "'");
                }

                Check(diagnostics.IsEmpty, "no client diagnostic was raised while connecting (" + string.Join("; ", diagnostics.Select(d => d.ToString())) + ")");
                Say("WP31-IT subscribe=ok");

                // ---- the fixture -------------------------------------------------------------------------------------------------
                Child fixtureProc = Child.Start(
                    fixture, "--agent", binDir, Path.Combine(workDir, "fixture.stdout.txt"),
                    new Dictionary<string, string>
                    {
                        ["CML_PIPE_NAME"] = agentPipe,
                        ["CML_CONTROL_PIPE"] = controlPipe,
                        ["CML_AGENT_LOG"] = Path.Combine(workDir, "agent.log"),
                        ["CML_BROKER_PATH"] = Path.Combine(workDir, "no_such_broker.exe"),
                    });
                children.Add(fixtureProc.Process);
                int pid = fixtureProc.Process.Id;

                bool ready = await WaitFor(() =>
                {
                    try
                    {
                        StatusInfo s = session.RefreshStatusAsync().GetAwaiter().GetResult();
                        ProcessStatus p = s.Processes.FirstOrDefault(x => x.Pid == pid);
                        return p != null && p.Modules.Any(m => m.Enabled && m.Path.IndexOf("fixture_basic", StringComparison.OrdinalIgnoreCase) >= 0 && m.IndexState == "ready");
                    }
                    catch (ControlException)
                    {
                        return false;
                    }
                }, 120000, 500);
                if (!Check(ready, "the broker lists the fixture (pid " + pid + ") with its module enabled and indexed"))
                {
                    return Finish(brokerProc, fixtureProc);
                }

                Check(await fixtureProc.WaitForOutput(new Regex(@"TICK n=\d+ value=1 "), 30000), "the fixture prints value=1");
                Say("WP31-IT fixture=ok pid=" + pid);

                // The debugger "is attached" to the fixture, as it is under F5. Its processes are remembered while they live.
                debugger.Attach(pid, "Native");
                coordinator.Observe();

                // ---- reload ------------------------------------------------------------------------------------------------------
                File.WriteAllText(source, originalSource.Replace("return 1;", "return 2;"));
                Check(File.ReadAllText(source).Contains("return 2;"), "the scripted edit changed logic.cpp");
                int before = events.Count("patch_applied");
                ControlResponse reload = await session.ReloadAsync(pid, false);
                bool reloadOk = Check(reload.Ok, "reload answered ok (" + (reload.Error != null ? reload.Error.ToString() : "") + ")");
                int patched = reloadOk && reload.Result["patched"] != null ? (int)reload.Result["patched"] : -1;
                Check(patched >= 1, "reload patched at least one compiland (patched=" + patched + ")");
                bool appliedEvent = await WaitFor(() => events.Count("patch_applied") > before, 10000);
                Check(appliedEvent, "the patch_applied event arrived");
                bool value2 = await fixtureProc.WaitForOutput(new Regex(@"TICK n=\d+ value=2 "), 30000);
                Check(value2, "the fixture prints value=2 after the reload");
                Say("WP31-IT reload=" + (reloadOk && appliedEvent && value2 ? "ok" : "FAILED") + " patched=" + patched + " patch_applied_event=" + (appliedEvent ? 1 : 0) + " value=2");

                // ---- revert ------------------------------------------------------------------------------------------------------
                int revertedBefore = events.Count("patch_reverted");
                ControlResponse revert = await session.RevertAsync("last", pid);
                bool revertOk = Check(revert.Ok, "revert answered ok (" + (revert.Error != null ? revert.Error.ToString() : "") + ")");
                bool revertedEvent = await WaitFor(() => events.Count("patch_reverted") > revertedBefore, 10000);
                Check(revertedEvent, "the patch_reverted event arrived");
                fixtureProc.MarkOutput();
                bool value1 = await fixtureProc.WaitForOutput(new Regex(@"TICK n=\d+ value=1 "), 30000);
                Check(value1, "the fixture prints value=1 again after the revert");
                Say("WP31-IT revert=" + (revertOk && revertedEvent && value1 ? "ok" : "FAILED") + " patch_reverted_event=" + (revertedEvent ? 1 : 0) + " value=1");

                // ---- WP-35: a compile error reaches the Error List at the exact file and line (Wp35Check.cs) ------------------------
                Wp35Result wp35 = await Wp35Check.RunAsync(session, pid, source, File.ReadAllText(source), Say);
                Check(wp35.Ok, "WP-35 diag location: " + wp35.Problem);

                // ---- reload again, then restart ---------------------------------------------------------------------------------
                ControlResponse reload2 = await session.ReloadAsync(pid, false);
                Check(reload2.Ok, "the second reload answered ok (" + (reload2.Error != null ? reload2.Error.ToString() : "") + ")");
                fixtureProc.MarkOutput();
                Check(await fixtureProc.WaitForOutput(new Regex(@"TICK n=\d+ value=2 "), 30000), "the fixture prints value=2 after the second reload");

                coordinator.Observe();
                int restartedBefore = events.Count("restarted");
                ControlResponse restart = await session.RestartAsync(pid);
                bool restartOk = Check(restart.Ok, "restart answered ok (" + (restart.Error != null ? restart.Error.ToString() : "") + ")");
                int oldPid = restartOk && restart.Result["old_pid"] != null ? (int)restart.Result["old_pid"] : 0;
                int newPid = restartOk && restart.Result["new_pid"] != null ? (int)restart.Result["new_pid"] : 0;
                newPidToKill = newPid;
                Check(oldPid == pid, "the restart result names the old pid (" + oldPid + ")");
                Check(newPid > 0 && newPid != oldPid, "the restart result names a new pid (" + newPid + ")");

                bool restartedEvent = await WaitFor(() => events.Count("restarted") > restartedBefore, 15000);
                Check(restartedEvent, "the restarted event arrived");
                BrokerEvent restarted = events.Last("restarted");
                int evOld = 0, evNew = 0;
                Check(restarted != null && restarted.TryGetRestarted(out evOld, out evNew) && evOld == oldPid && evNew == newPid,
                    "the restarted event carries old_pid and new_pid of the result (" + evOld + " -> " + evNew + ")");

                bool alive = IsAlive(newPid);
                Check(alive, "the new process (pid " + newPid + ") is running");
                fixtureProc.MarkOutput();
                bool ticking = await fixtureProc.WaitForOutput(new Regex(@"TICK n=\d+ value=\d "), 30000);
                Check(ticking, "the new process prints TICK lines into the same output");
                bool oldGone = !IsAlive(oldPid) || fixtureProc.Process.HasExited;
                Check(oldGone, "the old process is gone");
                Say("WP31-IT restart=" + (restartOk && restartedEvent && alive && ticking ? "ok" : "FAILED") + " old_pid=" + oldPid + " new_pid=" + newPid + " restarted_event=" + (restartedEvent ? 1 : 0) +
                    " new_pid_alive=" + (alive ? 1 : 0) + " new_pid_ticking=" + (ticking ? 1 : 0));

                // ---- the re-attach decision, on the real event --------------------------------------------------------------------
                // (The process that was debugged under VS has ended by now; the fake debugger drops it like VS does.)
                debugger.ProcessExits(oldPid);
                coordinator.Observe();
                bool attached = await WaitFor(() => debugger.AttachCalls.Any(c => c.Pid == newPid), 15000);
                Check(attached, "the debugger was asked to attach the new pid");
                var calls = debugger.AttachCalls.Where(c => c.Pid == newPid).ToList();
                bool engines = calls.Count > 0 && calls[0].Engines.SequenceEqual(new[] { "Native" });
                Check(engines, "the same engines were requested");
                Check(calls.Count == 1, "the debugger was asked exactly once");
                Check(reattachReports.IsEmpty, "no CML5997 was reported");
                Say("WP31-IT reattach=" + (attached ? "ok" : "FAILED") + " attach_pid=" + newPid + " engines=" + (calls.Count > 0 ? string.Join(",", calls[0].Engines) : "-"));

                return Finish(brokerProc, fixtureProc);
            }
            finally
            {
                if (session != null)
                {
                    session.Dispose();
                }

                File.WriteAllText(source, originalSource);
                if (newPidToKill > 0)
                {
                    Kill(newPidToKill);
                }

                foreach (Process p in children)
                {
                    try
                    {
                        if (!p.HasExited)
                        {
                            p.Kill(true);
                        }
                    }
                    catch (Exception)
                    {
                        // already gone
                    }
                }
            }
        }

        private static int Finish(params Child[] children)
        {
            if (Failures.Count > 0)
            {
                foreach (Child c in children)
                {
                    Say("---- tail of " + c.Name + " ----");
                    Say(c.Tail(40));
                }
            }

            Say("WP31-IT result=" + (Failures.Count == 0 ? "ok" : "FAILED") + " checks=" + _checks + " failures=" + Failures.Count);
            return Failures.Count == 0 ? 0 : 1;
        }

        private static bool IsAlive(int pid)
        {
            try
            {
                using (Process p = Process.GetProcessById(pid))
                {
                    return !p.HasExited;
                }
            }
            catch (ArgumentException)
            {
                return false;
            }
        }

        private static void Kill(int pid)
        {
            try
            {
                using (Process p = Process.GetProcessById(pid))
                {
                    p.Kill(true);
                }
            }
            catch (Exception)
            {
                // gone already
            }
        }

        private static async Task<bool> WaitFor(Func<bool> condition, int timeoutMs, int pollMs = 50)
        {
            DateTime until = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < until)
            {
                if (condition())
                {
                    return true;
                }

                await Task.Delay(pollMs);
            }

            return condition();
        }

        private sealed class EventLog
        {
            private readonly List<BrokerEvent> _events = new List<BrokerEvent>();

            public void Add(BrokerEvent e)
            {
                lock (_events)
                {
                    _events.Add(e);
                }

                Say("  event: " + e.Body.ToString(Newtonsoft.Json.Formatting.None));
            }

            public int Count(string name)
            {
                lock (_events)
                {
                    return _events.Count(e => e.Name == name);
                }
            }

            public BrokerEvent Last(string name)
            {
                lock (_events)
                {
                    return _events.LastOrDefault(e => e.Name == name);
                }
            }
        }

        private sealed class FakeDebugger : IDebuggerAttach
        {
            private readonly object _gate = new object();
            private readonly List<DebuggedProcess> _attached = new List<DebuggedProcess>();
            public readonly ConcurrentQueue<(int Pid, string[] Engines)> Calls = new ConcurrentQueue<(int, string[])>();

            public IEnumerable<(int Pid, string[] Engines)> AttachCalls
            {
                get { return Calls.ToArray(); }
            }

            public void Attach(int pid, params string[] engines)
            {
                lock (_gate) { _attached.Add(new DebuggedProcess(pid, engines)); }
            }

            public void ProcessExits(int pid)
            {
                lock (_gate) { _attached.RemoveAll(p => p.Pid == pid); }
            }

            public IReadOnlyList<DebuggedProcess> GetDebuggedProcesses()
            {
                lock (_gate) { return _attached.ToList(); }
            }

            public Task AttachAsync(int pid, IReadOnlyList<string> engines, CancellationToken cancellationToken)
            {
                Calls.Enqueue((pid, engines.ToArray()));
                Attach(pid, engines.ToArray());
                return Task.CompletedTask;
            }
        }

        /// <summary>A started program whose output is collected (the new process of a restart keeps writing into the same pipe).</summary>
        private sealed class Child
        {
            private readonly StringBuilder _all = new StringBuilder();
            private readonly object _gate = new object();
            private string _logFile;
            private int _mark;

            public string Name { get; private set; }
            public Process Process { get; private set; }

            public static Child Start(string exe, string arguments, string workingDirectory, string logFile, Dictionary<string, string> environment)
            {
                var psi = new ProcessStartInfo(exe, arguments)
                {
                    WorkingDirectory = workingDirectory,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    RedirectStandardInput = true,
                };
                foreach (var kv in environment)
                {
                    psi.Environment[kv.Key] = kv.Value;
                }

                var child = new Child { Name = Path.GetFileName(exe), _logFile = logFile };
                child.Process = new Process { StartInfo = psi };
                child.Process.OutputDataReceived += (s, e) => child.Append(e.Data);
                child.Process.ErrorDataReceived += (s, e) => child.Append(e.Data);
                child.Process.Start();
                child.Process.BeginOutputReadLine();
                child.Process.BeginErrorReadLine();
                Say("  started " + child.Name + " pid " + child.Process.Id);
                return child;
            }

            private void Append(string line)
            {
                if (line == null)
                {
                    return;
                }

                lock (_gate)
                {
                    _all.Append(line).Append('\n');
                    try
                    {
                        File.AppendAllText(_logFile, line + "\n");
                    }
                    catch (IOException)
                    {
                        // the log is evidence, not a requirement
                    }
                }
            }

            /// <summary>Only output printed from now on counts for the next <see cref="WaitForOutput"/>.</summary>
            public void MarkOutput()
            {
                lock (_gate) { _mark = _all.Length; }
            }

            public async Task<bool> WaitForOutput(Regex pattern, int timeoutMs)
            {
                DateTime until = DateTime.UtcNow.AddMilliseconds(timeoutMs);
                while (DateTime.UtcNow < until)
                {
                    lock (_gate)
                    {
                        if (pattern.IsMatch(_all.ToString(_mark, _all.Length - _mark)))
                        {
                            return true;
                        }
                    }

                    await Task.Delay(50);
                }

                return false;
            }

            public string Tail(int lines)
            {
                lock (_gate)
                {
                    string[] all = _all.ToString().Split('\n');
                    return string.Join("\n", all.Skip(Math.Max(0, all.Length - lines)));
                }
            }
        }
    }
}
