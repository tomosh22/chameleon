using System.ComponentModel;
using Chameleon.Control;
using Microsoft.VisualStudio.Shell;

namespace Chameleon.Vsix
{
    /// <summary>Tools &gt; Options &gt; Chameleon &gt; General.</summary>
    public class ChameleonOptionsPage : DialogPage
    {
        [Category("Debugger")]
        [DisplayName("Re-attach debugger after hot-restart")]
        [Description("When the debugger is attached to a process that Chameleon hot-restarts, attach it to the new process with the same debug engines. " +
                     "Turn it off to attach by hand (Debug > Attach to Process).")]
        public bool ReattachOnRestart { get; set; } = true;

        [Category("Debugger")]
        [DisplayName("Attach attempts")]
        [Description("How often the attach is tried before error CML5997 is reported (the new process may not be attachable for a moment).")]
        public int ReattachAttempts { get; set; } = 3;

        [Category("Connection")]
        [DisplayName("Connect automatically")]
        [Description("Connect to the Chameleon broker when Visual Studio starts, and keep waiting for one to appear.")]
        public bool AutoConnect { get; set; } = true;

        [Category("Connection")]
        [DisplayName("Reconnect automatically")]
        [Description("Connect again after the connection to the broker was lost.")]
        public bool AutoReconnect { get; set; } = true;

        [Category("Connection")]
        [DisplayName("Control pipe name")]
        [Description(@"Full name of the broker's control pipe (\\.\pipe\...). Empty: the CML_CONTROL_PIPE environment variable, else the default of this logon session.")]
        public string PipeName { get; set; } = "";

        [Category("Events")]
        [DisplayName("Show exception events")]
        [Description("Also subscribe to the broker's exception events. While a client listens, the agents hold a faulting thread until somebody chooses an action " +
                     "(cml_ui or the exception-action command), which stalls the program under the Visual Studio debugger. Off unless you want that. Takes effect at the next connect.")]
        public bool ShowExceptionEvents { get; set; }

        public void CopyTo(ChameleonSettings settings)
        {
            settings.ReattachOnRestart = ReattachOnRestart;
            settings.ReattachAttempts = ReattachAttempts < 1 ? 1 : ReattachAttempts;
            settings.AutoConnect = AutoConnect;
            settings.AutoReconnect = AutoReconnect;
            settings.PipeName = PipeName ?? "";
            settings.ShowExceptionEvents = ShowExceptionEvents;
        }

        protected override void OnApply(PageApplyEventArgs e)
        {
            base.OnApply(e);
            ChameleonPackage.Current?.OnSettingsChanged();
        }
    }
}
