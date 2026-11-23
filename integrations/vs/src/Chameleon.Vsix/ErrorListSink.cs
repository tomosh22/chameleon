using System;
using System.IO;
using Chameleon.Control;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;

namespace Chameleon.Vsix
{
    /// <summary>
    /// Puts the broker's diagnostics into the Error List, so the ones that name a file and line open that line on a double click.
    /// All members run on the main thread.
    /// </summary>
    internal sealed class ErrorListSink : IDisposable
    {
        private const int MaxTasks = 200;
        private readonly ErrorListProvider _provider;
        private readonly Func<string, string> _resolve;
        private int _count;

        public ErrorListSink(IServiceProvider services, Func<string, string> resolveRelativePath)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            _resolve = resolveRelativePath;
            _provider = new ErrorListProvider(services)
            {
                ProviderName = "Chameleon",
                ProviderGuid = new Guid(PackageGuids.ErrorListString),
            };
        }

        public void Add(OutputLine line)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            if (line.Id == null)
            {
                return;
            }

            if (_count >= MaxTasks)
            {
                Clear();
            }

            var task = new ErrorTask
            {
                Category = TaskCategory.User,
                ErrorCategory = line.Severity == OutputSeverity.Error ? TaskErrorCategory.Error : TaskErrorCategory.Warning,
                Text = line.Id + ": " + line.Message,
            };
            if (line.Location != null)
            {
                string file = line.Location.File;
                if (!Path.IsPathRooted(file) && _resolve != null)
                {
                    file = _resolve(file) ?? file;
                }

                if (Path.IsPathRooted(file))
                {
                    task.Document = file;
                    task.Line = line.Location.Line - 1;      // the Error List counts from 0
                    task.Column = line.Location.Column > 0 ? line.Location.Column - 1 : 0;
                    task.Navigate += OnNavigate;
                }
            }

            _provider.Tasks.Add(task);
            _count++;
        }

        public void Clear()
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            _provider.Tasks.Clear();
            _count = 0;
        }

        private void OnNavigate(object sender, EventArgs e)
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            var task = sender as ErrorTask;
            if (task == null || string.IsNullOrEmpty(task.Document))
            {
                return;
            }

            // ErrorListProvider.Navigate counts lines from 1, the task from 0.
            task.Line += 1;
            try
            {
                _provider.Navigate(task, new Guid(EnvDTE.Constants.vsViewKindCode));
            }
            finally
            {
                task.Line -= 1;
            }
        }

        public void Dispose()
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            _provider.Tasks.Clear();
            _provider.Dispose();
        }
    }
}
