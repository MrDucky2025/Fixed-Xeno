using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace XenoUI
{
    public partial class ClientsWindow : Window
    {
        public string XenoVersion = "1.0.8";

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        public struct ClientInfo
        {
            [MarshalAs(UnmanagedType.LPStr)]
            public string version;
            [MarshalAs(UnmanagedType.LPStr)]
            public string name;
            public int id;
        }

        [DllImport("Xeno.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern void Initialize();

        [DllImport("Xeno.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr GetClients();

        [DllImport("Xeno.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern void Execute(byte[] scriptSource, string[] clientUsers, int numUsers);

        [DllImport("Xeno.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern IntPtr Compilable(byte[] scriptSource);

        public List<ClientInfo> ActiveClients { get; private set; } = new();
        public string SupportedVersion { get; private set; } = "";

        private readonly object _lock = new();

        public ClientsWindow()
        {
            InitializeComponent();
            Initialize();
            MouseLeftButtonDown += (_, _) => DragMove();
            StartClientUpdateLoop();
        }

        private void StartClientUpdateLoop()
        {
            Task.Run(async () =>
            {
                while (true)
                {
                    var clients = GetClientsFromDll();

                    lock (_lock)
                    {
                        Dispatcher.BeginInvoke(new Action(() => UpdateUI(clients)));
                    }

                    await Task.Delay(500);
                }
            });
        }

        private void UpdateUI(List<ClientInfo> newClients)
        {
            var existingCheckBoxes = checkBoxContainer.Children.OfType<CheckBox>().ToList();
            var existingIds = existingCheckBoxes.Select(cb => GetClientId(cb.Content.ToString())).ToHashSet();

            foreach (var cb in existingCheckBoxes)
            {
                if (!newClients.Any(c => c.id == GetClientId(cb.Content.ToString())))
                {
                    checkBoxContainer.Children.Remove(cb);
                }
            }

            foreach (var client in newClients)
            {
                if (!existingIds.Contains(client.id) && !string.IsNullOrWhiteSpace(client.name) && client.name != "N/A")
                {
                    var cb = new CheckBox
                    {
                        Content = $"{client.name}, PID: {client.id}",
                        Foreground = Brushes.White,
                        FontFamily = new FontFamily("Franklin Gothic Medium"),
                        IsChecked = true,
                        Background = Brushes.Black
                    };
                    checkBoxContainer.Children.Add(cb);

                    if (!string.IsNullOrWhiteSpace(SupportedVersion) && SupportedVersion != client.version)
                    {
                        MessageBox.Show($"Xeno might not be compatible on the client {client.name} with {client.version}\n\nSupported version: {SupportedVersion}\n\nClick OK to continue using Xeno.", "Version Mismatch", MessageBoxButton.OK, MessageBoxImage.Warning);
                    }
                }
            }

            ActiveClients = checkBoxContainer.Children.OfType<CheckBox>()
                .Where(cb => cb.IsChecked == true)
                .Select(cb => new ClientInfo { name = GetClientName(cb.Content.ToString()), id = GetClientId(cb.Content.ToString()) })
                .ToList();
        }

        public void ExecuteScript(string source)
        {
            if (!ActiveClients.Any()) return;

            byte[] scriptBytes = Encoding.UTF8.GetBytes(source);

            Task.Run(() =>
            {
                Parallel.ForEach(ActiveClients, client =>
                {
                    try
                    {
                        Execute(scriptBytes, new[] { client.name }, 1);
                    }
                    catch {}
                });
            });
        }

        public string GetCompilableStatus(string script)
        {
            IntPtr resultPtr = Compilable(Encoding.ASCII.GetBytes(script));
            string result = Marshal.PtrToStringAnsi(resultPtr);
            Marshal.FreeCoTaskMem(resultPtr);
            return result;
        }

        private unsafe List<ClientInfo> GetClientsFromDll()
        {
            var clients = new List<ClientInfo>();
            IntPtr basePtr = GetClients();
            int structSize = Marshal.SizeOf<ClientInfo>();

            for (int i = 0; i < 256; i++) 
            {
                var client = Marshal.PtrToStructure<ClientInfo>(basePtr + (i * structSize));
                if (string.IsNullOrWhiteSpace(client.name)) break;
                clients.Add(client);
            }

            return clients;
        }

        private static int GetClientId(string content) => int.Parse(content.Split(", PID: ")[1]);
        private static string GetClientName(string content) => content.Split(", PID: ")[0].Trim();

        private void buttonClose_Click(object sender, RoutedEventArgs e) => Hide();
    }
}
