using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using Xunit;

namespace Chameleon.Control.Tests
{
    public class BrokerSessionTests
    {
        private sealed class Harness : IDisposable
        {
            public readonly ChameleonSettings Settings = new ChameleonSettings { ConnectTimeoutMs = 300 };
            public readonly List<ChameleonDiagnostic> Diagnostics = new List<ChameleonDiagnostic>();
            public readonly List<BrokerEvent> Events = new List<BrokerEvent>();
            public readonly BrokerSession Session;

            public Harness(FakeBroker broker)
            {
                Settings.PipeName = broker.FullName;
                Session = new BrokerSession(new NamedPipeConnector(), Settings, _ => { }, retryDelay: _ => TimeSpan.FromMilliseconds(30));
                Session.DiagnosticRaised += d => { lock (Diagnostics) { Diagnostics.Add(d); } };
                Session.EventReceived += e => { lock (Events) { Events.Add(e); } };
            }

            public List<string> DiagIds()
            {
                lock (Diagnostics) { return Diagnostics.Select(d => d.Id).ToList(); }
            }

            public void Dispose()
            {
                Session.Dispose();
            }
        }

        private static async Task<bool> WaitAsync(Func<bool> condition, int timeoutMs = 5000)
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

        private static string[] SubscribedEvents(FakeBroker broker)
        {
            JObject sub = broker.Requests().First(r => (string)r["cmd"] == "subscribe");
            return sub["args"]["events"].Select(e => (string)e).ToArray();
        }

        [Fact]
        public async Task ConnectChecksStatusThenSubscribesToTheEventsThePaneShowsPlusRestarted()
        {
            using (var broker = new FakeBroker())
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                Assert.Equal(ConnectionState.Connected, h.Session.State);
                Assert.True(h.Session.Supports("reload"));
                Assert.True(h.Session.Supports("restart"));
                Assert.False(h.Session.Supports("nonsense"));
                Assert.Equal("0.1.0", h.Session.LastStatus.Version);
                Assert.Equal(4242, h.Session.LastStatus.Processes[0].Pid);

                List<string> order = broker.Requests().Select(r => (string)r["cmd"]).ToList();
                Assert.Equal(new[] { "status", "subscribe" }, order);
                Assert.Equal(new[] { "diag", "patch_applied", "patch_reverted", "restarted" }, SubscribedEvents(broker));
                Assert.Empty(h.DiagIds());
            }
        }

        [Fact]
        public async Task ExceptionEventsAreSubscribedOnlyWhenAskedForAndOffered()
        {
            using (var broker = new FakeBroker("status", "subscribe", "restart", "exceptions"))
            using (var h = new Harness(broker))
            {
                h.Settings.ShowExceptionEvents = true;
                Assert.True(await h.Session.ConnectAsync());
                Assert.Contains("exception", SubscribedEvents(broker));
            }

            using (var broker = new FakeBroker("status", "subscribe", "restart", "exceptions"))
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                Assert.DoesNotContain("exception", SubscribedEvents(broker));
            }

