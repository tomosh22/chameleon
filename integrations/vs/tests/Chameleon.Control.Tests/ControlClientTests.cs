using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using Xunit;

namespace Chameleon.Control.Tests
{
    public class ControlClientTests
    {
        private static async Task<ControlClient> ConnectAsync(FakeBroker broker)
        {
            var stream = await new NamedPipeConnector().ConnectAsync(broker.FullName, 5000, CancellationToken.None);
            var client = new ControlClient(stream, broker.FullName);
            return client;
        }

        [Fact]
        public async Task CallSendsOneJsonLineWithIdCommandAndArgsAndReturnsTheResult()
        {
            using (var broker = new FakeBroker())
            {
                broker.Respond = r => new[] { FakeBroker.Ok((long)r["id"], new JObject { ["generation"] = 3 }) };
                using (ControlClient client = await ConnectAsync(broker))
                {
                    client.Start();
                    ControlResponse first = await client.CallAsync("reload", new JObject { ["pid"] = 7 }, 5000, CancellationToken.None);
                    ControlResponse second = await client.CallAsync("reload", null, 5000, CancellationToken.None);

                    Assert.True(first.Ok);
                    Assert.Equal(3, (int)first.Result["generation"]);
                    Assert.True(second.Id > first.Id);

                    List<JObject> seen = broker.Requests();
                    Assert.Equal("reload", (string)seen[0]["cmd"]);
                    Assert.Equal(7, (int)seen[0]["args"]["pid"]);
                    Assert.Null(seen[1]["args"]);
                }
            }
        }

        [Fact]
        public async Task ErrorResponsesCarryTheBrokersDiagnostic()
        {
            using (var broker = new FakeBroker())
            {
                broker.Respond = r => new[] { FakeBroker.Error((long)r["id"], "CML5304", "a reload is already running") };
                using (ControlClient client = await ConnectAsync(broker))
                {
                    client.Start();
                    ControlResponse r = await client.CallAsync("reload", null, 5000, CancellationToken.None);
                    Assert.False(r.Ok);
                    Assert.Equal("CML5304", r.Error.Id);
                    Assert.Equal("a reload is already running", r.Error.Message);
                    Assert.Equal("do something", r.Error.Action);
                }
            }
        }

        [Fact]
        public async Task EventsBetweenAndBeforeAnswersAreDeliveredAndDoNotBreakTheAnswer()
        {
            using (var broker = new FakeBroker())
            {
                broker.Respond = r => new[]
                {
                    "{\"event\":\"patch_applied\",\"pid\":1,\"module\":\"a.exe\",\"generation\":2}",
                    "this is not json",
                    "{\"id\":99999,\"ok\":true,\"result\":{}}",   // an answer nobody waits for
                    FakeBroker.Ok((long)r["id"], new JObject()),
                    "{\"event\":\"restarted\",\"old_pid\":10,\"new_pid\":11}",
                };
                var events = new List<BrokerEvent>();
                using (ControlClient client = await ConnectAsync(broker))
                {
                    client.EventReceived += e => { lock (events) { events.Add(e); } };
                    client.Start();
                    ControlResponse r = await client.CallAsync("restart", null, 5000, CancellationToken.None);
                    Assert.True(r.Ok);

                    DateTime until = DateTime.UtcNow.AddSeconds(5);
                    while (DateTime.UtcNow < until)
                    {
                        lock (events) { if (events.Count == 2) { break; } }
                        await Task.Delay(10);
                    }

                    lock (events)
                    {
                        Assert.Equal(2, events.Count);
                        Assert.Equal("patch_applied", events[0].Name);
                        Assert.Equal(2, events[0].Int("generation"));
                        Assert.True(events[1].TryGetRestarted(out int oldPid, out int newPid));
                        Assert.Equal(10, oldPid);
                        Assert.Equal(11, newPid);
                    }
                }
            }
        }

        [Fact]
        public async Task NoAnswerWithinTheTimeoutIsCml5995AndTheClientStaysUsable()
        {
            using (var broker = new FakeBroker())
            {
                int calls = 0;
                broker.Respond = r => Interlocked.Increment(ref calls) == 1
                    ? new string[0]
                    : new[] { FakeBroker.Ok((long)r["id"], new JObject { ["late"] = false }) };
                using (ControlClient client = await ConnectAsync(broker))
                {
                    client.Start();
                    ControlException ex = await Assert.ThrowsAsync<ControlException>(() => client.CallAsync("reload", null, 200, CancellationToken.None));
                    Assert.Equal(ClientDiagIds.PipeLost, ex.Diagnostic.Id);
                    Assert.Contains("no answer", ex.Diagnostic.Message);
                    Assert.Contains("What to do", ex.Diagnostic.ToString());

                    ControlResponse again = await client.CallAsync("reload", null, 5000, CancellationToken.None);
                    Assert.True(again.Ok);
                }
            }
        }

        [Fact]
        public async Task ABrokerThatGoesAwayFailsTheCallAndRaisesConnectionLost()
        {
            using (var broker = new FakeBroker())
            {
                broker.Respond = r => new string[0];
                var lost = new TaskCompletionSource<ChameleonDiagnostic>();
                using (ControlClient client = await ConnectAsync(broker))
                {
                    client.ConnectionLost += d => lost.TrySetResult(d);
                    client.Start();
                    Task<ControlResponse> pending = client.CallAsync("reload", null, 10000, CancellationToken.None);
                    await broker.WaitForRequestAsync("reload");
                    broker.Drop();

                    ControlException ex = await Assert.ThrowsAsync<ControlException>(() => pending);
                    Assert.Equal(ClientDiagIds.PipeLost, ex.Diagnostic.Id);
                    Assert.Same(lost.Task, await Task.WhenAny(lost.Task, Task.Delay(5000)));
                    ChameleonDiagnostic diagnostic = await lost.Task;
                    Assert.Equal(ClientDiagIds.PipeLost, diagnostic.Id);
                    Assert.False(client.IsOpen);

                    await Assert.ThrowsAsync<ControlException>(() => client.CallAsync("status", null, 1000, CancellationToken.None));
                }
            }
        }

        [Fact]
        public async Task ANonexistentPipeIsCml5994()
        {
            ControlException ex = await Assert.ThrowsAsync<ControlException>(
                () => new NamedPipeConnector().ConnectAsync(@"\\.\pipe\cml_vs_does_not_exist_" + Guid.NewGuid().ToString("N"), 150, CancellationToken.None));
            Assert.Equal(ClientDiagIds.NoBroker, ex.Diagnostic.Id);
            Assert.Contains("What to do", ex.Diagnostic.ToString());
        }
    }
}
