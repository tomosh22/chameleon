using System.Linq;
using Newtonsoft.Json.Linq;
using Xunit;

namespace Chameleon.Control.Tests
{
    public class FormattingTests
    {
        [Theory]
        [InlineData(@"C:\src\app\logic.cpp(12): error C2065: 'x': undeclared identifier", @"C:\src\app\logic.cpp", 12, 0)]
        [InlineData(@"C:\src\my app\logic.cpp(12,7): error C2065: 'x'", @"C:\src\my app\logic.cpp", 12, 7)]
        [InlineData(@"compile of c:/src/a.cc failed: C:/src/a.cc(3,4) : error C1", @"C:/src/a.cc", 3, 4)]
        [InlineData(@"\\server\share\x.cpp(5): warning C4100", @"\\server\share\x.cpp", 5, 0)]
        [InlineData(@"C:\src\a.cpp:40:9: error: use of undeclared identifier 'y'", @"C:\src\a.cpp", 40, 9)]
        [InlineData(@"C:\src\a.cpp:40: error: oops", @"C:\src\a.cpp", 40, 0)]
        [InlineData(@"logic.cpp(8): error C2143: syntax error", "logic.cpp", 8, 0)]
        [InlineData(@"C:\Program Files (x86)\Microsoft Visual Studio\a.cpp(12,3): error C1", @"C:\Program Files (x86)\Microsoft Visual Studio\a.cpp", 12, 3)]
        [InlineData(@"C:\src\dir (2)\sub (3)\b.cpp(7): warning C4100", @"C:\src\dir (2)\sub (3)\b.cpp", 7, 0)]
        [InlineData(@"In file included from C:\inc\h.hpp(3,1): note: here", @"C:\inc\h.hpp", 3, 1)]
        public void FindsTheFileLineAndColumnOfACompilerMessage(string text, string file, int line, int column)
        {
            Assert.True(DiagnosticLocation.TryParse(text, out DiagnosticLocation location));
            Assert.Equal(file, location.File);
            Assert.Equal(line, location.Line);
            Assert.Equal(column, location.Column);
        }

        [Theory]
        [InlineData("")]
        [InlineData(null)]
        [InlineData("the agent did not answer within 60 s")]
        [InlineData(@"C:\src\a.cpp(0): error")]
        [InlineData("error C2065 (12): not a location")]
        public void NoLocationNoMatch(string text)
        {
            Assert.False(DiagnosticLocation.TryParse(text, out DiagnosticLocation location));
            Assert.Null(location);
        }

        private static BrokerEvent Event(string json)
        {
            JObject body = Json.ParseObject(json);
            return new BrokerEvent((string)body["event"], body);
        }

        [Fact]
        public void ADiagEventWithALocationStartsWithItSoTheOutputWindowLinksToTheSource()
        {
            OutputLine line = EventFormatter.Format(Event(
                "{\"event\":\"diag\",\"id\":\"CML2201\",\"message\":\"C:\\\\src\\\\logic.cpp(12,5): error C2065: 'x': undeclared\",\"action\":\"fix it\",\"severity\":\"error\",\"stage\":\"compile\",\"pid\":7}"));
            Assert.StartsWith(@"C:\src\logic.cpp(12,5): error CML2201:", line.Text);
            Assert.Contains("What to do: fix it", line.Text);
            Assert.Equal(OutputSeverity.Error, line.Severity);
            Assert.Equal(12, line.Location.Line);
            Assert.Equal("CML2201", line.Id);
        }

        [Fact]
        public void ADiagEventWithoutALocationIsPlainTextAndWarningsStayWarnings()
        {
            OutputLine line = EventFormatter.Format(Event(
                "{\"event\":\"diag\",\"id\":\"CML5303\",\"message\":\"a changed file maps to no compiland\",\"action\":\"rebuild\",\"severity\":\"warning\",\"stage\":\"sweep\"}"));
            Assert.StartsWith("warning CML5303: a changed file", line.Text);
            Assert.Equal(OutputSeverity.Warning, line.Severity);
            Assert.Null(line.Location);
        }

        [Fact]
        public void TheOtherEventsHaveOneReadableLineEach()
        {
            Assert.Equal("patch applied: module app.exe in pid 7, generation 3",
                EventFormatter.Format(Event("{\"event\":\"patch_applied\",\"pid\":7,\"module\":\"app.exe\",\"generation\":3}")).Text);
            Assert.Equal("patch reverted: module app.exe in pid 7, now generation 2",
                EventFormatter.Format(Event("{\"event\":\"patch_reverted\",\"pid\":7,\"module\":\"app.exe\",\"generation\":2}")).Text);
            Assert.Equal("process restarted: pid 7 was replaced by pid 9",
                EventFormatter.Format(Event("{\"event\":\"restarted\",\"old_pid\":7,\"new_pid\":9}")).Text);
            OutputLine ex = EventFormatter.Format(Event(
                "{\"event\":\"exception\",\"pid\":7,\"tid\":8,\"exception_id\":1,\"code\":\"0xC0000005\",\"module\":\"app.exe\",\"function\":\"f\",\"offset\":\"0x10\",\"actions\":[\"continue\",\"leave\",\"ignore\"]}"));
            Assert.Contains("0xC0000005", ex.Text);
            Assert.Contains("continue, leave, ignore", ex.Text);
            Assert.Null(EventFormatter.Format(Event("{\"event\":\"log\",\"level\":\"info\",\"message\":\"x\"}")));
        }

