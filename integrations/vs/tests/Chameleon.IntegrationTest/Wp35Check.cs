using System;
using System.Collections.Concurrent;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Chameleon.Control;

namespace Chameleon.IntegrationTest
{
    /// <summary>The outcome of <see cref="Wp35Check.RunAsync"/>: Problem is empty when everything matched.</summary>
    internal sealed class Wp35Result
    {
        public string Problem { get; set; } = "";
        public bool Ok { get { return Problem.Length == 0; } }
    }

    /// <summary>
    /// WP-35 part of the integration test: a compile error must reach the Error List at the exact file and line.
    /// A syntax error is written into the fixture's logic.cpp, `reload` is refused with CML2200, and both the error answer and the
    /// `diag` event are turned into the lines the extension shows (<see cref="EventFormatter"/>; ErrorListSink copies
    /// <see cref="OutputLine.Location"/> into the task: Document, Line - 1, Column - 1). Prints <c>WP35-VS ...</c> lines.
    /// The source is restored to what it was (with return 2) before this returns.
    /// </summary>
    internal static class Wp35Check
    {
        public static async Task<Wp35Result> RunAsync(BrokerSession session, int pid, string source, string sourceBefore, Action<string> say)
        {
            var result = new Wp35Result();
            var diags = new ConcurrentQueue<BrokerEvent>();
            Action<BrokerEvent> onEvent = e =>
            {
                if (e.Name == "diag" && e.Str("id") == "CML2200")
                {
                    diags.Enqueue(e);
                }
            };
            session.EventReceived += onEvent;
            try
            {
                if (!session.LastStatus.Has("diag-locations"))
                {
                    result.Problem = "the broker does not advertise diag-locations";
                    return result;
                }

                string broken = sourceBefore.Replace("return 2;", "return 2 +;").Replace("return 1;", "return 1 +;");
                string[] lines = broken.Replace("\r\n", "\n").Split('\n');
                int expectedLine = Array.FindIndex(lines, l => l.Contains(" +;")) + 1;
                if (expectedLine <= 0)
                {
                    result.Problem = "the scripted syntax error was not applied to " + source;
                    return result;
                }

                File.WriteAllText(source, broken);
                ControlResponse answer = await session.ReloadAsync(pid, false);
                if (answer.Ok || answer.Error == null || answer.Error.Id != "CML2200")
                {
                    result.Problem = "the reload of a syntax error was not refused with CML2200: " + (answer.Error != null ? answer.Error.ToString() : "ok");
                    return result;
                }

                BrokerEvent diag = null;
                DateTime until = DateTime.UtcNow.AddSeconds(10);
                while (DateTime.UtcNow < until && !diags.TryPeek(out diag))
                {
                    await Task.Delay(50);
                }

                if (diag == null)
                {
                    result.Problem = "no diag event CML2200 arrived";
                    return result;
                }

                string expectedFile = Path.GetFullPath(source);
                var answerLine = EventFormatter.Format(answer.Error);
                var eventLine = EventFormatter.Format(diag);
                foreach (var pair in new[] { Tuple.Create("answer", answerLine), Tuple.Create("event", eventLine) })
                {
                    OutputLine line = pair.Item2;
                    DiagnosticLocation at = line.Location;
                    if (at == null)
                    {
                        result.Problem = "the " + pair.Item1 + " has no location";
                        return result;
                    }

                    // What ErrorListSink puts into the Error List task.
                    say("WP35-VS entry=" + pair.Item1 + " file=" + at.File + " line=" + at.Line + " col=" + at.Column + " id=" + line.Id + " expected_line=" + expectedLine);
                    if (!SamePath(at.File, expectedFile))
                    {
                        result.Problem = "the " + pair.Item1 + " names " + at.File + ", expected " + expectedFile;
                        return result;
                    }

                    if (at.Line != expectedLine)
                    {
                        result.Problem = "the " + pair.Item1 + " names line " + at.Line + ", expected " + expectedLine;
                        return result;
                    }

                    // The structured fields were used, not the text: the broker's own numbers are what the entry has.
                    long? fieldLine = pair.Item1 == "answer" ? answer.Error.Line : diag.Int("line");
                    long? fieldColumn = pair.Item1 == "answer" ? answer.Error.Column : diag.Int("column");
                    if (fieldLine != at.Line || (fieldColumn ?? 0) != at.Column)
                    {
                        result.Problem = "the " + pair.Item1 + " entry is not the broker's file/line/column fields (" + fieldLine + "," + fieldColumn + ")";
                        return result;
                    }
                }

                say("WP35-VS result=ok file=" + answerLine.Location.File + " line=" + answerLine.Location.Line + " col=" + answerLine.Location.Column +
                    " diag_event=1 answer=1");
                return result;
            }
            finally
            {
                session.EventReceived -= onEvent;
                File.WriteAllText(source, sourceBefore.Replace("return 1;", "return 2;"));
            }
        }

        private static bool SamePath(string a, string b)
        {
            return string.Equals(a.Replace('/', '\\'), b.Replace('/', '\\'), StringComparison.OrdinalIgnoreCase);
        }
    }
}
