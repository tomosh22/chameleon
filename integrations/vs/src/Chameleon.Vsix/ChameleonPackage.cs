using System;
using System.ComponentModel.Design;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Chameleon.Control;
using EnvDTE80;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Task = System.Threading.Tasks.Task;

namespace Chameleon.Vsix
{
    /// <summary>
    /// Chameleon for Visual Studio: Tools &gt; Chameleon (reload, hot-restart, revert, connect), the "Chameleon" Output pane, and the
    /// debugger re-attach after a hot-restart. Loads in the background when Visual Studio starts.
    /// </summary>
    [PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
    [Guid(PackageGuids.PackageString)]
    [ProvideMenuResource("Menus.ctmenu", 1)]
    [ProvideOptionPage(typeof(ChameleonOptionsPage), "Chameleon", "General", 0, 0, true)]
    [ProvideAutoLoad(PackageGuids.NoSolutionContext, PackageAutoLoadFlags.BackgroundLoad)]
    [ProvideAutoLoad(PackageGuids.SolutionExistsContext, PackageAutoLoadFlags.BackgroundLoad)]
    public sealed class ChameleonPackage : AsyncPackage
    {
        private ChameleonSettings _settings;
        private ErrorListSink _errors;
        private DTE2 _dte;

        internal static ChameleonPackage Current { get; private set; }

        internal ChameleonController Controller { get; private set; }

        protected override async Task InitializeAsync(CancellationToken cancellationToken, IProgress<ServiceProgressData> progress)
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
            Current = this;

            var options = (ChameleonOptionsPage)GetDialogPage(typeof(ChameleonOptionsPage));
            _settings = new ChameleonSettings();
            options.CopyTo(_settings);

            var window = await GetServiceAsync(typeof(SVsOutputWindow)) as IVsOutputWindow;
            OutputPane output = OutputPane.Create(window);
            _dte = await GetServiceAsync(typeof(SDTE)) as DTE2;
            _errors = new ErrorListSink(this, ResolveRelative);

            var debugger = new VsDebuggerAttach(this, output.WriteLine);
            var vsDebugger = await GetServiceAsync(typeof(SVsShellDebugger)) as IVsDebugger;
            if (vsDebugger != null)
            {
                debugger.Advise(vsDebugger);
            }

            Controller = new ChameleonController(this, output, _errors, debugger, _settings, () =>
            {
                Microsoft.VisualStudio.Shell.ThreadHelper.ThrowIfNotOnUIThread();
                options.ReattachOnRestart = _settings.ReattachOnRestart;
                options.SaveSettingsToStorage();
                return true;
            });

            var commands = await GetServiceAsync(typeof(IMenuCommandService)) as OleMenuCommandService;
            if (commands != null)
            {
                Controller.RegisterCommands(commands);
            }

            output.WriteLine("Chameleon for Visual Studio is ready (re-attach after hot-restart: " + (_settings.ReattachOnRestart ? "on" : "off") + ")");
            if (_settings.AutoConnect)
            {
                Controller.Session.StartAutoConnect();
            }
        }

        /// <summary>Called by the options page after OK.</summary>
        internal void OnSettingsChanged()
        {
            var options = (ChameleonOptionsPage)GetDialogPage(typeof(ChameleonOptionsPage));
            if (_settings == null)
            {
                return;
            }

            bool wasAuto = _settings.AutoConnect;
            options.CopyTo(_settings);
            if (Controller != null)
            {
                if (_settings.AutoConnect && !wasAuto)
                {
                    Controller.Session.StartAutoConnect();
                }
                else if (!_settings.AutoConnect && wasAuto)
                {
                    Controller.Session.StopAutoConnect();
                }
            }
        }

        // A relative path in a diagnostic is taken relative to the solution's folder.
        private string ResolveRelative(string file)
        {
            Microsoft.VisualStudio.Shell.ThreadHelper.ThrowIfNotOnUIThread();
            string solution = _dte != null && _dte.Solution != null ? _dte.Solution.FullName : null;
            if (string.IsNullOrEmpty(solution))
            {
                return null;
            }

            string candidate = Path.Combine(Path.GetDirectoryName(solution) ?? "", file);
            return File.Exists(candidate) ? candidate : null;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                if (Controller != null)
                {
                    Controller.Dispose();
                }

                Current = null;
            }

            base.Dispose(disposing);
        }
    }
}
