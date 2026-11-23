using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace Chameleon.Control.Tests
{
    public class FramingTests
    {
        // A stream that hands out one byte per read: lines must survive any chunking.
        private sealed class TrickleStream : MemoryStream
        {
            public TrickleStream(byte[] data) : base(data) { }

            public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken)
            {
                return base.ReadAsync(buffer, offset, Math.Min(1, count), cancellationToken);
            }
        }

        [Fact]
        public async Task SplitsLinesAndToleratesCarriageReturnAndBlankLines()
        {
            var reader = new LineReader(new MemoryStream(Encoding.UTF8.GetBytes("{\"a\":1}\r\n\n{\"b\":2}\nlast")));
            Assert.Equal("{\"a\":1}", await reader.ReadLineAsync(CancellationToken.None));
            Assert.Equal("", await reader.ReadLineAsync(CancellationToken.None));
            Assert.Equal("{\"b\":2}", await reader.ReadLineAsync(CancellationToken.None));
            Assert.Equal("last", await reader.ReadLineAsync(CancellationToken.None));
            Assert.Null(await reader.ReadLineAsync(CancellationToken.None));
        }

        [Fact]
        public async Task SurvivesOneByteReadsAndMultibyteCharacters()
        {
            var reader = new LineReader(new TrickleStream(new UTF8Encoding(false).GetBytes("gr\u00fc\u00dfe \u65e5\u672c\nnext\n")));
            Assert.Equal("gr\u00fc\u00dfe \u65e5\u672c", await reader.ReadLineAsync(CancellationToken.None));
            Assert.Equal("next", await reader.ReadLineAsync(CancellationToken.None));
            Assert.Null(await reader.ReadLineAsync(CancellationToken.None));
        }

        [Fact]
        public async Task ALineOverOneMebibyteIsRefused()
        {
            var big = new byte[LineReader.MaxLineBytes + 10];
            for (int i = 0; i < big.Length; i++)
            {
                big[i] = (byte)'x';
            }

            var reader = new LineReader(new MemoryStream(big));
            await Assert.ThrowsAsync<InvalidDataException>(() => reader.ReadLineAsync(CancellationToken.None));
        }

        [Fact]
        public void PipeNameResolutionFollowsTheBrokersOrder()
        {
            Func<string, string> noEnv = _ => null;
            Func<string, string> env = n => n == "CML_CONTROL_PIPE" ? @"\\.\pipe\from_env" : null;
            Func<string> sid = () => "S-1-5-5-0-12345";

            Assert.Equal(@"\\.\pipe\explicit", PipeNames.Resolve(@"\\.\pipe\explicit", env, sid));
            Assert.Equal(@"\\.\pipe\from_env", PipeNames.Resolve("", env, sid));
            Assert.Equal(@"\\.\pipe\cml_control_S-1-5-5-0-12345", PipeNames.Resolve(null, noEnv, sid));
        }

        [Fact]
        public void ClientNameLosesThePrefixAndRefusesOtherNames()
        {
            Assert.Equal("cml_control_x", PipeNames.ToClientName(@"\\.\pipe\cml_control_x"));
            Assert.Equal("cml_control_x", PipeNames.ToClientName(@"\\.\PIPE\cml_control_x"));
            Assert.Throws<ArgumentException>(() => PipeNames.ToClientName("cml_control_x"));
            Assert.False(PipeNames.HasPrefix(@"\\.\pipe\"));
        }
    }
}
