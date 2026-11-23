using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Newtonsoft.Json.Linq;

namespace Chameleon.Control
{
    public enum OutputSeverity
    {
        Info,
        Warning,
        Error,
    }

    /// <summary>One line for the Output pane, and the source position it names (if any), for the Error List.</summary>
    public sealed class OutputLine
    {
        public OutputLine(string text, OutputSeverity severity, DiagnosticLocation location = null, string id = null, string message = null, string action = null)
        {
            Text = text;
            Severity = severity;
            Location = location;
            Id = id;
            Message = message;
            Action = action;
        }

        public string Text { get; }
        public OutputSeverity Severity { get; }
        public DiagnosticLocation Location { get; }

        /// <summary>The diagnostic id (diag events) and its text, for the Error List.</summary>
        public string Id { get; }
        public string Message { get; }

        /// <summary>The "what to do" sentence, when there is one.</summary>
        public string Action { get; }

        /// <summary>The same failure arrives as an error answer and as a diag event; both give the same key.</summary>
        public string DedupeKey
        {
            get { return Id == null ? null : Id + "|" + Message; }
        }
    }

    /// <summary>Remembers the diagnostics already shown, so an error answer and the diag event for one failure are printed once.</summary>
    public sealed class DiagnosticDedupe
    {
        private readonly System.Collections.Generic.Dictionary<string, DateTime> _seen = new System.Collections.Generic.Dictionary<string, DateTime>();
        private readonly TimeSpan _window;
        private readonly Func<DateTime> _now;

        public DiagnosticDedupe(TimeSpan window, Func<DateTime> now = null)
        {
            _window = window;
            _now = now ?? (() => DateTime.UtcNow);
        }

        /// <summary>Records the line (diagnostics only).</summary>
        public void Remember(OutputLine line)
        {
            if (line.DedupeKey == null)
            {
                return;
            }

            lock (_seen)
            {
                _seen[line.DedupeKey] = _now();
            }
        }

        public bool SeenRecently(OutputLine line)
        {
            if (line.DedupeKey == null)
            {
                return false;
            }

            lock (_seen)
            {
                DateTime when;
                return _seen.TryGetValue(line.DedupeKey, out when) && _now() - when < _window;
            }
        }
    }

    /// <summary>Turns the broker's events and the extension's own diagnostics into Output pane text.</summary>
    public static class EventFormatter
    {
        public static OutputLine Format(BrokerEvent ev)
        {
            switch (ev.Name)
            {
                case "diag":
                    return FormatDiag(ev);
                case "patch_applied":
                    return new OutputLine(
                        string.Format(CultureInfo.InvariantCulture, "patch applied: module {0} in pid {1}, generation {2}", Str(ev, "module"), Num(ev, "pid"), Num(ev, "generation")),
                        OutputSeverity.Info);
                case "patch_reverted":
                    return new OutputLine(
                        string.Format(CultureInfo.InvariantCulture, "patch reverted: module {0} in pid {1}, now generation {2}", Str(ev, "module"), Num(ev, "pid"), Num(ev, "generation")),
                        OutputSeverity.Info);
                case "restarted":
                    return new OutputLine(
                        string.Format(CultureInfo.InvariantCulture, "process restarted: pid {0} was replaced by pid {1}", Num(ev, "old_pid"), Num(ev, "new_pid")),
                        OutputSeverity.Info);
                case "exception":
                    return FormatException(ev);
                default:
                    return null;
            }
        }

        public static OutputLine Format(ChameleonDiagnostic diagnostic)
        {
            return new OutputLine(
                (diagnostic.IsError ? "error " : "warning ") + diagnostic.Id + ": " + diagnostic.Message + " What to do: " + diagnostic.Action,
                diagnostic.IsError ? OutputSeverity.Error : OutputSeverity.Warning,
                null,
                diagnostic.Id,
                diagnostic.Message,
                diagnostic.Action);
        }

        public static OutputLine Format(BrokerError error)
        {
            string text = "error " + error.Id + ": " + error.Message + (string.IsNullOrEmpty(error.Action) ? "" : " What to do: " + error.Action);
            DiagnosticLocation location;
            if (!DiagnosticLocation.TryFromFields(error.File, error.Line, error.Column, out location))
            {
                DiagnosticLocation.TryParse(error.Message, out location);  // an older broker: the text is all there is
            }

            return new OutputLine(Navigable(location, text), OutputSeverity.Error, location, error.Id, error.Message, error.Action);
        }

        private static OutputLine FormatDiag(BrokerEvent ev)
        {
            string id = Str(ev, "id");
            string message = FirstNonEmpty(Str(ev, "message"));
            string action = Str(ev, "action");
            bool warning = string.Equals(Str(ev, "severity"), "warning", StringComparison.OrdinalIgnoreCase);
            DiagnosticLocation location;
            if (!DiagnosticLocation.TryFromFields(ev.Str("file"), ev.Int("line"), ev.Int("column"), out location))
            {
                DiagnosticLocation.TryParse(message, out location);  // a broker without "diag-locations": the regex parse stays
            }

            string text = (warning ? "warning " : "error ") + id + ": " + message + (string.IsNullOrEmpty(action) ? "" : " What to do: " + action);
            string stage = Str(ev, "stage");
            if (!string.IsNullOrEmpty(stage) && stage != "?")
            {
                text += " [" + stage + "]";
            }

            return new OutputLine(
                Navigable(location, text),
                warning ? OutputSeverity.Warning : OutputSeverity.Error,
                location,
                id,
                message,
                action);
        }

        // The Output window turns "path(line,col): ..." at the start of a line into a link to the source.
        private static string Navigable(DiagnosticLocation location, string text)
        {
            if (location == null)
            {
                return text;
            }

            string where = location.File + "(" + location.Line + (location.Column > 0 ? "," + location.Column : "") + ")";
            return where + ": " + text;
        }

        private static OutputLine FormatException(BrokerEvent ev)
        {
            string actions = string.Join(", ", (ev.Body["actions"] as JArray ?? new JArray()).Select(a => Json.Str(a) ?? "?"));
            string text = string.Format(
                CultureInfo.InvariantCulture,
                "exception {0} in pid {1} thread {2} at {3}!{4}+{5}; the thread is held until an action is chosen ({6}) with cml_ui or 'exception-action'",
                Str(ev, "code"), Num(ev, "pid"), Num(ev, "tid"), Str(ev, "module"), Str(ev, "function"), Str(ev, "offset"), actions);
            return new OutputLine(text, OutputSeverity.Warning);
        }

        private static string FirstNonEmpty(string text)
        {
            return text ?? "";
        }

        private static string Str(BrokerEvent ev, string key)
        {
            return ev.Str(key) ?? "?";
        }

        private static string Num(BrokerEvent ev, string key)
        {
            long? v = ev.Int(key);
            return v.HasValue ? v.Value.ToString(CultureInfo.InvariantCulture) : "?";
        }
    }
}
