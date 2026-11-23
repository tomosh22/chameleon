using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Chameleon.Control
{
    /// <summary>
    /// Splits a byte stream into lines (the control protocol's framing: UTF-8 text, one JSON object per line, "\n", a trailing "\r" is
    /// tolerated). A line over 1 MiB ends the connection, as the broker's own limit does.
    /// </summary>
    public sealed class LineReader
    {
        public const int MaxLineBytes = 1024 * 1024;

        private readonly Stream _stream;
        private readonly byte[] _buffer = new byte[16 * 1024];
        private int _start;
        private int _end;

        public LineReader(Stream stream)
        {
            _stream = stream;
        }

        /// <summary>The next line without its terminator, or null at the end of the stream.</summary>
        public async Task<string> ReadLineAsync(CancellationToken cancellationToken)
        {
            var line = new MemoryStream();
            for (;;)
            {
                if (_start == _end)
                {
                    int n = await _stream.ReadAsync(_buffer, 0, _buffer.Length, cancellationToken).ConfigureAwait(false);
                    if (n <= 0)
                    {
                        // A final line without a newline is still a line.
                        return line.Length == 0 ? null : Decode(line);
                    }

                    _start = 0;
                    _end = n;
                }

                int newline = Array.IndexOf(_buffer, (byte)'\n', _start, _end - _start);
                int take = newline < 0 ? _end - _start : newline - _start;
                if (line.Length + take > MaxLineBytes)
                {
                    throw new InvalidDataException("a control line is longer than 1 MiB");
                }

                line.Write(_buffer, _start, take);
                if (newline >= 0)
                {
                    _start = newline + 1;
                    return Decode(line);
                }

                _start = _end;
            }
        }

        private static string Decode(MemoryStream line)
        {
            byte[] bytes = line.ToArray();
            int length = bytes.Length;
            if (length > 0 && bytes[length - 1] == (byte)'\r')
            {
                length--;
            }

            return new UTF8Encoding(false).GetString(bytes, 0, length);
        }
    }
}
