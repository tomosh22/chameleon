using System;
using System.Collections.Generic;
using System.ComponentModel.Design;
using System.Linq;
using System.Threading.Tasks;
using Chameleon.Control;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.Threading;
using Newtonsoft.Json.Linq;

namespace Chameleon.Vsix
{
    /// <summary>
    /// Wires the plain-.NET logic (<see cref="BrokerSession"/>, <see cref="ReattachCoordinator"/>) to Visual Studio: the commands, the
    /// Output pane, the Error List and the debugger. Everything with real logic is in Chameleon.Control and is unit-tested there.
    /// </summary>
    internal sealed class ChameleonController : IDisposable
    {
        private readonly ChameleonPackage _package;
        private readonly OutputPane _output;
        private readonly ErrorListSink _errors;
        private readonly VsDebuggerAttach _debugger;
        private readonly ChameleonSettings _settings;
        private readonly Func<bool> _persistToggle;
        private readonly DiagnosticDedupe _shown = new DiagnosticDedupe(TimeSpan.FromSeconds(10));

        public ChameleonController(
            ChameleonPackage package,
            OutputPane output,
            ErrorListSink errors,
            VsDebuggerAttach debugger,
            ChameleonSettings settings,
            Func<bool> persistToggle)
        {
            _package = package;
            _output = output;
            _errors = errors;
            _debugger = debugger;
            _settings = settings;
            _persistToggle = persistToggle;

            Session = new BrokerSession(new NamedPipeConnector(), settings, _output.WriteLine);
            Reattach = new ReattachCoordinator(debugger, settings, ReportDiagnostic, _output.WriteLine);
            Session.DiagnosticRaised += ReportDiagnostic;
            // Only real transitions arrive here (BrokerSession keeps the retry loop silent).
            Session.StateChanged += state => _output.WriteLine("broker: " + state.ToString().ToLowerInvariant());
            Session.EventReceived += OnBrokerEvent;
            debugger.Changed += Reattach.Observe;
        }

        public BrokerSession Session { get; }
        public ReattachCoordinator Reattach { get; }

        // ---- events ------------------------------------------------------------------------------------------------------------

        private void OnBrokerEvent(BrokerEvent ev)
        {
            OutputLine line = EventFormatter.Format(ev);
            if (line != null)
            {
                Write(line);
                _shown.Remember(line);

                if (ev.Name == "diag" || ev.Name == "patch_applied")
                {
                    bool clear = ev.Name == "patch_applied";
                    _package.JoinableTaskFactory.RunAsync(async () =>
                    {
                        await _package.JoinableTaskFactory.SwitchToMainThreadAsync();
                        if (clear)
                        {
                            _errors.Clear();
                        }
                        else
                        {
                            _errors.Add(line);
                        }
                    });
                }
            }

            int oldPid, newPid;
            if (ev.TryGetRestarting(out oldPid, out newPid))
            {
                // WP-34: the broker holds the new process suspended for the restart this extension asked for. Attach the debugger before its
                // first instruction, then let it run. A held process that another client asked for is that client's to resume.
                if (Reattach.TryClaimRestarting())
                {
                    Task.Run(() => RestartingAsync(oldPid, newPid));
                }
            }
            else if (ev.TryGetRestarted(out oldPid, out newPid))
            {
                // Right away, on the reader's thread pool task: the new process runs while we wait.
                Task.Run(() => ReattachAsync(oldPid, newPid));
            }
        }

        private async Task RestartingAsync(int oldPid, int newPid)
        {
            try
            {
                _output.WriteLine("hot-restart: pid " + newPid + " is held before its first instruction; attaching the debugger ...");
                ReattachOutcome outcome = await Reattach.OnRestartingAsync(oldPid, newPid, () => Session.ResumeAsync(newPid)).ConfigureAwait(false);
                switch (outcome)
                {
                    case ReattachOutcome.Attached:
                        _output.WriteLine("debugger attached to pid " + newPid + " before it ran (it was attached to pid " + oldPid + "); pid " + newPid + " resumed");
                        break;
                    case ReattachOutcome.NotDebugged:
                        _output.WriteLine("the debugger was not attached to pid " + oldPid + "; pid " + newPid + " resumed");
                        break;
                    default:
                        _output.WriteLine("pid " + newPid + " resumed (" + outcome.ToString().ToLowerInvariant() + ")");
                        break;
                }
            }
            catch (Exception ex)
            {
                // OnRestartingAsync resumes the process on every path itself; this is the last net.
                ReportDiagnostic(ClientDiagnostics.ReattachFailed(oldPid, newPid, ex.Message));
            }
        }

