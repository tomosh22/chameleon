using System;
using System.Globalization;
using System.Text.RegularExpressions;

namespace Chameleon.Control
{
    /// <summary>A source position named by a diagnostic's text.</summary>
    public sealed class DiagnosticLocation
    {
        public DiagnosticLocation(string file, int line, int column)
        {
            File = file;
            Line = line;
            Column = column;
        }

        /// <summary>As written in the message: usually an absolute path, sometimes relative.</summary>
        public string File { get; }

        /// <summary>1-based.</summary>
        public int Line { get; }

        /// <summary>1-based; 0 when the message gives none.</summary>
        public int Column { get; }

        // cl, clang-cl, link: path(line[,col]) :
        private static readonly Regex Msvc = new Regex(
            @"(?<file>(?:[A-Za-z]:[\\/]|\\\\)[^\r\n():*?""<>|]+?|[^\s():*?""<>|]+\.(?:cpp|cc|cxx|c|hpp|hxx|hh|h|inl|ixx|cppm))\((?<line>\d+)(?:,(?<col>\d+))?\)\s*:",
            RegexOptions.CultureInvariant | RegexOptions.Compiled);

        // clang, gcc style: C:\path:line[:col]:
        private static readonly Regex Colon = new Regex(
            @"(?<file>(?:[A-Za-z]:[\\/]|\\\\)[^\r\n:*?""<>|]+?):(?<line>\d+)(?::(?<col>\d+))?:",
            RegexOptions.CultureInvariant | RegexOptions.Compiled);

        /// <summary>The first "file(line,col):" or "file:line:col:" in the text, or false.</summary>
        public static bool TryParse(string text, out DiagnosticLocation location)
        {
            location = null;
            if (string.IsNullOrEmpty(text))
            {
                return false;
            }

            Match best = null;
            foreach (Regex re in new[] { Msvc, Colon })
            {
                Match m = re.Match(text);
                if (m.Success && (best == null || m.Index < best.Index))
                {
                    best = m;
                }
            }

            if (best == null)
            {
                return false;
            }

            int line;
            if (!int.TryParse(best.Groups["line"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out line) || line <= 0)
            {
                return false;
            }

            int column = 0;
            if (best.Groups["col"].Success)
            {
                int.TryParse(best.Groups["col"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out column);
            }

            location = new DiagnosticLocation(best.Groups["file"].Value.Trim(), line, column);
            return true;
        }
    }
}
