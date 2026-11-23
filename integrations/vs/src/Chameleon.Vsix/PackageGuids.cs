namespace Chameleon.Vsix
{
    /// <summary>The GUIDs and command ids shared with ChameleonPackage.vsct (keep both in step).</summary>
    internal static class PackageGuids
    {
        public const string PackageString = "5b1c3f0e-6d2a-4c8e-9a47-31d2f0c8a5b1";
        public const string CommandSetString = "a3d71c52-8f4e-4b6a-b0c9-7e5d2a914f63";
        public const string OutputPaneString = "e2c9b7a4-1d53-4f08-8a6e-9c4b3d7f2a10";
        public const string ErrorListString = "7c4d1a98-3b6e-42f5-a1d0-5e8f9b2c6d34";

        // Visual Studio's own UI contexts: no solution open / a solution open. The package loads in either, in the background, so it
        // is listening to the debugger before the first F5.
        public const string NoSolutionContext = "ADFC4E64-0397-11D1-9F4E-00A0C911004F";
        public const string SolutionExistsContext = "F1536EF8-92EC-443C-9ED7-FDADF150DA82";
    }

    internal static class PackageIds
    {
        public const int CmdReload = 0x0100;
        public const int CmdHotRestart = 0x0101;
        public const int CmdRevertLast = 0x0102;
        public const int CmdRevertAll = 0x0103;
        public const int CmdConnect = 0x0104;
        public const int CmdDisconnect = 0x0105;
        public const int CmdToggleReattach = 0x0106;
        public const int CmdShowOutput = 0x0107;
    }
}
