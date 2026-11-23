using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace Chameleon.Control.Tests
{
    public class ReattachTests
    {
        private sealed class FakeDebugger : IDebuggerAttach
        {
            public readonly List<DebuggedProcess> Attached = new List<DebuggedProcess>();
            public readonly List<(int Pid, string[] Engines)> AttachCalls = new List<(int, string[])>();
            public int FailTimes;
            public string FailMessage = "access is denied";
            public bool ListThrows;

            public IReadOnlyList<DebuggedProcess> GetDebuggedProcesses()
            {
                if (ListThrows)
                {
                    throw new InvalidOperationException("debugger busy");
                }

                return Attached.ToList();
            }

            public Task AttachAsync(int pid, IReadOnlyList<string> engines, CancellationToken cancellationToken)
            {
                AttachCalls.Add((pid, engines.ToArray()));
                if (FailTimes > 0)
                {
                    FailTimes--;
                    throw new InvalidOperationException(FailMessage);
                }

                Attached.Add(new DebuggedProcess(pid, engines));
                return Task.CompletedTask;
            }

            // What the debugger does when the debuggee is terminated.
            public void ProcessExits(int pid)
            {
                Attached.RemoveAll(p => p.Pid == pid);
            }
        }

        private sealed class Rig
        {
            public readonly FakeDebugger Debugger = new FakeDebugger();
            public readonly ChameleonSettings Settings = new ChameleonSettings { ReattachRetryDelayMs = 0 };
            public readonly List<ChameleonDiagnostic> Reported = new List<ChameleonDiagnostic>();
            public readonly List<string> Log = new List<string>();
            public DateTime Now = new DateTime(2026, 1, 1, 12, 0, 0, DateTimeKind.Utc);
            public int Delays;
            public readonly ReattachCoordinator Coordinator;

            public Rig()
            {
                Coordinator = new ReattachCoordinator(
                    Debugger, Settings, d => Reported.Add(d), m => Log.Add(m), () => Now, (span, ct) => { Delays++; return Task.CompletedTask; });
            }
        }

        [Fact]
        public async Task AttachedToTheOldPidAttachesTheNewOneWithTheSameEngines()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native", "Managed (.NET Core, .NET 5+)" }));

            ReattachOutcome outcome = await rig.Coordinator.OnRestartedAsync(100, 200);

            Assert.Equal(ReattachOutcome.Attached, outcome);
            Assert.Single(rig.Debugger.AttachCalls);
            Assert.Equal(200, rig.Debugger.AttachCalls[0].Pid);
            Assert.Equal(new[] { "Native", "Managed (.NET Core, .NET 5+)" }, rig.Debugger.AttachCalls[0].Engines);
            Assert.Empty(rig.Reported);
        }

        [Fact]
        public async Task NotAttachedToTheOldPidDoesNothing()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(555, new[] { "Native" }));   // some other process

            ReattachOutcome outcome = await rig.Coordinator.OnRestartedAsync(100, 200);

            Assert.Equal(ReattachOutcome.NotDebugged, outcome);
            Assert.Empty(rig.Debugger.AttachCalls);
            Assert.Empty(rig.Reported);
        }

        [Fact]
        public async Task NoDebuggerAtAllDoesNothing()
        {
            var rig = new Rig();
            Assert.Equal(ReattachOutcome.NotDebugged, await rig.Coordinator.OnRestartedAsync(100, 200));
            Assert.Empty(rig.Debugger.AttachCalls);
        }

        [Fact]
        public async Task TheOldProcessIsAlreadyGoneWhenTheEventArrivesButWasObservedWhileItLived()
        {
            // The broker terminates the old process first; Visual Studio ends that debug session; only then does `restarted` arrive.
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            rig.Coordinator.Observe();
            rig.Debugger.ProcessExits(100);
            rig.Coordinator.Observe();
            rig.Now += TimeSpan.FromSeconds(90);

            ReattachOutcome outcome = await rig.Coordinator.OnRestartedAsync(100, 200);

            Assert.Equal(ReattachOutcome.Attached, outcome);
            Assert.Equal(200, rig.Debugger.AttachCalls[0].Pid);
            Assert.Equal(new[] { "Native" }, rig.Debugger.AttachCalls[0].Engines);
        }

        [Fact]
        public async Task AProcessThatLeftTheDebuggerLongAgoIsForgotten()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            rig.Coordinator.Observe();
            rig.Debugger.ProcessExits(100);
            rig.Coordinator.Observe();
            rig.Now += rig.Settings.GoneProcessMemory + TimeSpan.FromSeconds(1);

            Assert.Equal(ReattachOutcome.NotDebugged, await rig.Coordinator.OnRestartedAsync(100, 200));
            Assert.Empty(rig.Debugger.AttachCalls);
        }

        [Fact]
        public async Task TheSettingTurnsReattachOff()
        {
            var rig = new Rig();
            rig.Settings.ReattachOnRestart = false;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            Assert.Equal(ReattachOutcome.Disabled, await rig.Coordinator.OnRestartedAsync(100, 200));
            Assert.Empty(rig.Debugger.AttachCalls);
            Assert.Contains(rig.Log, m => m.Contains("off"));
        }

        [Fact]
        public async Task AFailureIsCml5997WithAWhatToDoSentenceAfterTheConfiguredAttempts()
        {
            var rig = new Rig();
            rig.Settings.ReattachAttempts = 3;
            rig.Debugger.FailTimes = 99;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            ReattachOutcome outcome = await rig.Coordinator.OnRestartedAsync(100, 200);

            Assert.Equal(ReattachOutcome.Failed, outcome);
            Assert.Equal(3, rig.Debugger.AttachCalls.Count);
            Assert.Equal(2, rig.Delays);
            ChameleonDiagnostic d = Assert.Single(rig.Reported);
            Assert.Equal("CML5997", d.Id);
            Assert.Contains("200", d.Message);
            Assert.Contains("100", d.Message);
            Assert.Contains("access is denied", d.Message);
            Assert.Contains("Attach to Process", d.Action);
            Assert.Contains("Re-attach debugger after hot-restart", d.Action);
        }

        [Fact]
        public async Task ASecondAttemptThatWorksReportsNothing()
        {
            var rig = new Rig();
            rig.Debugger.FailTimes = 1;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            Assert.Equal(ReattachOutcome.Attached, await rig.Coordinator.OnRestartedAsync(100, 200));
            Assert.Equal(2, rig.Debugger.AttachCalls.Count);
            Assert.Empty(rig.Reported);
        }

        [Fact]
        public async Task ANewProcessTheDebuggerAlreadyDebugsIsLeftAlone()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            rig.Debugger.Attached.Add(new DebuggedProcess(200, new[] { "Native" }));

            Assert.Equal(ReattachOutcome.AlreadyAttached, await rig.Coordinator.OnRestartedAsync(100, 200));
            Assert.Empty(rig.Debugger.AttachCalls);
        }

        [Fact]
        public async Task TheEventAndTheCommandAnswerForOneRestartAttachOnce()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            Task<ReattachOutcome> a = rig.Coordinator.OnRestartedAsync(100, 200);
            Task<ReattachOutcome> b = rig.Coordinator.OnRestartedAsync(100, 200);
            ReattachOutcome[] outcomes = await Task.WhenAll(a, b);

            Assert.Single(rig.Debugger.AttachCalls);
            Assert.Contains(ReattachOutcome.Attached, outcomes);
            Assert.Contains(ReattachOutcome.Duplicate, outcomes);
        }

        [Fact]
        public async Task ASecondRestartOfTheNewProcessAttachesAgain()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native", "Managed" }));

            Assert.Equal(ReattachOutcome.Attached, await rig.Coordinator.OnRestartedAsync(100, 200));
            rig.Debugger.ProcessExits(100);
            rig.Debugger.ProcessExits(200);
            // 200 was observed while it lived (OnRestartedAsync observes after attaching), then it was restarted as 300.
            Assert.Equal(ReattachOutcome.Attached, await rig.Coordinator.OnRestartedAsync(200, 300));
            Assert.Equal(new[] { "Native", "Managed" }, rig.Debugger.AttachCalls[1].Engines);
        }

        [Fact]
        public async Task ADebuggerThatCannotListProcessesIsNotACrash()
        {
            var rig = new Rig();
            rig.Debugger.ListThrows = true;
            Assert.Equal(ReattachOutcome.NotDebugged, await rig.Coordinator.OnRestartedAsync(100, 200));
            Assert.Contains(rig.Log, m => m.Contains("not available"));
        }

        [Fact]
        public async Task AutomaticEngineSelectionIsKeptWhenTheEnginesAreUnknown()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new string[0]));
            Assert.Equal(ReattachOutcome.Attached, await rig.Coordinator.OnRestartedAsync(100, 200));
            Assert.Empty(rig.Debugger.AttachCalls[0].Engines);
        }
    }
}