        private async Task ReattachAsync(int oldPid, int newPid)
        {
            try
            {
                ReattachOutcome outcome = await Reattach.OnRestartedAsync(oldPid, newPid).ConfigureAwait(false);
                switch (outcome)
                {
                    case ReattachOutcome.Attached:
                        _output.WriteLine("debugger re-attached to pid " + newPid + " (it was attached to pid " + oldPid + ")");
                        break;
                    case ReattachOutcome.AlreadyAttached:
                        _output.WriteLine("debugger is attached to pid " + newPid + " already");
                        break;
                    case ReattachOutcome.Disabled:
                        _output.WriteLine("automatic re-attach is off (Tools > Chameleon > Re-attach debugger after hot-restart); attach to pid " + newPid + " by hand if you need the debugger");
                        break;
                }
            }
            catch (Exception ex)
            {
                ReportDiagnostic(ClientDiagnostics.ReattachFailed(oldPid, newPid, ex.Message));
            }
        }

        private void Write(OutputLine line)
        {
            if (line.Location != null)
            {
                _output.WriteNavigable(line.Text);
            }
            else
            {
                _output.WriteLine(line.Text);
            }
        }

        private void ReportDiagnostic(ChameleonDiagnostic diagnostic)
        {
            OutputLine line = EventFormatter.Format(diagnostic);
            Write(line);
            _package.JoinableTaskFactory.RunAsync(async () =>
            {
                await _package.JoinableTaskFactory.SwitchToMainThreadAsync();
                _errors.Add(line);
            });
        }

        private void WriteBrokerError(BrokerError error)
        {
            OutputLine line = EventFormatter.Format(error);
            if (!_shown.SeenRecently(line))
            {
                Write(line);
            }
        }

        // ---- commands ----------------------------------------------------------------------------------------------------------

        public void RegisterCommands(OleMenuCommandService service)
        {
            Add(service, PackageIds.CmdReload, ReloadAsync, () => Session.Supports(Capabilities.Reload));
            Add(service, PackageIds.CmdHotRestart, HotRestartAsync, () => Session.Supports(Capabilities.Restart));
            Add(service, PackageIds.CmdRevertLast, () => RevertAsync("last"), () => Session.Supports(Capabilities.Revert));
            Add(service, PackageIds.CmdRevertAll, () => RevertAsync("all"), () => Session.Supports(Capabilities.Revert));
            Add(service, PackageIds.CmdConnect, ConnectAsync, () => Session.State == ConnectionState.Disconnected);
            Add(service, PackageIds.CmdDisconnect, DisconnectAsync, () => Session.State != ConnectionState.Disconnected || Session.WantsConnection);
            Add(service, PackageIds.CmdToggleReattach, ToggleReattachAsync, () => true, () => _settings.ReattachOnRestart);
            Add(service, PackageIds.CmdShowOutput, ShowOutputAsync, () => true);
        }

        private void Add(OleMenuCommandService service, int id, Func<Task> run, Func<bool> enabled, Func<bool> isChecked = null)
        {
            var commandId = new CommandID(new Guid(PackageGuids.CommandSetString), id);
            var command = new OleMenuCommand((sender, args) => Run(run), commandId);
            command.BeforeQueryStatus += (sender, args) =>
            {
                var c = (OleMenuCommand)sender;
                c.Visible = true;
                c.Enabled = enabled();
                if (isChecked != null)
                {
                    c.Checked = isChecked();
                }
            };
            service.AddCommand(command);
        }

        private void Run(Func<Task> work)
        {
            _package.JoinableTaskFactory.RunAsync(async () =>
            {
                try
                {
                    await work();
                }
                catch (ControlException ex)
                {
                    ReportDiagnostic(ex.Diagnostic);
                }
                catch (Exception ex)
                {
                    _output.WriteLine("error: " + ex.Message);
                }
            });
        }

        private async Task ConnectAsync()
        {
            await _package.JoinableTaskFactory.SwitchToMainThreadAsync();
            _output.Activate();
            await TaskScheduler.Default;
            _output.WriteLine("connecting to the Chameleon broker ...");
            await Session.ConnectAsync();
        }

        private Task DisconnectAsync()
        {
            Session.Disconnect();
            _output.WriteLine("disconnected from the Chameleon broker");
            return Task.CompletedTask;
        }

        private async Task ShowOutputAsync()
        {
            await _package.JoinableTaskFactory.SwitchToMainThreadAsync();
            _output.Activate();
        }

        private async Task ToggleReattachAsync()
        {
            await _package.JoinableTaskFactory.SwitchToMainThreadAsync();
            _settings.ReattachOnRestart = !_settings.ReattachOnRestart;
            _persistToggle();
            _output.WriteLine("automatic debugger re-attach after a hot-restart is now " + (_settings.ReattachOnRestart ? "on" : "off"));
        }