        [Fact]
        public void TheClientDiagnosticsUseOnlyTheRangeOfThisPackageAndAlwaysSayWhatToDo()
        {
            var all = new[]
            {
                ClientDiagnostics.NoBroker(@"\\.\pipe\p", "none"),
                ClientDiagnostics.PipeLost(@"\\.\pipe\p", "closed"),
                ClientDiagnostics.Unsupported("lacks 'subscribe'"),
                ClientDiagnostics.ReattachFailed(1, 2, "denied"),
            };
            Assert.Equal(new[] { "CML5994", "CML5995", "CML5996", "CML5997" }, all.Select(d => d.Id).ToArray());
            Assert.All(all, d => Assert.False(string.IsNullOrWhiteSpace(d.Action)));
            Assert.All(all, d => Assert.Contains("What to do:", d.ToString()));
            OutputLine line = EventFormatter.Format(all[3]);
            Assert.StartsWith("error CML5997:", line.Text);
        }

        [Fact]
        public void BrokerErrorsWithALocationAreNavigable()
        {
            OutputLine line = EventFormatter.Format(new BrokerError { Id = "CML2201", Message = @"C:\a\b.cpp(3): error C1", Action = "fix" });
            Assert.StartsWith(@"C:\a\b.cpp(3): error CML2201:", line.Text);
            Assert.Equal(3, line.Location.Line);
        }

        // ---- WP-35: structured file, line and column (capability "diag-locations") and the regex fallback ----

        [Fact]
        public void ADiagEventWithFileAndLineUsesThemExactlyAndNotTheText()
        {
            // The text names another place on purpose: the fields win.
            OutputLine line = EventFormatter.Format(Event(
                "{\"event\":\"diag\",\"id\":\"CML2200\",\"message\":\"compiling failed. First error: D:\\\\other\\\\x.cpp(99,1): error C2059\",\"action\":\"fix\",\"severity\":\"error\",\"stage\":\"compile\",\"file\":\"C:\\\\proj\\\\logic.cpp\",\"line\":12,\"column\":15}"));
            Assert.Equal(@"C:\proj\logic.cpp", line.Location.File);
            Assert.Equal(12, line.Location.Line);
            Assert.Equal(15, line.Location.Column);
            Assert.StartsWith(@"C:\proj\logic.cpp(12,15): error CML2200:", line.Text);
        }

        [Fact]
        public void ADiagEventWithoutAColumnHasColumnZero()
        {
            OutputLine line = EventFormatter.Format(Event(
                "{\"event\":\"diag\",\"id\":\"CML2200\",\"message\":\"failed\",\"action\":\"fix\",\"severity\":\"error\",\"file\":\"C:\\\\proj\\\\logic.cpp\",\"line\":12}"));
            Assert.Equal(12, line.Location.Line);
            Assert.Equal(0, line.Location.Column);
            Assert.StartsWith(@"C:\proj\logic.cpp(12): error CML2200:", line.Text);
        }

        [Theory]
        [InlineData("\"file\":\"\",\"line\":12")]
        [InlineData("\"file\":\"C:\\\\a.cpp\"")]
        [InlineData("\"file\":\"C:\\\\a.cpp\",\"line\":0")]
        [InlineData("\"file\":\"C:\\\\a.cpp\",\"line\":\"12\"")]
        [InlineData("\"line\":12")]
        public void UnusableFieldsFallBackToTheRegexParseOfTheMessage(string fields)
        {
            OutputLine line = EventFormatter.Format(Event(
                "{\"event\":\"diag\",\"id\":\"CML2200\",\"message\":\"C:\\\\old\\\\logic.cpp(7,3): error C1: e\",\"action\":\"fix\",\"severity\":\"error\"," + fields + "}"));
            Assert.Equal(@"C:\old\logic.cpp", line.Location.File);
            Assert.Equal(7, line.Location.Line);
            Assert.Equal(3, line.Location.Column);
        }

