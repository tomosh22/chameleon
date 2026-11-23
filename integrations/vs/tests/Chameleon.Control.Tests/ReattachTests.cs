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
            public bool Hang;                                       // WP-34: attaching never finishes (a slow debugger)
            public readonly List<string> Order = new List<string>();   // "attach:<pid>", "resume:<pid>" in the order they happened

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
                Order.Add("attach:" + pid);
                if (Hang)
                {
                    return new TaskCompletionSource<bool>().Task;
                }

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
            public bool AttachTimeoutFires;   // WP-34: the wait for a slow attach is over at once
            public readonly ReattachCoordinator Coordinator;

            public Rig()
            {
                Coordinator = new ReattachCoordinator(
                    Debugger, Settings, d => Reported.Add(d), m => Log.Add(m), () => Now, (span, ct) => { Delays++; return Task.CompletedTask; },
                    (span, ct) => AttachTimeoutFires ? Task.CompletedTask : Task.Delay(Timeout.Infinite, ct));
            }

            /// <summary>What `restart-resume` does: records the call and answers like the broker.</summary>
            public Func<Task<ControlResponse>> Resume(ControlResponse answer = null, Exception throws = null)
            {
                return () =>
                {
                    Debugger.Order.Add("resume:200");
                    if (throws != null)
                    {
                        throw throws;
                    }

                    return Task.FromResult(answer ?? new ControlResponse { Ok = true });
                };
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

        // ---- WP-34: the debugger attaches to a process the broker holds suspended, and the process is always resumed ----

        [Theory]
        [InlineData(true, true, true)]
        [InlineData(true, false, false)]   // the broker cannot hold: today's late re-attach
        [InlineData(false, true, false)]   // automatic re-attach is off: nothing to hold the process for
        [InlineData(false, false, false)]
        public void TheRestartIsHeldOnlyWhenReattachIsOnAndTheBrokerCan(bool reattachOn, bool brokerHasRestartHold, bool expected)
        {
            var rig = new Rig();
            rig.Settings.ReattachOnRestart = reattachOn;
            Assert.Equal(expected, rig.Coordinator.ShouldHoldNewProcess(brokerHasRestartHold));
        }

        [Fact]
        public async Task TheDebuggerAttachesToTheHeldProcessBeforeItIsResumed()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native", "Managed" }));
            rig.Coordinator.Observe();        // seen while it lived (the shell does it before sending the restart)
            rig.Debugger.ProcessExits(100);   // the broker terminated it before announcing the new one
            rig.Coordinator.Observe();

            ReattachOutcome outcome = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Attached, outcome);
            Assert.Equal(new[] { "attach:200", "resume:200" }, rig.Debugger.Order);   // attach first, then resume
            Assert.Equal(new[] { "Native", "Managed" }, rig.Debugger.AttachCalls[0].Engines);
            Assert.Empty(rig.Reported);
        }

        [Fact]
        public async Task TheLaterRestartedEventDoesNotAttachASecondTime()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Duplicate, await rig.Coordinator.OnRestartedAsync(100, 200));
            Assert.Single(rig.Debugger.AttachCalls);
        }

        [Fact]
        public async Task AProcessTheDebuggerWasNotAttachedToIsResumedWithoutAnAttach()
        {
            var rig = new Rig();
            ReattachOutcome outcome = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());
            Assert.Equal(ReattachOutcome.NotDebugged, outcome);
            Assert.Equal(new[] { "resume:200" }, rig.Debugger.Order);
            Assert.Empty(rig.Reported);
        }

        [Fact]
        public async Task AFailedAttachStillResumesAndIsReportedAsCml5997()
        {
            var rig = new Rig();
            rig.Debugger.FailTimes = 99;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            ReattachOutcome outcome = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Failed, outcome);
            Assert.Equal("resume:200", rig.Debugger.Order.Last());
            ChameleonDiagnostic d = Assert.Single(rig.Reported);
            Assert.Equal("CML5997", d.Id);
            Assert.Contains("access is denied", d.Message);
        }

        [Fact]
        public async Task ASlowAttachIsNotWaitedForForeverTheProcessIsResumedAndCml5997IsReported()
        {
            var rig = new Rig();
            rig.Debugger.Hang = true;
            rig.AttachTimeoutFires = true;
            rig.Settings.HoldAttachTimeoutMs = 20000;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            ReattachOutcome outcome = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Failed, outcome);
            Assert.Equal(new[] { "attach:200", "resume:200" }, rig.Debugger.Order);
            ChameleonDiagnostic d = Assert.Single(rig.Reported);
            Assert.Equal("CML5997", d.Id);
            Assert.Contains("did not finish attaching within 20 s", d.Message);
        }

        [Fact]
        public async Task ACancelledAttachStillResumes()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            using (var cts = new CancellationTokenSource())
            {
                cts.Cancel();
                await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume(), cts.Token);
            }

            Assert.Contains("resume:200", rig.Debugger.Order);
        }

        [Fact]
        public async Task ADebuggerThatThrowsStillGetsTheProcessResumed()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            rig.Debugger.ListThrows = false;
            rig.Settings.ReattachAttempts = 1;
            rig.Debugger.FailTimes = 1;
            await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());
            Assert.Equal("resume:200", rig.Debugger.Order.Last());
        }

        [Fact]
        public async Task WithReattachOffAHeldProcessIsResumedAtOnceAndNotAttached()
        {
            var rig = new Rig();
            rig.Settings.ReattachOnRestart = false;   // the user switched it off while the restart was under way
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            ReattachOutcome outcome = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Disabled, outcome);
            Assert.Equal(new[] { "resume:200" }, rig.Debugger.Order);
        }

        [Fact]
        public async Task AResumeThatDoesNotGetThroughIsReportedAndDoesNotThrow()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume(throws: new ControlException(ClientDiagnostics.PipeLost("pipe", "gone"))));

            ChameleonDiagnostic d = Assert.Single(rig.Reported);
            Assert.Equal("CML5997", d.Id);
            Assert.Contains("resume the restarted process", d.Message);
            Assert.Contains("resumes the process by itself", d.Action);
        }

        [Fact]
        public async Task ABrokerThatHadResumedTheProcessAlreadyIsNotAnError()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            var answer = new ControlResponse { Ok = false, Error = new BrokerError { Id = "CML5814", Message = "process 200 is not held any more", Action = "x" } };

            await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume(answer));

            Assert.Empty(rig.Reported);
            Assert.Contains(rig.Log, m => m.Contains("resumed pid 200 already"));
        }

        [Fact]
        public async Task AnyOtherRefusalOfTheResumeIsReported()
        {
            var rig = new Rig();
            var answer = new ControlResponse { Ok = false, Error = new BrokerError { Id = "CML5817", Message = "could not be resumed", Action = "x" } };

            await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume(answer));

            ChameleonDiagnostic d = Assert.Single(rig.Reported);
            Assert.Equal("CML5997", d.Id);
            Assert.Contains("CML5817", d.Message);
        }

        [Fact]
        public void OnlyARestartThisExtensionAskedToHoldIsClaimed()
        {
            var rig = new Rig();
            Assert.False(rig.Coordinator.TryClaimRestarting());   // another client's held process is not ours to resume

            object ticket = rig.Coordinator.ExpectRestarting();
            Assert.True(rig.Coordinator.TryClaimRestarting());
            Assert.False(rig.Coordinator.TryClaimRestarting());   // once per restart
            rig.Coordinator.RestartingNoLongerExpected(ticket);   // no effect after the claim
            Assert.False(rig.Coordinator.TryClaimRestarting());

            object unanswered = rig.Coordinator.ExpectRestarting();
            rig.Coordinator.RestartingNoLongerExpected(unanswered);   // the restart failed before it announced anything
            Assert.False(rig.Coordinator.TryClaimRestarting());
        }
    }
}