        // The pid a command should name: a process the debugger is attached to and the broker knows.
        private async Task<int?> TargetPidAsync()
        {
            Reattach.Observe();
            StatusInfo status = await Session.RefreshStatusAsync().ConfigureAwait(false);
            return BrokerSession.ChooseTarget(status, _debugger.GetDebuggedProcesses().Select(p => p.Pid));
        }

        private async Task ReloadAsync()
        {
            await _package.JoinableTaskFactory.SwitchToMainThreadAsync();
            _output.Activate();
            await TaskScheduler.Default;
            int? pid = await TargetPidAsync();
            _output.WriteLine("reload" + (pid.HasValue ? " (pid " + pid.Value + ")" : "") + " ...");
            ControlResponse r = await Session.ReloadAsync(pid, false);
            if (!r.Ok && r.Error != null && r.Error.Id == "CML5551")
            {
                // More compilands than the broker's limit: it asks before it recompiles that many.
                WriteBrokerError(r.Error);
                await _package.JoinableTaskFactory.SwitchToMainThreadAsync();
                bool yes = VsShellUtilities.ShowMessageBox(
                    _package, r.Error.Message + "\n\nRecompile them all?", "Chameleon", Microsoft.VisualStudio.Shell.Interop.OLEMSGICON.OLEMSGICON_QUERY,
                    Microsoft.VisualStudio.Shell.Interop.OLEMSGBUTTON.OLEMSGBUTTON_YESNO, Microsoft.VisualStudio.Shell.Interop.OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_SECOND) == 6;
                await TaskScheduler.Default;
                if (!yes)
                {
                    return;
                }

                r = await Session.ReloadAsync(pid, true);
            }

            if (r.Ok)
            {
                JToken patched = r.Result["patched"];
                JToken generation = r.Result["generation"];
                JToken total = r.Result["timing_ms"] != null ? r.Result["timing_ms"]["total"] : null;
                _output.WriteLine("reload ok: patched " + (patched ?? "0") + ", generation " + (generation ?? "?") + (total != null ? ", " + total + " ms" : ""));
            }
            else
            {
                WriteBrokerError(r.Error);
            }
        }

        private async Task HotRestartAsync()
        {
            await _package.JoinableTaskFactory.SwitchToMainThreadAsync();
            _output.Activate();
            await TaskScheduler.Default;
            int? pid = await TargetPidAsync();   // also remembers the debugged processes while the old one is still alive
            // WP-34: with re-attach on and a broker that can hold the new process, the debugger attaches before the process runs its first
            // instruction (`restarting`, then `restart-resume`); otherwise it attaches after `restarted`, as before.
            bool hold = Reattach.ShouldHoldNewProcess(Session.Supports(Capabilities.RestartHold));
            _output.WriteLine("hot-restart" + (pid.HasValue ? " (pid " + pid.Value + ")" : "") + (hold ? ", holding the new process for the debugger" : "") + " ...");
            ControlResponse r;
            object ticket = hold ? Reattach.ExpectRestarting() : null;

            try
            {
                r = await Session.RestartAsync(pid, holdMs: hold ? (int?)_settings.RestartHoldMs : null);
            }
            finally
            {
                if (ticket != null)
                {
                    Reattach.RestartingNoLongerExpected(ticket);
                }
            }

            if (!r.Ok)
            {
                WriteBrokerError(r.Error);
                return;
            }

            long oldPid = Json.Long(r.Result["old_pid"]) ?? 0;
            long newPid = Json.Long(r.Result["new_pid"]) ?? 0;
            JToken ms = r.Result["restart_ms"];
            _output.WriteLine("hot-restart ok: pid " + oldPid + " is now pid " + newPid + (ms != null ? ", " + ms + " ms" : ""));
            if (oldPid > 0 && newPid > 0)
            {
                // The `restarted` event normally got there first; the same restart is never handled twice.
                await ReattachAsync((int)oldPid, (int)newPid).ConfigureAwait(false);
            }
        }

        private async Task RevertAsync(string scope)
        {
            await _package.JoinableTaskFactory.SwitchToMainThreadAsync();
            _output.Activate();
            await TaskScheduler.Default;
            int? pid = await TargetPidAsync();
            ControlResponse r = await Session.RevertAsync(scope, pid);
            if (!r.Ok)
            {
                WriteBrokerError(r.Error);
                return;
            }

            var reverted = r.Result["reverted"] as JArray;
            _output.WriteLine("revert (" + scope + ") ok: " + (reverted != null ? reverted.Count : 0) + " module(s) reverted, generation " + (r.Result["generation"] ?? "?"));
        }

        public void Dispose()
        {
            _debugger.Changed -= Reattach.Observe;
            Session.Dispose();
        }
    }
}