            using (var broker = new FakeBroker("status", "subscribe"))
            using (var h = new Harness(broker))
            {
                h.Settings.ShowExceptionEvents = true;
                Assert.True(await h.Session.ConnectAsync());
                Assert.DoesNotContain("exception", SubscribedEvents(broker));
                Assert.DoesNotContain("restarted", SubscribedEvents(broker));
            }
        }

        [Fact]
        public async Task ABrokerWithoutSubscribeStaysConnectedForCommandsAndReportsCml5996()
        {
            using (var broker = new FakeBroker("status", "reload"))
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                Assert.Equal(new[] { ClientDiagIds.Unsupported }, h.DiagIds().ToArray());
                Assert.True(h.Session.Supports("reload"));
                Assert.False(h.Session.Supports("subscribe"));
                Assert.DoesNotContain(broker.Requests(), r => (string)r["cmd"] == "subscribe");
            }
        }

        [Fact]
        public async Task ABrokerThatRefusesAnEventNameGetsTheDefaultSubscription()
        {
            using (var broker = new FakeBroker())
            using (var h = new Harness(broker))
            {
                broker.RefuseEvent = "restarted";
                Assert.True(await h.Session.ConnectAsync());
                List<JObject> subs = broker.Requests().Where(r => (string)r["cmd"] == "subscribe").ToList();
                Assert.Equal(2, subs.Count);
                Assert.Null(subs[1]["args"]);
                Assert.Empty(h.DiagIds());
            }
        }

        [Fact]
        public async Task WithoutABrokerConnectFailsOnceWithCml5994AndKeepsTryingUntilOneAppears()
        {
            // The pipe name is fixed first; the broker comes later.
            string name = PipeNames.Prefix + "cml_vs_late_" + Guid.NewGuid().ToString("N").Substring(0, 10);
            var settings = new ChameleonSettings { PipeName = name, ConnectTimeoutMs = 100 };
            var diagnostics = new List<ChameleonDiagnostic>();
            using (var session = new BrokerSession(new NamedPipeConnector(), settings, _ => { }, retryDelay: _ => TimeSpan.FromMilliseconds(30)))
            {
                session.DiagnosticRaised += d => { lock (diagnostics) { diagnostics.Add(d); } };
                Assert.False(await session.ConnectAsync());
                Assert.True(session.WantsConnection);
                await Task.Delay(300);   // several failed attempts
                lock (diagnostics)
                {
                    Assert.Single(diagnostics);
                    Assert.Equal(ClientDiagIds.NoBroker, diagnostics[0].Id);
                }

                // Now a broker on that very name.
                using (var broker = new LateBroker(name))
                {
                    Assert.True(await WaitAsync(() => session.State == ConnectionState.Connected));
                }
            }
        }

        // FakeBroker picks its own name; this one serves a name chosen by the test.
        private sealed class LateBroker : IDisposable
        {
            private readonly FakeBroker _inner;

            public LateBroker(string fullName)
            {
                _inner = new FakeBroker(fullName, new string[0]);
            }

            public void Dispose() { _inner.Dispose(); }
        }

        [Fact]
        public async Task ALostConnectionIsReportedAndRestoredAutomatically()
        {
            using (var broker = new FakeBroker())
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                broker.Drop();
                Assert.True(await WaitAsync(() => h.DiagIds().Contains(ClientDiagIds.PipeLost)));
                Assert.True(await WaitAsync(() => h.Session.State == ConnectionState.Connected && broker.Connections == 2));
            }
        }

        [Fact]
        public async Task WithAutoReconnectOffALostConnectionStaysLostUntilConnectIsChosenAgain()
        {
            using (var broker = new FakeBroker())
            using (var h = new Harness(broker))
            {
                h.Settings.AutoReconnect = false;
                Assert.True(await h.Session.ConnectAsync());
                broker.Drop();
                Assert.True(await WaitAsync(() => h.Session.State == ConnectionState.Disconnected && h.DiagIds().Contains(ClientDiagIds.PipeLost)));
                await Task.Delay(300);
                Assert.Equal(ConnectionState.Disconnected, h.Session.State);
                Assert.Equal(1, broker.Connections);
                Assert.False(h.Session.WantsConnection);

                Assert.True(await h.Session.ConnectAsync());
                Assert.Equal(2, broker.Connections);
            }
        }

        [Fact]
        public async Task DisconnectClosesTheConnectionAndStopsTrying()
        {
            using (var broker = new FakeBroker())
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                h.Session.Disconnect();
                Assert.Equal(ConnectionState.Disconnected, h.Session.State);
                Assert.False(h.Session.WantsConnection);
                await Task.Delay(200);
                Assert.Equal(1, broker.Connections);
                ControlException ex = await Assert.ThrowsAsync<ControlException>(() => h.Session.ReloadAsync(null, false));
                Assert.Equal(ClientDiagIds.NoBroker, ex.Diagnostic.Id);
            }
        }

        [Fact]
        public async Task CommandsCarryTheArgumentsTheBrokerDocumentsAndEventsReachTheSubscriber()
        {
            using (var broker = new FakeBroker())
            using (var h = new Harness(broker))
            {
                broker.Respond = r => new[] { FakeBroker.Ok((long)r["id"], new JObject()) };
                Assert.True(await h.Session.ConnectAsync());

                await h.Session.ReloadAsync(4242, true);
                await h.Session.RestartAsync(4242);
                await h.Session.RevertAsync("all", null);
                await h.Session.RevertAsync("last", 4242);

                List<JObject> r = broker.Requests().Skip(2).ToList();
                Assert.Equal("reload", (string)r[0]["cmd"]);
                Assert.Equal(4242, (int)r[0]["args"]["pid"]);
                Assert.True((bool)r[0]["args"]["confirm"]);
                Assert.Equal("restart", (string)r[1]["cmd"]);
                Assert.Equal(4242, (int)r[1]["args"]["pid"]);
                Assert.Equal("revert", (string)r[2]["cmd"]);
                Assert.Equal("all", (string)r[2]["args"]["scope"]);
                Assert.Null(r[2]["args"]["pid"]);
                Assert.Equal("last", (string)r[3]["args"]["scope"]);

                broker.Push("{\"event\":\"patch_applied\",\"pid\":4242,\"module\":\"app.exe\",\"generation\":1}");
                Assert.True(await WaitAsync(() => { lock (h.Events) { return h.Events.Count == 1; } }));
            }
        }

        [Fact]
        public void TheTargetIsAProcessTheDebuggerIsAttachedToAndTheBrokerKnows()
        {
            var status = new StatusInfo();
            status.Processes.Add(new ProcessStatus { Pid = 10 });
            status.Processes.Add(new ProcessStatus { Pid = 20 });
            Assert.Equal(20, BrokerSession.ChooseTarget(status, new[] { 99, 20 }));
            Assert.Null(BrokerSession.ChooseTarget(status, new[] { 99 }));
            Assert.Null(BrokerSession.ChooseTarget(status, new int[0]));
            Assert.Null(BrokerSession.ChooseTarget(null, new[] { 10 }));
        }

        [Fact]
        public async Task ARetryingSessionAnnouncesOnlyRealTransitions()
        {
            string name = PipeNames.Prefix + "cml_vs_quiet_" + Guid.NewGuid().ToString("N").Substring(0, 10);
            var settings = new ChameleonSettings { PipeName = name, ConnectTimeoutMs = 50 };
            var states = new List<ConnectionState>();
            var diagnostics = new List<ChameleonDiagnostic>();
            using (var session = new BrokerSession(new NamedPipeConnector(), settings, _ => { }, retryDelay: _ => TimeSpan.FromMilliseconds(20)))
            {
                session.StateChanged += s => { lock (states) { states.Add(s); } };
                session.DiagnosticRaised += d => { lock (diagnostics) { diagnostics.Add(d); } };
                Assert.False(await session.ConnectAsync());
                await Task.Delay(400);   // many silent retries
                lock (states) { Assert.Equal(new[] { ConnectionState.Connecting, ConnectionState.Disconnected }, states.ToArray()); }
                lock (diagnostics) { Assert.Single(diagnostics); }
            }
        }

        [Fact]
        public async Task EveryFailureIdIsReportedOncePerEpisode()
        {
            using (var broker = new FakeBroker())
            using (var h = new Harness(broker))
            {
                broker.StatusError = true;   // not a Chameleon broker: CML5996 on every attempt
                Assert.False(await h.Session.ConnectAsync());
                await Task.Delay(400);
                Assert.Equal(new[] { ClientDiagIds.Unsupported }, h.DiagIds().ToArray());
                Assert.True(broker.Connections > 2);

                // A successful connect ends the episode: the next failure is reported again.
                broker.StatusError = false;
                Assert.True(await WaitAsync(() => h.Session.State == ConnectionState.Connected));
                broker.StatusError = true;
                broker.Drop();
                Assert.True(await WaitAsync(() => h.DiagIds().Count(i => i == ClientDiagIds.Unsupported) == 2));
            }
        }

        [Fact]
        public async Task APipeLostRightAfterTheSubscribeAnswerLeavesTheSessionDisconnected()
        {
            using (var broker = new FakeBroker())
            using (var h = new Harness(broker))
            {
                h.Settings.AutoReconnect = false;
                broker.DropAfter = "subscribe";
                await h.Session.ConnectAsync();
                Assert.True(await WaitAsync(() => h.Session.State == ConnectionState.Disconnected && h.DiagIds().Contains(ClientDiagIds.PipeLost)));
                await Task.Delay(200);
                Assert.Equal(ConnectionState.Disconnected, h.Session.State);
                Assert.False(h.Session.Supports("reload"));
            }
        }

        private sealed class GatedConnector : IPipeConnector
        {
            public readonly TaskCompletionSource<bool> Release = new TaskCompletionSource<bool>();
            public readonly TaskCompletionSource<bool> Entered = new TaskCompletionSource<bool>();
            private int _calls;

            public async Task<System.IO.Stream> ConnectAsync(string fullPipeName, int timeoutMs, CancellationToken cancellationToken)
            {
                if (Interlocked.Increment(ref _calls) == 1)
                {
                    Entered.TrySetResult(true);
                    await Release.Task;
                }

                return await new NamedPipeConnector().ConnectAsync(fullPipeName, timeoutMs, cancellationToken);
            }
        }

        [Fact]
        public async Task ADisconnectDuringAnAttemptMakesThatAttemptStaleAndTheNextConnectWorks()
        {
            using (var broker = new FakeBroker())
            {
                var settings = new ChameleonSettings { PipeName = broker.FullName, ConnectTimeoutMs = 500 };
                var gate = new GatedConnector();
                using (var session = new BrokerSession(gate, settings, _ => { }, retryDelay: _ => TimeSpan.FromMilliseconds(20)))
                {
                    Task<bool> first = session.ConnectAsync();
                    await gate.Entered.Task;
                    session.Disconnect();
                    Assert.Equal(ConnectionState.Disconnected, session.State);

                    // A new attempt while the stale one is still in flight.
                    Assert.True(await session.ConnectAsync());
                    Assert.Equal(ConnectionState.Connected, session.State);

                    gate.Release.SetResult(true);
                    Assert.False(await first);   // stale: it did not install its client
                    Assert.Equal(ConnectionState.Connected, session.State);
                    Assert.True(session.Supports("reload"));
                    ControlResponse r = await session.ReloadAsync(null, false);
                    Assert.True(r.Ok);
                }
            }
        }

        [Fact]
        public async Task StopAutoConnectStopsOnlyWhatAutoConnectStarted()
        {
            string name = PipeNames.Prefix + "cml_vs_auto_" + Guid.NewGuid().ToString("N").Substring(0, 10);
            var settings = new ChameleonSettings { PipeName = name, ConnectTimeoutMs = 50 };
            using (var session = new BrokerSession(new NamedPipeConnector(), settings, _ => { }, retryDelay: _ => TimeSpan.FromMilliseconds(20)))
            {
                session.StartAutoConnect();
                Assert.True(session.WantsConnection);
                session.StopAutoConnect();
                Assert.False(session.WantsConnection);

                session.StartAutoConnect();
                await session.ConnectAsync();     // the user chose Connect
                session.StopAutoConnect();
                Assert.True(session.WantsConnection);
            }
        }

        // ---- WP-34: restart {hold_new}, `restarting`, `restart-resume` ----

        [Fact]
        public async Task RestartingIsSubscribedOnlyWhenTheBrokerCanHoldTheProcess()
        {
            using (var broker = new FakeBroker("status", "subscribe", "restart", "restart-hold"))
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                Assert.True(h.Session.Supports(Capabilities.RestartHold));
                Assert.Contains("restarting", SubscribedEvents(broker));
                Assert.Contains("restarted", SubscribedEvents(broker));
            }

            using (var broker = new FakeBroker("status", "subscribe", "restart"))
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                Assert.False(h.Session.Supports(Capabilities.RestartHold));
                Assert.DoesNotContain("restarting", SubscribedEvents(broker));
            }
        }

        [Fact]
        public async Task ARestartWithoutAHoldTimeSendsNoHoldArguments()
        {
            using (var broker = new FakeBroker("status", "subscribe", "restart", "restart-hold"))
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                await h.Session.RestartAsync(4242);
                JObject sent = broker.Requests().Last(r => (string)r["cmd"] == "restart");
                Assert.Equal(4242, (int)sent["args"]["pid"]);
                Assert.Null(sent["args"]["hold_new"]);
                Assert.Null(sent["args"]["hold_ms"]);
            }
        }

        [Fact]
        public async Task ResumeGoesOutWhileTheHeldRestartIsStillUnansweredAndTheHoldTimeIsClamped()
        {
            using (var broker = new FakeBroker("status", "subscribe", "restart", "restart-hold"))
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                // The broker answers `restart` only after the resume (it waits for it), exactly like the real one.
                broker.Respond = req => (string)req["cmd"] == "restart"
                    ? new string[0]
                    : new[] { FakeBroker.Ok((long)req["id"], new JObject { ["pid"] = 200, ["resumed"] = true }) };

                Task<ControlResponse> restart = h.Session.RestartAsync(4242, holdMs: 120000, holdToken: "tok-1");
                await broker.WaitForRequestAsync("restart");
                JObject sent = broker.Requests().Last(r => (string)r["cmd"] == "restart");
                Assert.True((bool)sent["args"]["hold_new"]);
                Assert.Equal(60000, (int)sent["args"]["hold_ms"]);   // the broker's maximum
                Assert.Equal(4242, (int)sent["args"]["pid"]);
                Assert.Equal("tok-1", (string)sent["args"]["token"]);   // echoed in `restarting`

                ControlResponse resumed = await h.Session.ResumeAsync(200);   // does not wait behind the unanswered restart
                Assert.True(resumed.Ok);
                Assert.False(restart.IsCompleted);
                JObject resume = broker.Requests().Last(r => (string)r["cmd"] == "restart-resume");
                Assert.Equal(200, (int)resume["args"]["pid"]);

                broker.Push(FakeBroker.Ok((long)sent["id"], new JObject { ["old_pid"] = 4242, ["new_pid"] = 200 }));
                ControlResponse done = await restart;
                Assert.True(done.Ok);
                Assert.Equal(200, (int)done.Result["new_pid"]);
            }
        }

        [Fact]
        public async Task TheRestartingEventCarriesBothPids()
        {
            using (var broker = new FakeBroker("status", "subscribe", "restart", "restart-hold"))
            using (var h = new Harness(broker))
            {
                Assert.True(await h.Session.ConnectAsync());
                broker.Push("{\"event\":\"restarting\",\"old_pid\":4242,\"new_pid\":200,\"hold_ms\":30000}");
                Assert.True(await WaitAsync(() => { lock (h.Events) { return h.Events.Any(e => e.Name == "restarting"); } }));
                BrokerEvent ev;
                lock (h.Events) { ev = h.Events.First(e => e.Name == "restarting"); }
                int oldPid, newPid;
                Assert.True(ev.TryGetRestarting(out oldPid, out newPid));
                Assert.Equal(4242, oldPid);
                Assert.Equal(200, newPid);
                Assert.False(ev.TryGetRestarted(out oldPid, out newPid));   // not mistaken for `restarted`
            }
        }
    }
}