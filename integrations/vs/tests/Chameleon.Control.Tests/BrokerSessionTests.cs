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
    }
}
