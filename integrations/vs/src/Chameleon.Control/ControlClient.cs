using System;
using System.Collections.Concurrent;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace Chameleon.Control
{
    /// <summary>
    /// One connection to the broker's control pipe: JSON lines, one request in flight at a time (the broker answers in order),
    /// events delivered as they arrive between the answers.
    /// <code>
    /// request   {"id":N,"cmd":"reload","args":{...}}
    /// response  {"id":N,"ok":true,"result":{...}}  or  {"id":N,"ok":false,"diag":{"id","message","action"}}
    /// event     {"event":"diag", ...}
    /// </code>
    /// </summary>
    public sealed class ControlClient : IDisposable
    {
        private readonly Stream _stream;
        private readonly string _pipeName;
        private readonly LineReader _reader;
        private readonly SemaphoreSlim _callLock = new SemaphoreSlim(1, 1);
        private readonly ConcurrentDictionary<long, TaskCompletionSource<ControlResponse>> _pending =
            new ConcurrentDictionary<long, TaskCompletionSource<ControlResponse>>();
        private readonly CancellationTokenSource _stop = new CancellationTokenSource();
        private long _nextId;
        private int _closed;
        private int _disposed;

        public ControlClient(Stream stream, string pipeName)
        {
            _stream = stream;
            _pipeName = pipeName;
            _reader = new LineReader(stream);
        }

        /// <summary>Raised on the reader thread for every event line. Handlers must not block.</summary>
        public event Action<BrokerEvent> EventReceived;

        /// <summary>Raised once when the connection ended without <see cref="Dispose"/> (the broker went away, a line was unusable).</summary>
        public event Action<ChameleonDiagnostic> ConnectionLost;

        public bool IsOpen
        {
            get { return Volatile.Read(ref _closed) == 0; }
        }

        /// <summary>Starts reading. Attach the handlers first.</summary>
        public void Start()
        {
            Task.Run(ReadLoopAsync);
        }

        /// <summary>Sends one request and waits for its answer. Broker errors come back as a response with Ok = false.</summary>
        public async Task<ControlResponse> CallAsync(string command, JObject args, int timeoutMs, CancellationToken cancellationToken)
        {
            await _callLock.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                if (!IsOpen)
                {
                    throw new ControlException(ClientDiagnostics.PipeLost(_pipeName, "the connection is closed"));
                }

                long id = Interlocked.Increment(ref _nextId);
                var completion = new TaskCompletionSource<ControlResponse>(TaskCreationOptions.RunContinuationsAsynchronously);
                _pending[id] = completion;
                try
                {
                    // Fail() sets _closed before it fails the pending requests: checking again after the add closes the gap.
                    if (!IsOpen)
                    {
                        throw new ControlException(ClientDiagnostics.PipeLost(_pipeName, "the connection is closed"));
                    }

                    var request = new JObject { ["id"] = id, ["cmd"] = command };
                    if (args != null)
                    {
                        request["args"] = args;
                    }

                    byte[] bytes = new UTF8Encoding(false).GetBytes(request.ToString(Formatting.None) + "\n");
                    try
                    {
                        await _stream.WriteAsync(bytes, 0, bytes.Length, cancellationToken).ConfigureAwait(false);
                        await _stream.FlushAsync(cancellationToken).ConfigureAwait(false);
                    }
                    catch (Exception ex) when (ex is IOException || ex is ObjectDisposedException || ex is InvalidOperationException)
                    {
                        throw new ControlException(ClientDiagnostics.PipeLost(_pipeName, ex.Message));
                    }

                    using (var delayCancel = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
                    {
                        Task finished = await Task.WhenAny(completion.Task, Task.Delay(timeoutMs, delayCancel.Token)).ConfigureAwait(false);
                        if (finished != completion.Task)
                        {
                            cancellationToken.ThrowIfCancellationRequested();
                            throw new ControlException(ClientDiagnostics.PipeLost(
                                _pipeName, "no answer to '" + command + "' within " + (timeoutMs / 1000) + " s"));
                        }

                        delayCancel.Cancel();
                    }

                    return await completion.Task.ConfigureAwait(false);
                }
                finally
                {
                    _pending.TryRemove(id, out _);
                }
            }
            finally
            {
                _callLock.Release();
            }
        }

        private async Task ReadLoopAsync()
        {
            string reason = "the broker closed the pipe";
            try
            {
                for (;;)
                {
                    string line = await _reader.ReadLineAsync(_stop.Token).ConfigureAwait(false);
                    if (line == null)
                    {
                        break;
                    }

                    if (line.Length == 0)
                    {
                        continue;
                    }

                    JObject obj;
                    try
                    {
                        obj = Json.ParseObject(line);
                    }
                    catch (JsonException)
                    {
                        continue;  // not ours to interpret; the protocol ignores what it does not know
                    }

                    Dispatch(obj);
                }
            }
            catch (Exception ex)
            {
                if (Volatile.Read(ref _disposed) != 0)
                {
                    return;
                }

                reason = ex.Message;
            }

            if (Volatile.Read(ref _disposed) == 0)
            {
                Fail(reason, raise: true);
            }
        }

        private void Dispatch(JObject obj)
        {
            JToken ev = obj["event"];
            if (ev != null && ev.Type == JTokenType.String)
            {
                Action<BrokerEvent> handler = EventReceived;
                if (handler != null)
                {
                    try
                    {
                        handler(new BrokerEvent(ev.Value<string>(), obj));
                    }
                    catch (Exception)
                    {
                        // A faulty handler must not end the connection.
                    }
                }

                return;
            }

            JToken id = obj["id"];
            if (id != null && id.Type == JTokenType.Integer && _pending.TryGetValue(id.Value<long>(), out TaskCompletionSource<ControlResponse> completion))
            {
                completion.TrySetResult(ControlResponse.Parse(obj));
            }
            // An answer nobody waits for any more (it timed out) is dropped.
        }

        private void Fail(string reason, bool raise)
        {
            if (Interlocked.Exchange(ref _closed, 1) != 0)
            {
                return;
            }

            var diagnostic = ClientDiagnostics.PipeLost(_pipeName, reason);
            foreach (var pair in _pending)
            {
                pair.Value.TrySetException(new ControlException(diagnostic));
            }

            if (raise)
            {
                Action<ChameleonDiagnostic> handler = ConnectionLost;
                if (handler != null)
                {
                    try
                    {
                        handler(diagnostic);
                    }
                    catch (Exception)
                    {
                        // see Dispatch
                    }
                }
            }
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0)
            {
                return;
            }

            Fail("the client closed the connection", raise: false);
            _stop.Cancel();
            try
            {
                _stream.Dispose();
            }
            catch (Exception)
            {
                // already broken
            }
        }
    }
}
