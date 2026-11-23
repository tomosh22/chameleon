using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;

namespace Chameleon.Control
{
    public enum ConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
    }

    /// <summary>
    /// The extension's view of the broker: connects (and keeps trying while the user wants a connection), checks the capabilities
    /// (ADR-0032: a missing capability disables that feature), subscribes to the events and offers the commands.
    /// </summary>
    public sealed class BrokerSession : IDisposable
    {
        // The events the Output pane shows. `exception` is added only on request (see ChameleonSettings.ShowExceptionEvents).
        private static readonly string[] BaseEvents = { "diag", "patch_applied", "patch_reverted" };
        private static readonly string[] RestartEvents = { "restarted" };

        private readonly IPipeConnector _connector;
        private readonly ChameleonSettings _settings;
        private readonly Func<string, string> _getEnvironment;
        private readonly Func<string> _logonSid;
        private readonly Action<string> _log;
        private readonly object _gate = new object();
        private readonly Func<int, TimeSpan> _retryDelay;
        private ControlClient _client;
        private StatusInfo _status;
        private ConnectionState _state = ConnectionState.Disconnected;
        private CancellationTokenSource _wanted;   // non-null while the user wants a connection (the keep-trying loop runs)
        private bool _reportedNoBroker;
        private bool _disposed;

        public BrokerSession(
            IPipeConnector connector,
            ChameleonSettings settings,
            Action<string> log,
            Func<string, string> getEnvironment = null,
            Func<string> logonSid = null,
            Func<int, TimeSpan> retryDelay = null)
        {
            _connector = connector;
            _settings = settings;
            _log = log ?? (_ => { });
            _getEnvironment = getEnvironment ?? Environment.GetEnvironmentVariable;
            _logonSid = logonSid ?? PipeNames.CurrentLogonSessionSid;
            _retryDelay = retryDelay ?? DefaultRetryDelay;
        }

        public event Action<ConnectionState> StateChanged;
        public event Action<BrokerEvent> EventReceived;

        /// <summary>A client-side diagnostic (CML5994-5997) the user should see.</summary>
        public event Action<ChameleonDiagnostic> DiagnosticRaised;

        public ConnectionState State
        {
            get { lock (_gate) { return _state; } }
        }

        /// <summary>True while the keep-trying loop runs (the user chose Connect, or auto-connect is on).</summary>
        public bool WantsConnection
        {
            get { lock (_gate) { return _wanted != null; } }
        }

        public StatusInfo LastStatus
        {
            get { lock (_gate) { return _status; } }
        }

        public bool Supports(string capability)
        {
            StatusInfo s = LastStatus;
            return State == ConnectionState.Connected && s != null && s.Has(capability);
        }

        /// <summary>Connects now, once, and then keeps the connection (or keeps trying) until <see cref="Disconnect"/>.</summary>
        public async Task<bool> ConnectAsync(CancellationToken cancellationToken = default(CancellationToken))
        {
            lock (_gate)
            {
                if (_disposed)
                {
                    return false;
                }

                if (_wanted == null)
                {
                    _wanted = new CancellationTokenSource();
                }
            }

            bool connected = await TryConnectOnceAsync(cancellationToken).ConfigureAwait(false);
            if (!connected)
            {
                StartKeepTrying();
            }

            return connected;
        }

        /// <summary>Starts the keep-trying loop without waiting for the first attempt (auto-connect at start-up).</summary>
        public void StartAutoConnect()
        {
            lock (_gate)
            {
                if (_disposed)
                {
                    return;
                }

                if (_wanted == null)
                {
                    _wanted = new CancellationTokenSource();
                }
            }

            StartKeepTrying();
        }

        public void Disconnect()
        {
            ControlClient client;
            CancellationTokenSource wanted;
            lock (_gate)
            {
                wanted = _wanted;
                _wanted = null;
                client = _client;
                _client = null;
                _status = null;
            }

            if (wanted != null)
            {
                wanted.Cancel();
            }

            if (client != null)
            {
                client.Dispose();
            }

            SetState(ConnectionState.Disconnected);
        }

        private CancellationTokenSource _loopFor;   // the `_wanted` source whose keep-trying loop is running

        private void StartKeepTrying()
        {
            CancellationTokenSource wanted;
            lock (_gate)
            {
                wanted = _wanted;
                if (wanted == null || ReferenceEquals(_loopFor, wanted))
                {
                    return;
                }

                _loopFor = wanted;
            }

            CancellationToken token = wanted.Token;
            Task.Run(async () =>
            {
                try
                {
                    int attempt = 0;
                    while (!token.IsCancellationRequested)
                    {
                        if (State == ConnectionState.Connected)
                        {
                            await Task.Delay(500, token).ConfigureAwait(false);
                            attempt = 0;
                            continue;
                        }

                        await Task.Delay(_retryDelay(attempt++), token).ConfigureAwait(false);
                        await TryConnectOnceAsync(token).ConfigureAwait(false);
                    }
                }
                catch (OperationCanceledException)
                {
                    // Disconnect
                }
                catch (Exception ex)
                {
                    _log("keep-trying loop ended: " + ex.Message);
                }
            });
        }

        private static TimeSpan DefaultRetryDelay(int attempt)
        {
            return TimeSpan.FromSeconds(attempt < 3 ? 1 : 2);
        }

        private async Task<bool> TryConnectOnceAsync(CancellationToken cancellationToken)
        {
            lock (_gate)
            {
                if (_state != ConnectionState.Disconnected || _disposed)
                {
                    return _state == ConnectionState.Connected;
                }

                _state = ConnectionState.Connecting;
            }

            RaiseState(ConnectionState.Connecting);
            string pipe = null;
            ControlClient client = null;
            try
            {
                pipe = PipeNames.Resolve(_settings.PipeName, _getEnvironment, _logonSid);
                System.IO.Stream stream = await _connector.ConnectAsync(pipe, _settings.ConnectTimeoutMs, cancellationToken).ConfigureAwait(false);
                client = new ControlClient(stream, pipe);
                client.EventReceived += OnClientEvent;
                client.ConnectionLost += diag => OnClientLost(client, diag);
                client.Start();

                ControlResponse status = await client.CallAsync("status", null, 10000, cancellationToken).ConfigureAwait(false);
                if (!status.Ok)
                {
                    throw new ControlException(ClientDiagnostics.Unsupported(
                        "answered 'status' with an error (" + (status.Error != null ? status.Error.ToString() : "no detail") + "); it is not a compatible Chameleon broker", true));
                }

                StatusInfo info = StatusInfo.FromResult(status.Result);
                if (!info.Has(Capabilities.Status) && info.Capabilities.Count == 0 && string.IsNullOrEmpty(info.Version))
                {
                    throw new ControlException(ClientDiagnostics.Unsupported("did not describe itself in 'status'; it is not a Chameleon broker", true));
                }

                await SubscribeAsync(client, info, cancellationToken).ConfigureAwait(false);

                lock (_gate)
                {
                    if (_disposed || _wanted == null)
                    {
                        // Disconnect was chosen while we connected.
                        client.Dispose();
                        client = null;
                    }
                    else
                    {
                        _client = client;
                        _status = info;
                        _state = ConnectionState.Connected;
                        _reportedNoBroker = false;
                    }
                }

                if (client == null)
                {
                    lock (_gate) { _state = ConnectionState.Disconnected; }
                    RaiseState(ConnectionState.Disconnected);
                    return false;
                }

                _log("connected to " + pipe + ": " + info);
                RaiseState(ConnectionState.Connected);
                return true;
            }
            catch (OperationCanceledException)
            {
                if (client != null)
                {
                    client.Dispose();
                }

                lock (_gate) { _state = ConnectionState.Disconnected; }
                RaiseState(ConnectionState.Disconnected);
                return false;
            }
            catch (ControlException ex)
            {
                if (client != null)
                {
                    client.Dispose();
                }

                lock (_gate) { _state = ConnectionState.Disconnected; }
                RaiseState(ConnectionState.Disconnected);
                ReportConnectFailure(ex.Diagnostic);
                return false;
            }
            catch (Exception ex)
            {
                if (client != null)
                {
                    client.Dispose();
                }

                lock (_gate) { _state = ConnectionState.Disconnected; }
                RaiseState(ConnectionState.Disconnected);
                ReportConnectFailure(ClientDiagnostics.NoBroker(pipe ?? "(unresolved)", ex.Message));
                return false;
            }
        }

        private void ReportConnectFailure(ChameleonDiagnostic diagnostic)
        {
            // While nobody started a broker yet the keep-trying loop would repeat CML5994 every second: say it once per episode.
            bool report = true;
            if (diagnostic.Id == ClientDiagIds.NoBroker)
            {
                lock (_gate)
                {
                    report = !_reportedNoBroker;
                    _reportedNoBroker = true;
                }
            }

            if (report)
            {
                Action<ChameleonDiagnostic> handler = DiagnosticRaised;
                if (handler != null)
                {
                    handler(diagnostic);
                }
            }
        }

        private async Task SubscribeAsync(ControlClient client, StatusInfo info, CancellationToken cancellationToken)
        {
            if (!info.Has(Capabilities.Subscribe))
            {
                RaiseDiagnostic(ClientDiagnostics.Unsupported(
                    "does not offer 'subscribe': the Chameleon output pane shows no events and the debugger is not re-attached after a hot-restart"));
                return;
            }

            var events = new List<string>(BaseEvents);
            if (info.Has(Capabilities.Restart))
            {
                events.AddRange(RestartEvents);
            }

            if (_settings.ShowExceptionEvents && info.Has(Capabilities.Exceptions))
            {
                events.Add("exception");
            }

            ControlResponse answer = await client.CallAsync("subscribe", new JObject { ["events"] = new JArray(events) }, 10000, cancellationToken).ConfigureAwait(false);
            if (!answer.Ok)
            {
                // An older broker may not know every name: fall back to the ones every broker with `subscribe` knows.
                _log("subscribe with " + string.Join(",", events) + " refused (" + (answer.Error != null ? answer.Error.ToString() : "?") + "); retrying with the default set");
                answer = await client.CallAsync("subscribe", null, 10000, cancellationToken).ConfigureAwait(false);
                if (!answer.Ok)
                {
                    RaiseDiagnostic(ClientDiagnostics.Unsupported("refused 'subscribe' (" + (answer.Error != null ? answer.Error.ToString() : "no detail") + "); no events are shown"));
                }
            }
        }

        private void OnClientEvent(BrokerEvent ev)
        {
            Action<BrokerEvent> handler = EventReceived;
            if (handler != null)
            {
                handler(ev);
            }
        }

        private void OnClientLost(ControlClient client, ChameleonDiagnostic diagnostic)
        {
            bool ours;
            lock (_gate)
            {
                ours = ReferenceEquals(_client, client);
                if (ours)
                {
                    _client = null;
                    _status = null;
                    _state = ConnectionState.Disconnected;
                }
            }

            if (!ours)
            {
                return;
            }

            client.Dispose();
            RaiseState(ConnectionState.Disconnected);
            RaiseDiagnostic(diagnostic);
            if (_settings.AutoReconnect)
            {
                StartKeepTrying();
            }
            else
            {
                // The user chose not to reconnect by themselves: stop wanting a connection until they choose Connect again.
                CancellationTokenSource wanted;
                lock (_gate)
                {
                    wanted = _wanted;
                    _wanted = null;
                }

                if (wanted != null)
                {
                    wanted.Cancel();
                }
            }
        }

        private void RaiseDiagnostic(ChameleonDiagnostic diagnostic)
        {
            Action<ChameleonDiagnostic> handler = DiagnosticRaised;
            if (handler != null)
            {
                handler(diagnostic);
            }
        }

        private void SetState(ConnectionState state)
        {
            lock (_gate)
            {
                _state = state;
            }

            RaiseState(state);
        }

        private void RaiseState(ConnectionState state)
        {
            Action<ConnectionState> handler = StateChanged;
            if (handler != null)
            {
                handler(state);
            }
        }

        // ---- commands ----------------------------------------------------------------------------------------------------------

        /// <summary>Sends a command. Throws <see cref="ControlException"/> (CML5994/5995) when there is no connection or no answer.</summary>
        public async Task<ControlResponse> SendAsync(string command, JObject args, int timeoutMs, CancellationToken cancellationToken = default(CancellationToken))
        {
            ControlClient client;
            lock (_gate)
            {
                client = _client;
            }

            if (client == null)
            {
                throw new ControlException(ClientDiagnostics.NoBroker(
                    PipeNames.Resolve(_settings.PipeName, _getEnvironment, _logonSid), "the extension is not connected"));
            }

            return await client.CallAsync(command, args, timeoutMs, cancellationToken).ConfigureAwait(false);
        }

        public async Task<StatusInfo> RefreshStatusAsync(CancellationToken cancellationToken = default(CancellationToken))
        {
            ControlResponse r = await SendAsync("status", null, 10000, cancellationToken).ConfigureAwait(false);
            if (!r.Ok)
            {
                throw new ControlException(ClientDiagnostics.Unsupported("answered 'status' with an error", true));
            }

            StatusInfo info = StatusInfo.FromResult(r.Result);
            lock (_gate)
            {
                if (_client != null)
                {
                    _status = info;
                }
            }

            return info;
        }

        public Task<ControlResponse> ReloadAsync(int? pid, bool confirm, CancellationToken cancellationToken = default(CancellationToken))
        {
            var args = new JObject();
            if (pid.HasValue)
            {
                args["pid"] = pid.Value;
            }

            if (confirm)
            {
                args["confirm"] = true;
            }

            return SendAsync("reload", args, _settings.ReloadTimeoutMs, cancellationToken);
        }

        public Task<ControlResponse> RestartAsync(int? pid, CancellationToken cancellationToken = default(CancellationToken))
        {
            var args = new JObject();
            if (pid.HasValue)
            {
                args["pid"] = pid.Value;
            }

            return SendAsync("restart", args, _settings.RestartTimeoutMs, cancellationToken);
        }

        /// <param name="scope">"last" or "all"</param>
        public Task<ControlResponse> RevertAsync(string scope, int? pid, CancellationToken cancellationToken = default(CancellationToken))
        {
            var args = new JObject { ["scope"] = scope };
            if (pid.HasValue)
            {
                args["pid"] = pid.Value;
            }

            return SendAsync("revert", args, _settings.ReloadTimeoutMs, cancellationToken);
        }

        /// <summary>
        /// The pid a command should name: a process the debugger is attached to and the broker knows, else null (the broker then takes the
        /// only connected process, or refuses with CML5801 when there are several).
        /// </summary>
        public static int? ChooseTarget(StatusInfo status, IEnumerable<int> debuggedPids)
        {
            if (status == null || debuggedPids == null)
            {
                return null;
            }

            HashSet<int> known = new HashSet<int>(status.Processes.Select(p => p.Pid));
            foreach (int pid in debuggedPids)
            {
                if (known.Contains(pid))
                {
                    return pid;
                }
            }

            return null;
        }

        public void Dispose()
        {
            ControlClient client;
            CancellationTokenSource wanted;
            lock (_gate)
            {
                if (_disposed)
                {
                    return;
                }

                _disposed = true;
                client = _client;
                _client = null;
                wanted = _wanted;
                _wanted = null;
            }

            if (wanted != null)
            {
                wanted.Cancel();
            }

            if (client != null)
            {
                client.Dispose();
            }
        }
    }
}
