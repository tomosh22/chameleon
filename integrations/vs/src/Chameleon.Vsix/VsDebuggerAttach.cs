using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Chameleon.Control;
using EnvDTE;
using EnvDTE80;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Debugger.Interop;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;

namespace Chameleon.Vsix
{
    /// <summary>
    /// The Visual Studio side of <see cref="IDebuggerAttach"/>.
    /// <list type="bullet">
    /// <item>Which processes are debugged, and under which engines: learnt from the debug events Visual Studio sends every
    /// <see cref="IDebugEventCallback2"/> (a program was created in a process by an engine).</item>
    /// <item>Attach: the automation model (<c>Debugger.LocalProcesses</c>, <c>Process2.Attach2(engines)</c>), the same call as
    /// Debug &gt; Attach to Process with the engines chosen.</item>
    /// </list>
    /// </summary>
    internal sealed class VsDebuggerAttach : IDebuggerAttach, IDebugEventCallback2
    {
        private readonly AsyncPackage _package;
        private readonly Action<string> _log;
        private readonly object _gate = new object();
        private readonly Dictionary<int, List<string>> _engines = new Dictionary<int, List<string>>();

        public VsDebuggerAttach(AsyncPackage package, Action<string> log)
        {
            _package = package;
            _log = log;
        }

        /// <summary>Raised (on the debugger's thread) after the set of debugged processes changed.</summary>
        public event Action Changed;

        /// <summary>Starts listening to the debugger. Main thread.</summary>
        public void Advise(IVsDebugger debugger)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            debugger.AdviseDebugEventCallback(this);
        }

        // ---- IDebuggerAttach ---------------------------------------------------------------------------------------------------

        public IReadOnlyList<DebuggedProcess> GetDebuggedProcesses()
        {
            lock (_gate)
            {
                return _engines
                    .Where(kv => IsAlive(kv.Key))
                    .Select(kv => new DebuggedProcess(kv.Key, kv.Value.ToArray()))
                    .ToList();
            }
        }

        private static bool IsAlive(int pid)
        {
            try
            {
                using (System.Diagnostics.Process p = System.Diagnostics.Process.GetProcessById(pid))
                {
                    return !p.HasExited;
                }
            }
            catch (Exception)
            {
                return false;
            }
        }

        public async Task AttachAsync(int pid, IReadOnlyList<string> engines, CancellationToken cancellationToken)
        {
            await _package.JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
            var dte = await _package.GetServiceAsync(typeof(SDTE)) as DTE2;
            if (dte == null)
            {
                throw new InvalidOperationException("Visual Studio's automation model is not available");
            }

            EnvDTE.Process target = null;
            foreach (EnvDTE.Process candidate in dte.Debugger.LocalProcesses)
            {
                if (candidate.ProcessID == pid)
                {
                    target = candidate;
                    break;
                }
            }

            if (target == null)
            {
                throw new InvalidOperationException("the process is not in the debugger's list of local processes");
            }

            var target2 = target as Process2;
            if (engines.Count > 0 && target2 != null)
            {
                try
                {
                    target2.Attach2(engines.ToArray());
                    return;
                }
                catch (System.Runtime.InteropServices.COMException ex)
                {
                    _log("attach with the engines " + string.Join(", ", engines) + " was refused (" + ex.Message.Trim() + "); letting the debugger choose the engines");
                }
            }

            target.Attach();
        }

        // ---- IDebugEventCallback2 ----------------------------------------------------------------------------------------------

        public int Event(IDebugEngine2 pEngine, IDebugProcess2 pProcess, IDebugProgram2 pProgram, IDebugThread2 pThread, IDebugEvent2 pEvent, ref Guid riidEvent, uint dwAttrib)
        {
            try
            {
                Guid id = riidEvent;
                if (id == typeof(IDebugProgramCreateEvent2).GUID)
                {
                    OnProgramCreated(pEngine, pProcess, pProgram);
                }
                else if (id == typeof(IDebugProcessDestroyEvent2).GUID)
                {
                    OnProcessDestroyed(pProcess, pProgram);
                }
            }
            catch (Exception ex)
            {
                _log("debug event ignored: " + ex.Message);
            }

            return VSConstants.S_OK;
        }

        private void OnProgramCreated(IDebugEngine2 engine, IDebugProcess2 process, IDebugProgram2 program)
        {
            int pid = PidOf(process, program);
            if (pid <= 0)
            {
                return;
            }

            string name = null;
            if (program != null)
            {
                string programEngine;
                Guid engineGuid;
                if (program.GetEngineInfo(out programEngine, out engineGuid) == VSConstants.S_OK)
                {
                    name = programEngine;
                }
            }

            if (string.IsNullOrEmpty(name))
            {
                // No name: the engine's id is still a usable (if less readable) name for Attach2.
                Guid engineId;
                if (engine != null && engine.GetEngineId(out engineId) == VSConstants.S_OK)
                {
                    name = engineId.ToString("B");
                }
            }

            lock (_gate)
            {
                List<string> list;
                if (!_engines.TryGetValue(pid, out list))
                {
                    list = new List<string>();
                    _engines[pid] = list;
                }

                if (!string.IsNullOrEmpty(name) && !list.Contains(name))
                {
                    list.Add(name);
                }
            }

            _log("debugger: attached to pid " + pid + (string.IsNullOrEmpty(name) ? "" : " (engine " + name + ")"));
            Changed?.Invoke();
        }

        private void OnProcessDestroyed(IDebugProcess2 process, IDebugProgram2 program)
        {
            int pid = PidOf(process, program);
            if (pid <= 0)
            {
                return;
            }

            lock (_gate)
            {
                _engines.Remove(pid);
            }

            Changed?.Invoke();
        }

        private static int PidOf(IDebugProcess2 process, IDebugProgram2 program)
        {
            if (process == null && program != null)
            {
                program.GetProcess(out process);
            }

            if (process == null)
            {
                return 0;
            }

            var id = new AD_PROCESS_ID[1];
            if (process.GetPhysicalProcessId(id) != VSConstants.S_OK || id[0].ProcessIdType != (uint)enum_AD_PROCESS_ID.AD_PROCESS_ID_SYSTEM)
            {
                return 0;
            }

            return unchecked((int)id[0].dwProcessId);
        }
    }
}
