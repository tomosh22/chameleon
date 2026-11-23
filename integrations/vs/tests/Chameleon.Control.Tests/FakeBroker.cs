using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;

namespace Chameleon.Control.Tests
{
    /// <summary>
    /// A broker stand-in on a real named pipe: accepts one connection after another, answers `status` and `subscribe` like the real one
    /// (the capability list is the test's choice), and lets a test script anything else.
    /// </summary>
    internal sealed class FakeBroker : IDisposable
    {
        private readonly string _name;
        private readonly CancellationTokenSource _stop = new CancellationTokenSource();
        private readonly object _gate = new object();
        private readonly List<JObject> _requests = new List<JObject>();
        private NamedPipeServerStream _current;
        private readonly Task _acceptLoop;

        public FakeBroker(params string[] capabilities)
            : this(PipeNames.Prefix + "cml_vs_test_" + Guid.NewGuid().ToString("N").Substring(0, 12), capabilities)
        {
        }

        /// <summary>A broker on a pipe name the test chose (the name must carry the \\.\pipe\ prefix).</summary>
        public FakeBroker(string fullPipeName, string[] capabilities)
        {
            _name = PipeNames.ToClientName(fullPipeName);
            Capabilities = capabilities.Length > 0 ? capabilities : new[] { "status", "reload", "wait", "set-option", "subscribe", "diag-events", "revert", "restart" };
            _acceptLoop = Task.Run(AcceptLoopAsync);
        }

        public string FullName
        {
            get { return PipeNames.Prefix + _name; }
        }

        public string[] Capabilities { get; set; }

        /// <summary>Lines to send for a request other than status and subscribe (default: ok with an empty result).</summary>
        public Func<JObject, string[]> Respond { get; set; }

        /// <summary>Make `subscribe` fail when it names this event (older broker).</summary>
        public string RefuseEvent { get; set; }

        /// <summary>Close the connection right after answering this command.</summary>
        public string DropAfter { get; set; }

        /// <summary>Answer `status` with an error (not a Chameleon broker).</summary>
        public bool StatusError { get; set; }

        public int Connections { get; private set; }

        public List<JObject> Requests()
        {
            lock (_gate)
            {
                return new List<JObject>(_requests);
            }
        }

        public async Task WaitForRequestAsync(string command, int timeoutMs = 5000)
        {
            DateTime until = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < until)
            {
                if (Requests().Exists(r => (string)r["cmd"] == command))
                {
                    return;
                }

                await Task.Delay(10);
            }

            throw new TimeoutException("the broker never received '" + command + "'");
        }

        public void Push(string line)
        {
            NamedPipeServerStream s;
            lock (_gate)
            {
                s = _current;
            }

            byte[] bytes = new UTF8Encoding(false).GetBytes(line + "\n");
            s.Write(bytes, 0, bytes.Length);
            s.Flush();
        }

        public void Drop()
        {
            NamedPipeServerStream s;
            lock (_gate)
            {
                s = _current;
                _current = null;
            }

            if (s != null)
            {
                s.Dispose();
            }
        }

        private async Task AcceptLoopAsync()
        {
            while (!_stop.IsCancellationRequested)
            {
                var server = new NamedPipeServerStream(_name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
                try
                {
                    await server.WaitForConnectionAsync(_stop.Token);
                }
                catch (Exception)
                {
                    server.Dispose();
                    return;
                }

                lock (_gate)
                {
                    _current = server;
                    Connections++;
                }

                try
                {
                    var reader = new LineReader(server);
                    for (;;)
                    {
                        string line = await reader.ReadLineAsync(_stop.Token);
                        if (line == null)
                        {
                            break;
                        }

                        JObject request = Json.ParseObject(line);
                        lock (_gate)
                        {
                            _requests.Add(request);
                        }

                        foreach (string reply in Answer(request))
                        {
                            Write(server, reply);
                        }

                        if (DropAfter != null && DropAfter == (string)request["cmd"])
                        {
                            break;
                        }
                    }
                }
                catch (Exception)
                {
                    // the test dropped or closed the connection
                }
                finally
                {
                    server.Dispose();
                }
            }
        }

        private static void Write(Stream stream, string line)
        {
            byte[] bytes = new UTF8Encoding(false).GetBytes(line + "\n");
            lock (stream)
            {
                stream.Write(bytes, 0, bytes.Length);
                stream.Flush();
            }
        }

        private string[] Answer(JObject request)
        {
            string cmd = (string)request["cmd"];
            long id = (long)request["id"];
            if (cmd == "status" && StatusError)
            {
                return new[] { Error(id, "CML5402", "unknown command") };
            }

            if (cmd == "status")
            {
                var result = new JObject
                {
                    ["version"] = "0.1.0",
                    ["capabilities"] = new JArray(Capabilities),
                    ["processes"] = new JArray(new JObject { ["pid"] = 4242, ["modules"] = new JArray(new JObject { ["path"] = @"C:\x\app.exe", ["enabled"] = true }) }),
                };
                return new[] { Ok(id, result) };
            }

            if (cmd == "subscribe")
            {
                JToken events = request["args"] != null ? request["args"]["events"] : null;
                if (RefuseEvent != null && events != null && events.ToString().Contains(RefuseEvent))
                {
                    return new[] { Error(id, "CML5311", "unknown event name in 'events'") };
                }

                return new[] { Ok(id, new JObject { ["events"] = events ?? new JArray("diag", "patch_applied", "patch_reverted", "log") }) };
            }

            if (Respond != null)
            {
                return Respond(request);
            }

            return new[] { Ok(id, new JObject()) };
        }

        public static string Ok(long id, JObject result)
        {
            return new JObject { ["id"] = id, ["ok"] = true, ["result"] = result }.ToString(Newtonsoft.Json.Formatting.None);
        }

        public static string Error(long id, string diagId, string message)
        {
            return new JObject
            {
                ["id"] = id,
                ["ok"] = false,
                ["diag"] = new JObject { ["id"] = diagId, ["message"] = message, ["action"] = "do something" },
            }.ToString(Newtonsoft.Json.Formatting.None);
        }

        public void Dispose()
        {
            _stop.Cancel();
            Drop();
            try
            {
                _acceptLoop.Wait(2000);
            }
            catch (Exception)
            {
                // cancelled
            }
        }
    }
}
