// WP-26: a minimal client of the broker's control pipe (docs/wp/WP-13.md: UTF-8 JSON objects, one per line;
// {"id":N,"cmd":"...","args":{...}} answered by {"id":N,"ok":true,"result":{...}} or {"id":N,"ok":false,"diag":{...}};
// events {"event":"...",...} may arrive between any two lines). Compiled by run_corpus.ps1 with Add-Type.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;

public sealed class CmlControl : IDisposable
{
    private readonly NamedPipeClientStream pipe;
    private readonly StreamReader reader;
    private readonly StreamWriter writer;
    private Task<string> pending;
    private int nextId = 1;

    public readonly List<string> Events = new List<string>();

    public CmlControl(string pipeName, int connectTimeoutMs)
    {
        const string prefix = "\\\\.\\pipe\\";
        string name = pipeName.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ? pipeName.Substring(prefix.Length) : pipeName;
        pipe = new NamedPipeClientStream(".", name, PipeDirection.InOut, PipeOptions.Asynchronous, TokenImpersonationLevel.Identification);
        pipe.Connect(connectTimeoutMs);
        UTF8Encoding utf8 = new UTF8Encoding(false);
        reader = new StreamReader(pipe, utf8);
        writer = new StreamWriter(pipe, utf8);
        writer.NewLine = "\n";
        writer.AutoFlush = true;
    }

    private string ReadLine(int timeoutMs)
    {
        if (pending == null)
        {
            pending = reader.ReadLineAsync();
        }
        if (!pending.Wait(timeoutMs))
        {
            return null;
        }
        string line = pending.Result;
        pending = null;
        return line;
    }

    private static bool IsResponse(string line)
    {
        return line.Contains("\"ok\":");
    }

    // Sends one request and returns the response line, or null when none arrived within timeoutMs. Events that come
    // first are appended to Events.
    public string Call(string cmd, string argsJson, int timeoutMs)
    {
        int id = nextId++;
        string request = "{\"id\":" + id + ",\"cmd\":\"" + cmd + "\"" + (argsJson != null ? ",\"args\":" + argsJson : "") + "}";
        writer.WriteLine(request);
        Stopwatch watch = Stopwatch.StartNew();
        while (true)
        {
            long left = timeoutMs - watch.ElapsedMilliseconds;
            if (left <= 0)
            {
                return null;
            }
            string line = ReadLine((int)Math.Min(left, int.MaxValue));
            if (line == null)
            {
                return null;
            }
            if (line.Length == 0)
            {
                continue;
            }
            if (IsResponse(line))
            {
                return line;
            }
            Events.Add(line);
        }
    }

    // Collects events for up to waitMs (returns early when `stopAfter` events were collected).
    public void Pump(int waitMs)
    {
        Stopwatch watch = Stopwatch.StartNew();
        while (watch.ElapsedMilliseconds < waitMs)
        {
            string line = ReadLine((int)Math.Max(1, waitMs - watch.ElapsedMilliseconds));
            if (line == null)
            {
                return;
            }
            if (line.Length == 0)
            {
                continue;
            }
            if (!IsResponse(line))
            {
                Events.Add(line);
            }
        }
    }

    public void Dispose()
    {
        try { pipe.Dispose(); } catch (Exception) { }
    }
}
