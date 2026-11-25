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
            public bool BlockUntilResumed;                          // WP-34: attaching returns only once the held process runs (a real attach to a suspended process)
            public bool FailWhileHeld;                              // WP-34: every attach fails until the process has been resumed
            public TaskCompletionSource<bool> ManualRelease;        // WP-34: attaching returns when the test completes this
            public bool TokenCancelledAtEnd;                        // the attach's token was cancelled when it finished
            public bool ListEarly;                                  // ... but the debugger lists the pid as soon as the attach starts
            public readonly TaskCompletionSource<bool> ResumeSignal = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            public readonly List<string> Order = new List<string>();   // "attach:<pid>", "resume:<pid>" in the order they happened

            public IReadOnlyList<DebuggedProcess> GetDebuggedProcesses()
            {
                if (ListThrows)
                {
                    throw new InvalidOperationException("debugger busy");
                }

                return Attached.ToList();
            }

            public async Task AttachAsync(int pid, IReadOnlyList<string> engines, CancellationToken cancellationToken)
            {
                AttachCalls.Add((pid, engines.ToArray()));
                Order.Add("attach:" + pid);
                if (BlockUntilResumed)
                {
                    if (ListEarly)
                    {
                        Attached.Add(new DebuggedProcess(pid, engines));
                    }

                    await ResumeSignal.Task.ConfigureAwait(false);
                }

                if (FailWhileHeld && !ResumeSignal.Task.IsCompleted)
                {
                    throw new InvalidOperationException("the debugger cannot attach to a process that has not run");
                }

                if (ManualRelease != null)
                {
                    await ManualRelease.Task.ConfigureAwait(false);
                    TokenCancelledAtEnd = cancellationToken.IsCancellationRequested;
                }

                if (Hang)
                {
                    await new TaskCompletionSource<bool>().Task.ConfigureAwait(false);
                }

                if (FailTimes > 0)
                {
                    FailTimes--;
                    throw new InvalidOperationException(FailMessage);
                }

                if (!Attached.Any(p => p.Pid == pid))
                {
                    Attached.Add(new DebuggedProcess(pid, engines));
                }
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
            public bool GraceFires;           // WP-34: the grace period before the resume is over at once
            public readonly ReattachCoordinator Coordinator;

            public Rig()
            {
                Coordinator = new ReattachCoordinator(
                    Debugger, Settings, d => Reported.Add(d), m => Log.Add(m), () => Now, (span, ct) => { Delays++; return Task.CompletedTask; },
                    (span, ct) => AttachTimeoutFires ? Task.CompletedTask : Task.Delay(Timeout.Infinite, ct),
                    (span, ct) => GraceFires ? Task.CompletedTask : Task.Delay(Timeout.Infinite, ct));
            }

            /// <summary>What `restart-resume` does: records the call and answers like the broker.</summary>
            public Func<Task<ControlResponse>> Resume(ControlResponse answer = null, Exception throws = null)
            {
                return () =>
                {
                    Debugger.Order.Add("resume:200");
                    Debugger.ResumeSignal.TrySetResult(true);
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
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));   // the debugger is on the process to be restarted
            Assert.Equal(expected, rig.Coordinator.ShouldHoldNewProcess(brokerHasRestartHold, 100));
        }

        // ---- WP-43 (R-40): the WP-34 review follow-ups -----------------------------------------------------------------------------

        [Fact]
        public void TheRestartIsNotHeldWhenTheDebuggerIsNotOnTheProcessToBeRestarted()
        {
            var rig = new Rig();
            Assert.False(rig.Coordinator.ShouldHoldNewProcess(true, 100));       // no debugger at all (before WP-43: true)
            rig.Debugger.Attached.Add(new DebuggedProcess(300, new[] { "Native" }));
            Assert.False(rig.Coordinator.ShouldHoldNewProcess(true, 100));       // the debugger is on another process
            Assert.False(rig.Coordinator.ShouldHoldNewProcess(true, null));      // the broker picks the process: unknown, not held
            Assert.True(rig.Coordinator.ShouldHoldNewProcess(true, 300));
        }

        [Fact]
        public async Task AFailedEarlyAttachAndAFailedLateAttachAreReportedOnce()
        {
            var rig = new Rig();
            rig.Debugger.FailTimes = 99;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            rig.Coordinator.Observe();
            rig.Debugger.ProcessExits(100);
            rig.Coordinator.Observe();

            Assert.Equal(ReattachOutcome.Failed, await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume()));
            Assert.Equal(ReattachOutcome.Failed, await rig.Coordinator.OnRestartedAsync(100, 200));   // the late attach is tried, and fails too

            ChameleonDiagnostic d = Assert.Single(rig.Reported);   // before WP-43: two CML5997 for one restart
            Assert.Equal("CML5997", d.Id);
            Assert.Contains(rig.Log, m => m.Contains("reported for this restart already"));
        }

        [Fact]
        public async Task AFailedAttachAndAFailedResumeOfOneRestartAreOneReport()
        {
            var rig = new Rig();
            rig.Debugger.FailTimes = 99;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume(throws: new InvalidOperationException("the request could not be sent")));
            await rig.Coordinator.OnRestartedAsync(100, 200);

            ChameleonDiagnostic d = Assert.Single(rig.Reported);   // before WP-43: up to three
            Assert.Equal("CML5997", d.Id);
            // A different restart is reported again.
            rig.Debugger.Attached.Add(new DebuggedProcess(300, new[] { "Native" }));
            await rig.Coordinator.OnRestartingAsync(300, 400, rig.Resume());
            Assert.Equal(2, rig.Reported.Count);
        }

        [Fact]
        public async Task ARestartResumeThatGetsNoAnswerInTimeIsReportedNotOnlyLogged()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            var timedOut = new ControlException(ClientDiagnostics.PipeLost("pipe", "no answer to 'restart-resume' within 20 s"), timedOut: true);

            await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume(throws: timedOut));

            ChameleonDiagnostic d = Assert.Single(rig.Reported);   // before WP-43: only logged, the process frozen until hold_ms
            Assert.Equal("CML5997", d.Id);
            Assert.Contains("did not answer restart-resume in time", d.Message);
            Assert.Contains("resumes the process by itself", d.Action);
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
            rig.GraceFires = true;
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

            await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume(throws: new InvalidOperationException("the request could not be sent")));

            ChameleonDiagnostic d = Assert.Single(rig.Reported);
            Assert.Equal("CML5997", d.Id);
            Assert.Contains("resume the restarted process", d.Message);
            Assert.Contains("resumes the process by itself", d.Action);
            Assert.Contains("30 s", d.Action);
        }

        [Fact]
        public async Task AConnectionThatDroppedDuringTheResumeIsOnlyLoggedTheBrokerResumesWhenItsClientIsGone()
        {
            var rig = new Rig();
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume(throws: new ControlException(ClientDiagnostics.PipeLost("pipe", "gone"))));

            Assert.Empty(rig.Reported);
            Assert.Contains(rig.Log, m => m.Contains("connection dropped"));
        }

        [Fact]
        public void TheDisplayedHoldTimeIsClampedToTheBrokersMaximum()
        {
            ChameleonDiagnostic d = ClientDiagnostics.ResumeFailed(200, "x", 600000);
            Assert.Contains("60 s", d.Action);
        }

        [Fact]
        public async Task AHeldAttachThatFailsStillGetsALateAttachWhenRestartedArrives()
        {
            var rig = new Rig();
            rig.Debugger.FailWhileHeld = true;   // the debugger cannot attach to a process that has not run
            rig.Settings.ReattachAttempts = 2;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));
            rig.Coordinator.Observe();
            rig.Debugger.ProcessExits(100);       // the broker terminated it
            rig.Coordinator.Observe();

            ReattachOutcome held = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Failed, held);
            Assert.Single(rig.Reported);   // one CML5997 for the failed early attach
            // The process runs now; the `restarted` event gets a normal late attach (not Duplicate).
            ReattachOutcome late = await rig.Coordinator.OnRestartedAsync(100, 200);
            Assert.Equal(ReattachOutcome.Attached, late);
            Assert.Equal(new[] { "Native" }, rig.Debugger.AttachCalls.Last().Engines);
            Assert.Single(rig.Reported);   // no second report
        }

        [Fact]
        public async Task AnAttachThatFinishesAfterTheTimeoutIsNotCancelledAndEndsUpAttached()
        {
            var rig = new Rig();
            rig.AttachTimeoutFires = true;
            rig.GraceFires = true;
            rig.Debugger.ManualRelease = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            ReattachOutcome outcome = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Failed, outcome);   // we stopped waiting
            Assert.Contains(rig.Reported, d => d.Message.Contains("did not finish attaching"));
            rig.Debugger.ManualRelease.SetResult(true);       // the debugger finishes after the timeout
            Assert.True(await WaitForAsync(() => rig.Debugger.Attached.Any(p => p.Pid == 200)));
            Assert.False(rig.Debugger.TokenCancelledAtEnd);   // it was not cancelled
            Assert.Equal(ReattachOutcome.AlreadyAttached, await rig.Coordinator.OnRestartedAsync(100, 200));
        }

        private static async Task<bool> WaitForAsync(Func<bool> condition, int timeoutMs = 5000)
        {
            DateTime until = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < until)
            {
                if (condition())
                {
                    return true;
                }

                await Task.Delay(10);
            }

            return condition();
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
        public void OnlyARestartThisExtensionAskedToHoldIsClaimedByItsToken()
        {
            var rig = new Rig();
            Assert.False(rig.Coordinator.TryClaimRestarting("anything"));   // nothing expected: another client's held process is not ours
            Assert.False(rig.Coordinator.TryClaimRestarting(null));

            ReattachCoordinator.HoldTicket ticket = rig.Coordinator.ExpectRestarting();
            Assert.False(string.IsNullOrEmpty(ticket.Token));
            Assert.False(rig.Coordinator.TryClaimRestarting("somebody-elses-token"));   // a pending ticket does not claim a foreign event
            Assert.False(rig.Coordinator.TryClaimRestarting(null));                     // nor one without a token
            Assert.True(rig.Coordinator.TryClaimRestarting(ticket.Token));
            Assert.False(rig.Coordinator.TryClaimRestarting(ticket.Token));              // once per restart
            rig.Coordinator.RestartingNoLongerExpected(ticket);                          // no effect after the claim

            ReattachCoordinator.HoldTicket unanswered = rig.Coordinator.ExpectRestarting();
            Assert.NotEqual(ticket.Token, unanswered.Token);
            rig.Coordinator.RestartingNoLongerExpected(unanswered);   // the restart failed before it announced anything
            Assert.False(rig.Coordinator.TryClaimRestarting(unanswered.Token));
        }

        [Fact]
        public async Task AnAttachThatOnlyReturnsOnceTheProcessRunsDoesNotDelayTheResume()
        {
            var rig = new Rig();
            rig.Debugger.BlockUntilResumed = true;   // what a real attach to a suspended process may do
            rig.GraceFires = true;                    // the debugger never lists the pid: the grace period ends the wait
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            ReattachOutcome outcome = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Attached, outcome);   // the attach finished after the resume
            Assert.Equal(new[] { "attach:200", "resume:200" }, rig.Debugger.Order);
            Assert.Empty(rig.Reported);                          // no CML5997: nothing failed
        }

        [Fact]
        public async Task TheResumeComesAsSoonAsTheDebuggerListsThePidWithoutWaitingForTheGrace()
        {
            var rig = new Rig();   // the grace period never ends in this test
            rig.Debugger.BlockUntilResumed = true;
            rig.Debugger.ListEarly = true;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            ReattachOutcome outcome = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Attached, outcome);
            Assert.Equal(new[] { "attach:200", "resume:200" }, rig.Debugger.Order);
            Assert.Empty(rig.Reported);
        }

        [Fact]
        public async Task ARealAttachFailureAfterTheResumeIsStillCml5997()
        {
            var rig = new Rig();
            rig.GraceFires = true;
            rig.Debugger.BlockUntilResumed = true;
            rig.Debugger.FailTimes = 99;
            rig.Settings.ReattachAttempts = 1;
            rig.Debugger.Attached.Add(new DebuggedProcess(100, new[] { "Native" }));

            ReattachOutcome outcome = await rig.Coordinator.OnRestartingAsync(100, 200, rig.Resume());

            Assert.Equal(ReattachOutcome.Failed, outcome);
            Assert.Equal("attach:200", rig.Debugger.Order[0]);
            Assert.Contains("resume:200", rig.Debugger.Order);
            ChameleonDiagnostic d = Assert.Single(rig.Reported);
            Assert.Equal("CML5997", d.Id);
            Assert.Contains("access is denied", d.Message);
        }
    }
}
