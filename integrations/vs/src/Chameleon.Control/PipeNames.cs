using System;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;

namespace Chameleon.Control
{
    /// <summary>Opens the byte stream to the broker. The real one is a named pipe; the tests use their own.</summary>
    public interface IPipeConnector
    {
        /// <summary>Connects within <paramref name="timeoutMs"/>; throws <see cref="ControlException"/> (CML5994) when it cannot.</summary>
        Task<Stream> ConnectAsync(string fullPipeName, int timeoutMs, CancellationToken cancellationToken);
    }

    public static class PipeNames
    {
        public const string Prefix = @"\\.\pipe\";
        public const string EnvironmentVariable = "CML_CONTROL_PIPE";

        public static bool HasPrefix(string name)
        {
            return name != null && name.Length > Prefix.Length && name.StartsWith(Prefix, StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>The name without the \\.\pipe\ prefix, as <see cref="NamedPipeClientStream"/> wants it.</summary>
        public static string ToClientName(string fullName)
        {
            if (!HasPrefix(fullName))
            {
                throw new ArgumentException("the control pipe name '" + fullName + "' must start with " + Prefix, nameof(fullName));
            }

            return fullName.Substring(Prefix.Length);
        }

        /// <summary>
        /// The control pipe to use, in the broker's own order: an explicit name, then CML_CONTROL_PIPE, then
        /// \\.\pipe\cml_control_&lt;logon-session SID&gt;.
        /// </summary>
        public static string Resolve(string explicitName, Func<string, string> getEnvironmentVariable, Func<string> logonSessionSid)
        {
            string name = explicitName;
            if (string.IsNullOrWhiteSpace(name))
            {
                name = getEnvironmentVariable != null ? getEnvironmentVariable(EnvironmentVariable) : null;
            }

            if (string.IsNullOrWhiteSpace(name))
            {
                name = Prefix + "cml_control_" + logonSessionSid();
            }

            return name.Trim();
        }

        /// <summary>
        /// The logon-session SID of the current process (the group with the logon-id attribute, S-1-5-5-x-y), as the broker names its pipe;
        /// the user SID when the token has none.
        /// </summary>
        public static string CurrentLogonSessionSid()
        {
            using (WindowsIdentity identity = WindowsIdentity.GetCurrent())
            {
                if (identity.Groups != null)
                {
                    SecurityIdentifier logon = identity.Groups
                        .OfType<SecurityIdentifier>()
                        .FirstOrDefault(g => g.Value.StartsWith("S-1-5-5-", StringComparison.Ordinal));
                    if (logon != null)
                    {
                        return logon.Value;
                    }
                }

                return identity.User != null ? identity.User.Value : "unknown";
            }
        }
    }

    /// <summary>Connects to the broker's control pipe on this machine.</summary>
    public sealed class NamedPipeConnector : IPipeConnector
    {
        public async Task<Stream> ConnectAsync(string fullPipeName, int timeoutMs, CancellationToken cancellationToken)
        {
            if (!PipeNames.HasPrefix(fullPipeName))
            {
                throw new ControlException(ClientDiagnostics.NoBroker(fullPipeName, "a pipe name must start with " + PipeNames.Prefix));
            }

            var pipe = new NamedPipeClientStream(".", PipeNames.ToClientName(fullPipeName), PipeDirection.InOut, PipeOptions.Asynchronous);
            try
            {
                await pipe.ConnectAsync(timeoutMs, cancellationToken).ConfigureAwait(false);
                return pipe;
            }
            catch (OperationCanceledException)
            {
                pipe.Dispose();
                throw;
            }
            catch (TimeoutException)
            {
                pipe.Dispose();
                throw new ControlException(ClientDiagnostics.NoBroker(fullPipeName, "no broker is listening on it"));
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
            {
                pipe.Dispose();
                throw new ControlException(ClientDiagnostics.NoBroker(fullPipeName, ex.Message));
            }
        }
    }
}
