using System;
using System.IO;
using System.Management.Automation;
using System.Management.Automation.Runspaces;
using System.Threading;
using System.Windows.Forms;

internal static class ProjectManager
{
    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            string root = AppDomain.CurrentDomain.BaseDirectory;
            string script = Path.Combine(root, "Tools", "Agent", "ProjectManager.ps1");
            if (!File.Exists(script)) throw new FileNotFoundException("HEngine Project Manager is incomplete.", script);
            if (args.Length > 1) throw new ArgumentException("Pass a project folder or Project.heproject.");
            var initialState = InitialSessionState.CreateDefault();
            initialState.ExecutionPolicy = Microsoft.PowerShell.ExecutionPolicy.Bypass;
            using (var runspace = RunspaceFactory.CreateRunspace(initialState))
            {
                runspace.ApartmentState = ApartmentState.STA;
                runspace.ThreadOptions = PSThreadOptions.UseCurrentThread;
                runspace.Open();
                runspace.SessionStateProxy.Path.SetLocation(root);
                using (var shell = PowerShell.Create())
                {
                    shell.Runspace = runspace;
                    shell.AddCommand(script);
                    if (args.Length == 1) shell.AddParameter("Project", Path.GetFullPath(args[0]));
                    shell.Invoke();
                    if (shell.InvocationStateInfo.State == PSInvocationState.Failed)
                        throw shell.InvocationStateInfo.Reason;
                }
                return 0;
            }
        }
        catch (Exception error)
        {
            MessageBox.Show(error.Message, "HEngine Project Manager", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
