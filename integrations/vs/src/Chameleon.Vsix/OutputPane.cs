using System;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;

namespace Chameleon.Vsix
{
    /// <summary>The "Chameleon" pane of the Output window.</summary>
    internal sealed class OutputPane
    {
        private readonly IVsOutputWindowPane _pane;

        private OutputPane(IVsOutputWindowPane pane)
        {
            _pane = pane;
        }

        /// <summary>Creates the pane. Main thread.</summary>
        public static OutputPane Create(IVsOutputWindow window)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            Guid id = new Guid(PackageGuids.OutputPaneString);
            window.CreatePane(ref id, "Chameleon", 1, 0);
            IVsOutputWindowPane pane;
            window.GetPane(ref id, out pane);
            return new OutputPane(pane);
        }

        /// <summary>Appends one line, with a time stamp. Any thread.</summary>
        public void WriteLine(string text)
        {
            if (_pane == null)
            {
                return;
            }

            _pane.OutputStringThreadSafe(DateTime.Now.ToString("HH:mm:ss") + "  " + text + Environment.NewLine);
        }

        /// <summary>Appends a line that must start in column 0 (the Output window links "path(line): ..." only there).</summary>
        public void WriteNavigable(string text)
        {
            if (_pane == null)
            {
                return;
            }

            _pane.OutputStringThreadSafe(text + Environment.NewLine);
        }

        /// <summary>Main thread.</summary>
        public void Activate()
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            if (_pane != null)
            {
                _pane.Activate();
            }
        }
    }
}
