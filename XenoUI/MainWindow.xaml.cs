using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace XenoUI
{
    public partial class MainWindow : Window
    {
        public readonly ClientsWindow _clientsWindow = new();
        private readonly ScriptsWindow _scriptsWindow;
        private readonly DispatcherTimer _timer;
        private string _lastContent;

        public MainWindow()
        {
            InitializeComponent();
            _scriptsWindow = new ScriptsWindow(this);
            Icon = BitmapFrame.Create(new Uri("pack://application:,,,/Resources/Images/icon.ico"));
            InitializeWebView2();

            _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
            _timer.Tick += SaveChanges;
            _timer.Start();

            _clientsWindow.Closed += (_, _) => _clientsWindow.Dispatcher.Invoke(() => _clientsWindow.Hide());
        }

        private async void SaveChanges(object sender, EventArgs e)
        {
            try
            {
                string content = await GetScriptContent();
                if (_lastContent != content)
                {
                    string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "bin", "editor.lua");
                    File.WriteAllText(path, content);
                    _lastContent = content;
                }
            }
            catch {}
        }

        private async void InitializeWebView2()
        {
            try
            {
                await script_editor.EnsureCoreWebView2Async(null);

                string bin = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "bin");
                Directory.CreateDirectory(bin);

                string indexPath = Path.Combine(bin, "Monaco", "index.html");
                string tab = Path.Combine(bin, "editor.lua");

                if (!File.Exists(tab))
                    File.WriteAllText(tab, "print(\"Hello, World!\")\n-- Made by .ente0216 ON Discord");

                if (!File.Exists(indexPath))
                    throw new FileNotFoundException("Could not load the Monaco editor.");

                script_editor.Source = new Uri(indexPath);
                await LoadWebView();
                await SetScriptContent(await File.ReadAllTextAsync(tab));
            }
            catch (Exception ex)
            {
                ShowError($"Error initializing WebView2: {ex.Message}");
            }
        }

        private async Task LoadWebView()
        {
            var tcs = new TaskCompletionSource<bool>();
            script_editor.CoreWebView2.NavigationCompleted += (s, e) =>
            {
                if (e.IsSuccess) tcs.TrySetResult(true);
                else tcs.TrySetException(new Exception($"Navigation failed: {e.WebErrorStatus}"));
            };
            await tcs.Task;
        }

        private async Task<string> GetScriptContent()
        {
            string textContent = await script_editor.CoreWebView2.ExecuteScriptAsync("getText()");
            if (textContent.StartsWith("\"") && textContent.EndsWith("\""))
                textContent = textContent[1..^1];

            return Regex.Unescape(textContent);
        }

        private async Task SetScriptContent(string content)
        {
            string escaped = EscapeForScript(content);
            await script_editor.CoreWebView2.ExecuteScriptAsync($"setText(\"{escaped}\")");
        }

        private static string EscapeForScript(string content) =>
            content.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\n", "\\n").Replace("\r", "\\r");

        private static void ShowError(string message) =>
            MessageBox.Show(message, "Error", MessageBoxButton.OK, MessageBoxImage.Error);

        private void buttonMinimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

        private void buttonMaximize_Click(object sender, RoutedEventArgs e)
        {
            WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
            maximizeImage.Source = new BitmapImage(new Uri(WindowState == WindowState.Maximized
                ? "pack://application:,,,/Resources/Images/normalize.png"
                : "pack://application:,,,/Resources/Images/maximize.png"));
        }

        private async void buttonClose_Click(object sender, RoutedEventArgs e)
        {
            await SaveScriptContent();
            _timer.Stop();
            _clientsWindow.Hide();
            _scriptsWindow.Hide();

            Application.Current.Shutdown();
        }

        private async Task SaveScriptContent()
        {
            try
            {
                string content = await GetScriptContent();
                string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "bin", "editor.lua");
                File.WriteAllText(path, content);
            }
            catch {}
        }

        private void Window_MouseLeftButtonDown(object sender, MouseButtonEventArgs e) => DragMove();

        public void ExecuteScript(string scriptContent)
        {
            if (!_clientsWindow.ActiveClients.Any())
            {
                MessageBox.Show("No clients selected. Select at least one client before executing.", "No Client Selected", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            Task.Run(() =>
            {
                string status = _clientsWindow.GetCompilableStatus(scriptContent);

                if (status != "success")
                {
                    _clientsWindow.Dispatcher.Invoke(() =>
                    {
                        MessageBox.Show(status, "Compiler Error", MessageBoxButton.OK, MessageBoxImage.Exclamation);
                    });
                    return;
                }

                _clientsWindow.ExecuteScript(scriptContent);
            });
        }
        private async void buttonExecute_Click(object sender, RoutedEventArgs e)
        {
            string scriptContent = await GetScriptContent();
            _clientsWindow.ExecuteScript(scriptContent);
        }

        private async void buttonClear_Click(object sender, RoutedEventArgs e) =>
            await script_editor.CoreWebView2.ExecuteScriptAsync("setText(\"\")");

        private async void buttonOpenFile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFileDialog { Filter = "Script files (*.lua;*.luau;*.txt)|*.lua;*.luau;*.txt|All files (*.*)|*.*" };
            if (dlg.ShowDialog() != true) return;

            try
            {
                string content = await File.ReadAllTextAsync(dlg.FileName);
                await SetScriptContent(content);
            }
            catch (Exception ex)
            {
                ShowError($"Error loading script: {ex.Message}");
            }
        }

        private async void buttonSaveFile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new SaveFileDialog { Filter = "Script files (*.lua;*.luau;*.txt)|*.lua;*.luau;*.txt|All files (*.*)|*.*" };
            if (dlg.ShowDialog() != true) return;

            try
            {
                string content = await GetScriptContent();
                await File.WriteAllTextAsync(dlg.FileName, content, Encoding.UTF8);
                MessageBox.Show("File saved successfully!", "Success", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                ShowError($"Error saving file: {ex.Message}");
            }
        }

        private void buttonShowMultinstance_Click(object sender, RoutedEventArgs e) => ToggleWindowVisibility(_clientsWindow);
        private void buttonShowScripts_Click(object sender, RoutedEventArgs e) => ToggleWindowVisibility(_scriptsWindow);

        private static void ToggleWindowVisibility(Window window)
        {
            if (window.IsVisible) window.Hide();
            else window.Show();
        }
    }
}