        [Fact]
        public void AnErrorAnswerWithFieldsIsNavigableAtThemAndWithoutThemFallsBack()
        {
            JObject answer = Json.ParseObject(
                "{\"id\":4,\"ok\":false,\"diag\":{\"id\":\"CML2200\",\"message\":\"First error: C:\\\\x\\\\a.cpp(1): error C1\",\"action\":\"fix\",\"file\":\"C:\\\\proj\\\\logic.cpp\",\"line\":12,\"column\":15}}");
            ControlResponse response = ControlResponse.Parse(answer);
            Assert.Equal(@"C:\proj\logic.cpp", response.Error.File);
            Assert.Equal(12, response.Error.Line);
            Assert.Equal(15, response.Error.Column);
            OutputLine line = EventFormatter.Format(response.Error);
            Assert.Equal(@"C:\proj\logic.cpp", line.Location.File);
            Assert.Equal(12, line.Location.Line);
            Assert.Equal(15, line.Location.Column);

            ControlResponse old = ControlResponse.Parse(Json.ParseObject(
                "{\"id\":5,\"ok\":false,\"diag\":{\"id\":\"CML2200\",\"message\":\"First error: C:\\\\x\\\\a.cpp(1): error C1\",\"action\":\"fix\"}}"));
            Assert.Null(old.Error.File);
            Assert.Null(old.Error.Line);
            Assert.Equal(1, EventFormatter.Format(old.Error).Location.Line);
        }

        [Theory]
        [InlineData("C:\\p\\a.cpp", 12L, 15L, 12, 15)]
        [InlineData("C:\\p\\a.cpp", 12L, null, 12, 0)]
        [InlineData("C:\\p\\a.cpp", 12L, 0L, 12, 0)]
        public void TryFromFieldsKeepsWhatItIsGiven(string file, long line, long? column, int expectedLine, int expectedColumn)
        {
            Assert.True(DiagnosticLocation.TryFromFields(file, line, column, out DiagnosticLocation location));
            Assert.Equal(file, location.File);
            Assert.Equal(expectedLine, location.Line);
            Assert.Equal(expectedColumn, location.Column);
        }

        [Fact]
        public void TryFromFieldsRefusesAMissingFileOrLine()
        {
            Assert.False(DiagnosticLocation.TryFromFields(null, 12L, 1L, out _));
            Assert.False(DiagnosticLocation.TryFromFields("", 12L, 1L, out _));
            Assert.False(DiagnosticLocation.TryFromFields("C:\\a.cpp", null, 1L, out _));
            Assert.False(DiagnosticLocation.TryFromFields("C:\\a.cpp", 0L, 1L, out _));
            Assert.False(DiagnosticLocation.TryFromFields("C:\\a.cpp", long.MaxValue, null, out _));
        }

        [Fact]
        public void StatusIsParsedLeniently()
        {
            StatusInfo s = StatusInfo.FromResult(Json.ParseObject(
                "{\"version\":\"0.1.0\",\"capabilities\":[\"status\",\"reload\",5],\"processes\":[{\"pid\":10,\"modules\":[{\"path\":\"a.exe\",\"enabled\":true,\"index\":{\"state\":\"ready\"}},3]},{\"nopid\":1}],\"future\":true}"));
            Assert.Equal("0.1.0", s.Version);
            Assert.True(s.Has("reload"));
            Assert.False(s.Has("restart"));
            Assert.Single(s.Processes);
            Assert.Equal(10, s.Processes[0].Pid);
            Assert.True(s.Processes[0].Modules[0].Enabled);
            Assert.Equal("ready", s.Processes[0].Modules[0].IndexState);
        }

        [Fact]
        public void AnErrorAnswerAndTheDiagEventOfOneFailureAreShownOnce()
        {
            var dedupe = new DiagnosticDedupe(System.TimeSpan.FromSeconds(10));
            OutputLine fromEvent = EventFormatter.Format(Event(
                "{\"event\":\"diag\",\"id\":\"CML2201\",\"message\":\"C:\\\\a\\\\b.cpp(3): error C1\",\"action\":\"fix it\",\"severity\":\"error\",\"stage\":\"compile\"}"));
            OutputLine fromAnswer = EventFormatter.Format(new BrokerError { Id = "CML2201", Message = @"C:\a\b.cpp(3): error C1", Action = "fix it" });
            OutputLine other = EventFormatter.Format(new BrokerError { Id = "CML2201", Message = "another failure", Action = "fix it" });

            Assert.Equal(fromEvent.DedupeKey, fromAnswer.DedupeKey);
            Assert.False(dedupe.SeenRecently(fromAnswer));
            dedupe.Remember(fromEvent);
            Assert.True(dedupe.SeenRecently(fromAnswer));
            Assert.False(dedupe.SeenRecently(other));
        }

        [Fact]
        public void TheDedupeWindowExpires()
        {
            System.DateTime now = new System.DateTime(2026, 1, 1, 0, 0, 0, System.DateTimeKind.Utc);
            var dedupe = new DiagnosticDedupe(System.TimeSpan.FromSeconds(10), () => now);
            OutputLine line = EventFormatter.Format(new BrokerError { Id = "CML1", Message = "m", Action = "a" });
            dedupe.Remember(line);
            now += System.TimeSpan.FromSeconds(11);
            Assert.False(dedupe.SeenRecently(line));
        }
    }
}
