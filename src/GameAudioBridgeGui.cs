using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Microsoft.Win32;

namespace GameAudioBridgeGui
{
    internal sealed class GameProfile
    {
        public string Name;
        public Dictionary<string, string> Values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        public string Get(string key, string fallback = "") { string v; return Values.TryGetValue(key, out v) ? v : fallback; }
        public void Set(string key, string value) { Values[key] = value ?? ""; }
        public bool Enabled { get { return String.Equals(Get("Enabled", "FALSE"), "TRUE", StringComparison.OrdinalIgnoreCase) || String.Equals(Get("Intercept", "FALSE"), "TRUE", StringComparison.OrdinalIgnoreCase); } set { Set("Enabled", value ? "TRUE" : "FALSE"); } }
    }

    internal sealed class IniDocument
    {
        public readonly List<GameProfile> Profiles = new List<GameProfile>();
        public readonly Dictionary<string, string> Settings = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        public static IniDocument Load(string path)
        {
            IniDocument doc = new IniDocument();
            if (!File.Exists(path)) return doc;
            GameProfile current = null;
            foreach (string raw in File.ReadAllLines(path, Encoding.UTF8))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line.StartsWith(";") || line.StartsWith("#")) continue;
                if (line.StartsWith("[") && line.EndsWith("]") && line.Length > 2)
                {
                    string name = line.Substring(1, line.Length - 2).Trim();
                    if (name.Equals("Settings", StringComparison.OrdinalIgnoreCase)) { current = null; continue; }
                    current = doc.Profiles.FirstOrDefault(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
                    if (current == null) { current = new GameProfile { Name = name }; doc.Profiles.Add(current); }
                    continue;
                }
                int equals = line.IndexOf('=');
                if (equals <= 0) continue;
                string key = line.Substring(0, equals).Trim();
                string value = line.Substring(equals + 1).Trim();
                if (current == null) doc.Settings[key] = value;
                else current.Set(key, value);
            }
            return doc;
        }
        public void Save(string path)
        {
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("; Game Audio Bridge: only game profiles are stored here.");
            sb.AppendLine("; Enabled=TRUE means interception is enabled for this game.");
            foreach (GameProfile p in Profiles)
            {
                sb.AppendLine(); sb.Append('[').Append(p.Name).AppendLine("]");
                foreach (KeyValuePair<string, string> kv in p.Values.OrderBy(x => x.Key, StringComparer.OrdinalIgnoreCase))
                    sb.Append(kv.Key).Append('=').AppendLine(kv.Value ?? "");
            }
            File.WriteAllText(path, sb.ToString(), new UTF8Encoding(false));
        }
    }

    internal sealed class GameAudioBridgeForm : Form
    {
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern int GAB_ASIO_IsOpen();
        private readonly string root;
        private readonly string iniPath;
        private readonly IniDocument config;
        private readonly ListBox disabledList = new ListBox();
        private readonly ListBox enabledList = new ListBox();
        private readonly Label status = new Label();
        private readonly ComboBox renderMode = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
        private string selectedRenderMode = "OpenALCreative";
        private readonly Dictionary<string, GameProfile> rowProfiles = new Dictionary<string, GameProfile>(StringComparer.OrdinalIgnoreCase);

        public GameAudioBridgeForm()
        {
            root = AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            iniPath = Path.Combine(root, "GameAudioBridge.ini");
            config = IniDocument.Load(iniPath);
            Text = "Game Audio Bridge 2.2 — управление играми";
            StartPosition = FormStartPosition.CenterScreen; Size = new Size(1100, 730); MinimumSize = new Size(930, 620);
            Font = new Font("Segoe UI", 9F); AllowDrop = false;

            Label intro = new Label { Text = "Добавляй EXE-файлы или папки. Разрядность определяется автоматически. Настройки хранятся рядом с GameAudioBridge.exe.", Left = 14, Top = 12, Width = 1050, Height = 38, Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            Controls.Add(intro);
            Label modeLabel = new Label { Text = "Режим вывода:", Left = 14, Top = 57, Width = 110, Height = 24 };
            renderMode.SetBounds(124, 52, 300, 28);
            renderMode.Items.Add("Аппаратный OpenAL Creative");
            renderMode.Items.Add("ASIO (драйвер звуковой карты)");
            renderMode.Items.Add("WASAPI Exclusive");
            renderMode.SelectedIndex = 0;
            renderMode.SelectedIndexChanged += (s, e) =>
            {
                selectedRenderMode = renderMode.SelectedIndex == 1 ? "ASIO" : renderMode.SelectedIndex == 2 ? "WASAPIExclusive" : "OpenALCreative";
                if (selectedRenderMode == "OpenALCreative") status.Text = "Выбран OpenAL Creative. Проверка драйвера доступна кнопкой выше.";
                else if (selectedRenderMode == "ASIO") status.Text = "ASIO: откройте настройки, выберите драйвер и примените матрицу до запуска игры.";
                else status.Text = "WASAPI Exclusive пока не реализован; запуск в этом режиме будет заблокирован.";
            };
            Button probe = MakeButton("Проверить Creative OpenAL", 440, 50, 220); probe.Anchor = AnchorStyles.Top | AnchorStyles.Left; probe.Click += (s, e) => ProbeCreativeOpenALBoth();
            Button asioSettings = MakeButton("Настройки ASIO…", 675, 50, 180); asioSettings.Anchor = AnchorStyles.Top | AnchorStyles.Left; asioSettings.Click += (s, e) => { using (AsioRoutingForm f = new AsioRoutingForm()) f.ShowDialog(this); };
            Label leftTitle = new Label { Text = "Перехват выключен", Left = 14, Top = 86, Width = 450, Height = 24 };
            Label rightTitle = new Label { Text = "Перехват включён", Left = 620, Top = 86, Width = 450, Height = 24 };
            Controls.Add(modeLabel); Controls.Add(renderMode); Controls.Add(probe); Controls.Add(asioSettings); Controls.Add(leftTitle); Controls.Add(rightTitle);
            disabledList.SetBounds(14, 112, 455, 408); disabledList.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left; disabledList.HorizontalScrollbar = true; disabledList.SelectionMode = SelectionMode.MultiExtended;
            enabledList.SetBounds(620, 112, 455, 408); enabledList.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Right; enabledList.HorizontalScrollbar = true; enabledList.SelectionMode = SelectionMode.MultiExtended;
            Controls.Add(disabledList); Controls.Add(enabledList);
            disabledList.MouseDoubleClick += (s, e) => ToggleRowOnDoubleClick(disabledList, true, e);
            enabledList.MouseDoubleClick += (s, e) => ToggleRowOnDoubleClick(enabledList, false, e);
            Button addExe = MakeButton("Добавить EXE…", 14, 532, 130); addExe.Anchor = AnchorStyles.Bottom | AnchorStyles.Left; addExe.Click += AddExe;
            Button addFolder = MakeButton("Добавить папку…", 152, 532, 145); addFolder.Anchor = AnchorStyles.Bottom | AnchorStyles.Left; addFolder.Click += AddFolder;
            Button remove = MakeButton("Удалить", 14, 570, 130); remove.Anchor = AnchorStyles.Bottom | AnchorStyles.Left; remove.Click += RemoveSelected;
            Button monitor = MakeButton("Аудиомонитор", 470, 570, 160); monitor.Anchor = AnchorStyles.Bottom; monitor.Click += (s, e) => new AudioMonitorForm().ShowDialog(this);
            Button on = MakeButton("Включить →", 482, 220, 122); on.Anchor = AnchorStyles.Top; on.Click += EnableSelected;
            Button off = MakeButton("← Выключить", 482, 266, 122); off.Anchor = AnchorStyles.Top; off.Click += DisableSelected;
            Button launch = MakeButton("Запустить выбранную игру", 620, 532, 455); launch.Anchor = AnchorStyles.Bottom | AnchorStyles.Right; launch.Click += LaunchSelected;
            status.SetBounds(14, 612, 1050, 24); status.Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right; status.Text = "Готово. Важные сообщения и формат потока появятся здесь; подробный журнал открывается вручную.";
            Timer statusLogTimer = new Timer { Interval = 1000 };
            statusLogTimer.Tick += (s, e) => RefreshStatusFromLog();
            statusLogTimer.Start();
            FormClosed += (s, e) => statusLogTimer.Stop();
            Controls.Add(addExe); Controls.Add(addFolder); Controls.Add(remove); Controls.Add(monitor); Controls.Add(on); Controls.Add(off); Controls.Add(launch); Controls.Add(status);
            AttachRemovalContextMenu(disabledList); AttachRemovalContextMenu(enabledList);
            RefreshLists();
            try { config.Save(iniPath); } catch (Exception ex) { status.Text = "Не удалось сохранить INI: " + ex.Message; }
        }

        private void RefreshStatusFromLog()
        {
            try
            {
                string path = Path.Combine(root, "GameAudioBridge.log");
                if (!File.Exists(path)) return;
                string[] lines = File.ReadAllLines(path);
                for (int i = lines.Length - 1; i >= 0; --i)
                {
                    string line = lines[i];
                    if (line.IndexOf("error=0x", StringComparison.OrdinalIgnoreCase) >= 0 || line.IndexOf("refused", StringComparison.OrdinalIgnoreCase) >= 0 || line.IndexOf("no known exact", StringComparison.OrdinalIgnoreCase) >= 0 || line.IndexOf("could not", StringComparison.OrdinalIgnoreCase) >= 0 || line.IndexOf("failed", StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        status.Text = "ПРЕДУПРЕЖДЕНИЕ: " + (line.Length > 205 ? line.Substring(line.Length - 205) : line);
                        break;
                    }
                    if (line.IndexOf("WASAPI render stream format:", StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        string format = line.Substring(line.IndexOf("WASAPI render stream format:", StringComparison.OrdinalIgnoreCase));
                        status.Text = format.Length > 220 ? format.Substring(0, 217) + "…" : format;
                        break;
                    }
                }
            }
            catch { /* The hook appends to this file concurrently. */ }
        }

        private Button MakeButton(string text, int x, int y, int width) { return new Button { Text = text, Left = x, Top = y, Width = width, Height = 32 }; }
        private static string Quote(string s) { return "\"" + s.Replace("\"", "\\\"") + "\""; }
        private string ResolveInstallRoot(GameProfile p)
        {
            string gamePath = p.Get("GamePath").Trim().Trim('"');
            if (!String.IsNullOrWhiteSpace(gamePath)) return Environment.ExpandEnvironmentVariables(gamePath);
            string reg = p.Get("RegPath").Trim();
            if (String.IsNullOrWhiteSpace(reg)) return "";
            int split = reg.LastIndexOf('\\'); if (split <= 0 || split == reg.Length - 1) return "";
            string keyPath = reg.Substring(0, split); string valueName = reg.Substring(split + 1);
            string hivePart = ""; string subKey = keyPath;
            if (keyPath.StartsWith("HKEY_LOCAL_MACHINE\\", StringComparison.OrdinalIgnoreCase)) { hivePart = "HKLM"; subKey = keyPath.Substring("HKEY_LOCAL_MACHINE\\".Length); }
            else if (keyPath.StartsWith("HKLM\\", StringComparison.OrdinalIgnoreCase)) { hivePart = "HKLM"; subKey = keyPath.Substring(5); }
            else if (keyPath.StartsWith("HKEY_CURRENT_USER\\", StringComparison.OrdinalIgnoreCase)) { hivePart = "HKCU"; subKey = keyPath.Substring("HKEY_CURRENT_USER\\".Length); }
            else if (keyPath.StartsWith("HKCU\\", StringComparison.OrdinalIgnoreCase)) { hivePart = "HKCU"; subKey = keyPath.Substring(5); }
            if (hivePart.Length == 0) return "";
            RegistryHive hive = hivePart == "HKLM" ? RegistryHive.LocalMachine : RegistryHive.CurrentUser;
            RegistryView[] views = { RegistryView.Registry64, RegistryView.Registry32, RegistryView.Default };
            foreach (RegistryView view in views)
            {
                try { using (RegistryKey baseKey = RegistryKey.OpenBaseKey(hive, view)) using (RegistryKey key = baseKey.OpenSubKey(subKey)) { if (key != null) { object val = key.GetValue(valueName); if (val != null && Directory.Exists(val.ToString())) return val.ToString(); } } }
                catch { }
            }
            return "";
        }
        private string ResolveSearchRoot(GameProfile p)
        {
            string path = ResolveInstallRoot(p);
            if (String.IsNullOrWhiteSpace(path)) return "";
            string sub = p.Get("SubDir").Trim().Trim('"');
            if (!String.IsNullOrWhiteSpace(sub)) path = Path.Combine(path, Environment.ExpandEnvironmentVariables(sub));
            return path;
        }
        private static string DetectArchitecture(string path)
        {
            try
            {
                using (FileStream fs = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (BinaryReader br = new BinaryReader(fs))
                {
                    if (fs.Length < 64) return "?"; fs.Position = 0x3c; int pe = br.ReadInt32(); if (pe < 0 || pe > fs.Length - 6) return "?";
                    fs.Position = pe; if (br.ReadUInt32() != 0x4550) return "?"; ushort machine = br.ReadUInt16();
                    if (machine == 0x14c) return "x86"; if (machine == 0x8664) return "x64"; if (machine == 0xaa64) return "ARM64"; return "?";
                }
            } catch { return "?"; }
        }
        private List<string> FindExecutables(GameProfile p)
        {
            string exact = p.Get("ExePath").Trim().Trim('"');
            if (!String.IsNullOrWhiteSpace(exact) && File.Exists(exact)) return new List<string> { Path.GetFullPath(exact) };
            string rootPath = ResolveSearchRoot(p);
            if (String.IsNullOrWhiteSpace(rootPath) || !Directory.Exists(rootPath)) return new List<string>();
            try { return Directory.GetFiles(rootPath, "*.exe", SearchOption.AllDirectories).Where(f => !f.Contains("\\_CommonRedist\\") && !f.Contains("\\redist\\")).Take(3000).OrderBy(f => f).ToList(); }
            catch { try { return Directory.GetFiles(rootPath, "*.exe", SearchOption.TopDirectoryOnly).OrderBy(f => f).ToList(); } catch { return new List<string>(); } }
        }
        private string Display(GameProfile p)
        {
            string exe = p.Get("ExePath"); string arch = File.Exists(exe) ? DetectArchitecture(exe) : "?";
            if (arch == "?")
            {
                List<string> files = FindExecutables(p);
                List<string> architectures = files.Select(DetectArchitecture).Where(a => a != "?").Distinct(StringComparer.OrdinalIgnoreCase).ToList();
                if (architectures.Count == 1) arch = architectures[0];
                else if (architectures.Count > 1) arch = String.Join("/", architectures.OrderBy(a => a));
            }
            string location = String.IsNullOrWhiteSpace(exe) ? (p.Get("GamePath") + (p.Get("SubDir").Length > 0 ? "\\" + p.Get("SubDir") : "")) : exe;
            return "[" + arch + "]  " + p.Name + "  —  " + location;
        }
        private void RefreshLists()
        {
            disabledList.BeginUpdate(); enabledList.BeginUpdate(); disabledList.Items.Clear(); enabledList.Items.Clear(); rowProfiles.Clear();
            foreach (GameProfile p in config.Profiles)
            {
                string row = Display(p); int suffix = 2; string unique = row; while (rowProfiles.ContainsKey(unique)) unique = row + " (" + suffix++ + ")";
                rowProfiles[unique] = p; if (p.Enabled) enabledList.Items.Add(unique); else disabledList.Items.Add(unique);
            }
            disabledList.EndUpdate(); enabledList.EndUpdate(); status.Text = "INI: " + iniPath + "    |    Игр: " + config.Profiles.Count;
        }
        private string UniqueName(string desired)
        {
            string name = desired; int i = 2;
            while (config.Profiles.Any(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase))) name = desired + " (" + i++ + ")";
            return name;
        }
        private void AddProfileFromExe(string path)
        {
            if (!File.Exists(path) || DetectArchitecture(path) == "?") return;
            string full = Path.GetFullPath(path);
            if (config.Profiles.Any(p => p.Get("ExePath").Equals(full, StringComparison.OrdinalIgnoreCase))) return;
            GameProfile profile = new GameProfile(); profile.Name = UniqueName(Path.GetFileNameWithoutExtension(full));
            profile.Set("RegPath", ""); profile.Set("GamePath", Path.GetDirectoryName(full)); profile.Set("ExePath", full); profile.Set("SubDir", "");
            profile.Set("Enabled", "FALSE"); profile.Set("Preset", "1"); profile.Set("RootDirInstallOption", "FALSE"); profile.Set("Buffers", "4"); profile.Set("Duration", "25"); profile.Set("MaxVoiceCount", "128"); profile.Set("DisableDirectMusic", "FALSE");
            config.Profiles.Add(profile);
        }
        private void AddExe(object sender, EventArgs e)
        {
            using (OpenFileDialog dlg = new OpenFileDialog()) { dlg.Title = "Выбери исполняемые файлы игр"; dlg.Filter = "Программы (*.exe)|*.exe|Все файлы (*.*)|*.*"; dlg.Multiselect = true; if (dlg.ShowDialog(this) != DialogResult.OK) return; int before = config.Profiles.Count; foreach (string f in dlg.FileNames) AddProfileFromExe(f); SaveAndRefresh("Добавлено игр: " + (config.Profiles.Count - before)); }
        }
        private void AddFolder(object sender, EventArgs e)
        {
            using (FolderBrowserDialog dlg = new FolderBrowserDialog()) { dlg.Description = "Выбери папку с играми — будут найдены EXE во вложенных папках"; if (dlg.ShowDialog(this) != DialogResult.OK) return; int before = config.Profiles.Count; try { foreach (string f in Directory.GetFiles(dlg.SelectedPath, "*.exe", SearchOption.AllDirectories).Take(3000)) AddProfileFromExe(f); } catch (Exception ex) { MessageBox.Show(this, ex.Message, "Ошибка сканирования", MessageBoxButtons.OK, MessageBoxIcon.Error); } SaveAndRefresh("Добавлено игр: " + (config.Profiles.Count - before)); }
        }
        private void ToggleRowOnDoubleClick(ListBox list, bool enable, MouseEventArgs e)
        {
            int index = list.IndexFromPoint(e.Location);
            if (index < 0 || index >= list.Items.Count) return;
            list.SelectedIndex = index;
            GameProfile p;
            if (!rowProfiles.TryGetValue(list.Items[index].ToString(), out p)) return;
            p.Enabled = enable;
            SaveAndRefresh(enable ? "Перехват включён двойным щелчком." : "Перехват выключен двойным щелчком.");
        }

        private void MoveSelected(ListBox from, bool enable)
        {
            foreach (object item in from.SelectedItems) { GameProfile p; if (rowProfiles.TryGetValue(item.ToString(), out p)) p.Enabled = enable; }
            SaveAndRefresh(enable ? "Перехват включён для выбранных игр." : "Перехват выключен для выбранных игр.");
        }
        private void EnableSelected(object sender, EventArgs e) { MoveSelected(disabledList, true); }
        private void DisableSelected(object sender, EventArgs e) { MoveSelected(enabledList, false); }
        private void AttachRemovalContextMenu(ListBox list)
        {
            list.MouseDown += (sender, e) =>
            {
                if (e.Button != MouseButtons.Right) return;

                int index = list.IndexFromPoint(e.Location);
                if (index < 0 || index >= list.Items.Count) return;

                // If the clicked row is not selected, make it the sole selection.
                // If it is already selected, preserve multi-selection within this list.
                if (!list.SelectedIndices.Contains(index))
                {
                    list.ClearSelected();
                    list.SelectedIndex = index;
                }

                // Snapshot targets from the list that was right-clicked. Do not call
                // RemoveSelected here: it also reads stale selections from the other
                // ListBox and can delete unintended entries or hit refresh-time state.
                List<GameProfile> targets = new List<GameProfile>();
                foreach (int selectedIndex in list.SelectedIndices)
                {
                    if (selectedIndex < 0 || selectedIndex >= list.Items.Count) continue;
                    GameProfile profile;
                    string row = list.Items[selectedIndex] == null ? null : list.Items[selectedIndex].ToString();
                    if (!String.IsNullOrEmpty(row) && rowProfiles.TryGetValue(row, out profile) && !targets.Contains(profile))
                        targets.Add(profile);
                }
                if (targets.Count == 0) return;

                ContextMenuStrip menu = new ContextMenuStrip();
                ToolStripMenuItem removeItem = new ToolStripMenuItem("Удалить игру из программы…");
                removeItem.Click += (menuSender, args) => RemoveProfiles(targets);
                menu.Items.Add(removeItem);
                menu.Show(list, e.Location);
            };
        }

        private void RemoveSelected(object sender, EventArgs e)
        {
            List<GameProfile> remove = new List<GameProfile>();
            CollectSelectedProfiles(disabledList, remove);
            CollectSelectedProfiles(enabledList, remove);
            RemoveProfiles(remove);
        }

        private void CollectSelectedProfiles(ListBox list, List<GameProfile> destination)
        {
            foreach (object item in list.SelectedItems)
            {
                GameProfile profile;
                if (item != null && rowProfiles.TryGetValue(item.ToString(), out profile) && !destination.Contains(profile))
                    destination.Add(profile);
            }
        }

        private void RemoveProfiles(IEnumerable<GameProfile> profiles)
        {
            List<GameProfile> remove = profiles == null
                ? new List<GameProfile>()
                : profiles.Where(profile => profile != null && config.Profiles.Contains(profile)).Distinct().ToList();
            if (remove.Count == 0)
            {
                status.Text = "Сначала выбери игру для удаления.";
                return;
            }

            string names = String.Join("\\r\\n", remove.Select(profile => profile.Name).ToArray());
            DialogResult answer = MessageBox.Show(this,
                "Удалить из Game Audio Bridge следующие записи?\\r\\n\\r\\n" + names + "\\r\\n\\r\\nФайлы игры не будут удалены.",
                "Подтверждение удаления", MessageBoxButtons.YesNo, MessageBoxIcon.Question, MessageBoxDefaultButton.Button1);
            if (answer != DialogResult.Yes)
            {
                status.Text = "Удаление отменено.";
                return;
            }

            int removedCount = 0;
            foreach (GameProfile profile in remove)
                if (config.Profiles.Remove(profile)) removedCount++;

            if (removedCount > 0)
                SaveAndRefresh("Удалено записей: " + removedCount);
            else
                status.Text = "Записи уже отсутствуют; список не изменён.";
        }
        private string ChooseExecutable(GameProfile p)
        {
            List<string> candidates = FindExecutables(p);
            if (candidates.Count == 1) return candidates[0];
            if (candidates.Count == 0) { MessageBox.Show(this, "Не удалось найти EXE. Проверь GamePath, RegPath и SubDir в GameAudioBridge.ini либо добавь EXE через кнопку «Добавить EXE».", "Файл игры не найден", MessageBoxButtons.OK, MessageBoxIcon.Warning); return null; }
            using (Form chooser = new Form())
            {
                chooser.Text = "Выбери EXE для " + p.Name; chooser.StartPosition = FormStartPosition.CenterParent; chooser.Size = new Size(850, 500); chooser.MinimizeBox = false; chooser.MaximizeBox = true;
                ListBox list = new ListBox { Dock = DockStyle.Fill, HorizontalScrollbar = true };
                foreach (string f in candidates) list.Items.Add("[" + DetectArchitecture(f) + "]  " + f);
                Button ok = new Button { Text = "Выбрать", Dock = DockStyle.Bottom, Height = 34, DialogResult = DialogResult.OK }; chooser.Controls.Add(list); chooser.Controls.Add(ok); chooser.AcceptButton = ok;
                if (chooser.ShowDialog(this) != DialogResult.OK || list.SelectedIndex < 0) return null;
                return candidates[list.SelectedIndex];
            }
        }
        private void ProbeCreativeOpenALBoth()
        {
            StringBuilder report = new StringBuilder();
            report.AppendLine("GAME AUDIO BRIDGE — ОТЧЁТ ПРОВЕРКИ CREATIVE OPENAL");
            report.AppendLine("Дата: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
            report.AppendLine("ОС: " + (Environment.Is64BitOperatingSystem ? "Windows x64" : "Windows x86"));
            report.AppendLine("Проверка запускает отдельный диагностический процесс каждой разрядности.");
            report.AppendLine("Она проверяет загрузку системного OpenAL32.dll, необходимые функции, открытие устройства, создание контекста и идентификаторы vendor/renderer/device/version.");
            report.AppendLine("Она НЕ доказывает аппаратное ускорение во всех играх и аудиопутях.");
            report.AppendLine(new string('=', 76));

            bool allCreative = true;
            foreach (string arch in new[] { "x86", "x64" })
            {
                report.AppendLine();
                report.AppendLine("ПРОФИЛЬ " + arch + (arch == "x86" ? " (32-бит)" : " (64-бит)"));
                report.AppendLine(new string('-', 76));
                string launcher = Path.Combine(root, arch, "GameAudioBridgeLauncher.exe");
                report.AppendLine("Диагностический EXE: " + launcher);
                if (arch == "x64" && !Environment.Is64BitOperatingSystem)
                {
                    report.AppendLine("РЕЗУЛЬТАТ: ПРОПУЩЕНО — 64-битная Windows не обнаружена.");
                    allCreative = false;
                    continue;
                }
                if (!File.Exists(launcher))
                {
                    report.AppendLine("РЕЗУЛЬТАТ: ОШИБКА — диагностический EXE не найден.");
                    allCreative = false;
                    continue;
                }
                try
                {
                    ProcessStartInfo psi = new ProcessStartInfo(launcher, "--probe-openal")
                    {
                        UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true,
                        CreateNoWindow = true, WorkingDirectory = root
                    };
                    using (Process proc = Process.Start(psi))
                    {
                        string stdout = proc.StandardOutput.ReadToEnd();
                        string stderr = proc.StandardError.ReadToEnd();
                        proc.WaitForExit();
                        report.AppendLine("Код завершения: " + proc.ExitCode);
                        report.AppendLine("Вывод диагностики:");
                        report.AppendLine(String.IsNullOrWhiteSpace(stdout) ? "(пусто)" : stdout.TrimEnd());
                        if (!String.IsNullOrWhiteSpace(stderr))
                        {
                            report.AppendLine("Сообщения об ошибках:");
                            report.AppendLine(stderr.TrimEnd());
                        }
                        report.AppendLine("ИТОГ: " + (proc.ExitCode == 0 ? "CREATIVE OPENAL ПОДТВЕРЖДЁН" : "ПРОВЕРКА НЕ ПРОЙДЕНА"));
                        if (proc.ExitCode != 0) allCreative = false;
                    }
                }
                catch (Exception ex)
                {
                    report.AppendLine("РЕЗУЛЬТАТ: ОШИБКА ЗАПУСКА ДИАГНОСТИКИ");
                    report.AppendLine(ex.GetType().Name + ": " + ex.Message);
                    allCreative = false;
                }
            }
            report.AppendLine();
            report.AppendLine(new string('=', 76));
            report.AppendLine("ОБЩИЙ ИТОГ: " + (allCreative ? "обе доступные разрядности подтвердили Creative OpenAL" : "есть ошибки или непрошедшие проверки; см. разделы выше"));
            report.AppendLine("Примечание: успешный тест означает, что этот диагностический процесс смог открыть OpenAL-устройство и получил идентификаторы Creative. Он не является тестом игрового перехвата или аппаратного ускорения конкретной игры.");

            status.Text = allCreative ? "Проверка OpenAL завершена: x86/x64 подтверждены." : "Проверка OpenAL завершена: есть замечания; см. подробный отчёт.";
            using (Form f = new Form())
            {
                f.Text = "Подробный отчёт Creative OpenAL — x86 / x64";
                f.StartPosition = FormStartPosition.CenterParent;
                f.Size = new Size(900, 680); f.MinimumSize = new Size(700, 450);
                TextBox box = new TextBox { Multiline = true, ReadOnly = true, WordWrap = false, ScrollBars = ScrollBars.Both,
                    Font = new Font("Consolas", 9F), Dock = DockStyle.Fill, Text = report.ToString() };
                Button save = new Button { Text = "Сохранить отчёт…", Dock = DockStyle.Bottom, Height = 36 };
                save.Click += (s, e) =>
                {
                    using (SaveFileDialog dlg = new SaveFileDialog { Title = "Сохранить отчёт OpenAL", Filter = "Текстовый отчёт (*.txt)|*.txt|Все файлы (*.*)|*.*", FileName = "GameAudioBridge_OpenAL_Report_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".txt" })
                    {
                        if (dlg.ShowDialog(f) == DialogResult.OK)
                        {
                            try { File.WriteAllText(dlg.FileName, report.ToString(), new UTF8Encoding(true)); MessageBox.Show(f, "Отчёт сохранён.", "Creative OpenAL", MessageBoxButtons.OK, MessageBoxIcon.Information); }
                            catch (Exception ex) { MessageBox.Show(f, ex.Message, "Не удалось сохранить отчёт", MessageBoxButtons.OK, MessageBoxIcon.Error); }
                        }
                    }
                };
                f.Controls.Add(box); f.Controls.Add(save); f.ShowDialog(this);
            }
        }

        private bool ProbeCreativeOpenAL(bool showMessage, string arch = null)
        {
            if (String.IsNullOrEmpty(arch)) arch = Environment.Is64BitOperatingSystem ? "x64" : "x86";
            string launcher = Path.Combine(root, arch, "GameAudioBridgeLauncher.exe");
            if (!File.Exists(launcher))
            {
                string missing = "Не найден диагностический лаунчер: " + launcher;
                if (showMessage) MessageBox.Show(this, missing, "Creative OpenAL недоступен", MessageBoxButtons.OK, MessageBoxIcon.Error);
                status.Text = missing; return false;
            }
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo(launcher, "--probe-openal") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true, WorkingDirectory = root };
                using (Process proc = Process.Start(psi))
                {
                    string stdout = proc.StandardOutput.ReadToEnd(); string stderr = proc.StandardError.ReadToEnd(); proc.WaitForExit();
                    string report = (stdout + Environment.NewLine + stderr).Trim();
                    if (proc.ExitCode == 0)
                    {
                        status.Text = "Creative OpenAL обнаружен; аппаратное ускорение требует отдельной проверки.";
                        if (showMessage) MessageBox.Show(this, report + "\r\n\r\nПроверка подтверждает обнаружение реализации/устройства Creative, но не может гарантировать аппаратное ускорение для каждого игрового аудиопути.", "Creative OpenAL обнаружен", MessageBoxButtons.OK, MessageBoxIcon.Information);
                        return true;
                    }
                    status.Text = "Creative OpenAL не подтверждён; режим перехвата не запущен.";
                    if (showMessage) MessageBox.Show(this, report + "\r\n\r\nGame Audio Bridge не будет заявлять, что Creative OpenAL работает, и не запустит игру в этом режиме.", "Creative OpenAL не поддерживается", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return false;
                }
            }
            catch (Exception ex)
            {
                if (showMessage) MessageBox.Show(this, ex.Message, "Ошибка проверки OpenAL", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

        private void LaunchSelected(object sender, EventArgs e)
        {
            if (enabledList.SelectedItems.Count != 1) { MessageBox.Show(this, "Выбери одну игру в списке «Перехват включён».", "Game Audio Bridge", MessageBoxButtons.OK, MessageBoxIcon.Information); return; }
            GameProfile p; if (!rowProfiles.TryGetValue(enabledList.SelectedItem.ToString(), out p)) return;
            string exe = ChooseExecutable(p); if (String.IsNullOrEmpty(exe)) return;
            string mode = selectedRenderMode;
            if (mode == "WASAPIExclusive")
            {
                MessageBox.Show(this, "WASAPI Exclusive ещё не реализован в этой версии.", "Режим вывода недоступен", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            string arch = DetectArchitecture(exe); if (arch != "x86" && arch != "x64") { MessageBox.Show(this, "Архитектура EXE не поддерживается: " + arch, "Game Audio Bridge", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            if (mode == "ASIO")
            {
                try { if (GAB_ASIO_IsOpen() == 0) { MessageBox.Show(this, "Сначала открой настройки ASIO и нажми «Открыть драйвер». Драйвер должен оставаться открытым во время игры.", "ASIO не запущен", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; } }
                catch (Exception ex) { MessageBox.Show(this, "ASIO backend недоступен: " + ex.Message, "ASIO", MessageBoxButtons.OK, MessageBoxIcon.Error); return; }
            }
            else if (!ProbeCreativeOpenAL(true, arch)) return;
            string launcher = Path.Combine(root, arch, "GameAudioBridgeLauncher.exe"); string dll = Path.Combine(root, arch, "GameAudioBridgeHook.dll");
            if (!File.Exists(launcher) || !File.Exists(dll)) { MessageBox.Show(this, "Не найдены файлы перехвата " + arch + " в папке:\r\n" + Path.Combine(root, arch), "Game Audio Bridge", MessageBoxButtons.OK, MessageBoxIcon.Error); return; }
            DialogResult answer = MessageBox.Show(this, "Игра:\r\n" + exe + "\r\n\r\nРазрядность: " + arch + "\r\n\r\nЗапустить с экспериментальным перехватом? Не используйте с античитом или защищёнными процессами.", "Game Audio Bridge", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning);
            if (answer != DialogResult.OK) return;
            try { ProcessStartInfo psi = new ProcessStartInfo(launcher, "--launch " + Quote(exe) + " " + Quote(dll)); psi.WorkingDirectory = Path.GetDirectoryName(exe); Process.Start(psi); status.Text = "Запущено: " + p.Name + " (" + arch + "). Ожидание сведений об аудиопотоке…"; }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Не удалось запустить игру", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }
        private void SaveAndRefresh(string message)
        {
            try { config.Save(iniPath); RefreshLists(); status.Text = message + "  |  INI: " + iniPath; }
            catch (Exception ex) { MessageBox.Show(this, "Не удалось сохранить INI рядом с EXE. Если программа установлена в защищённую папку, перенеси её в папку с правом записи.\r\n" + ex.Message, "Ошибка сохранения", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }
    }

    internal sealed class AsioRoutingForm : Form
    {
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)] private static extern int GAB_ASIO_GetDriverCount();
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)] private static extern int GAB_ASIO_GetDriverName(int index, StringBuilder dst, int chars);
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern int GAB_ASIO_Open(int index, double sampleRate, int preferredBufferFrames, int sourceChannels);
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern int GAB_ASIO_OpenControlPanel();
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern int GAB_ASIO_CanSampleRate(double rate);
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern int GAB_ASIO_SetResamplingEnabled(int enabled);
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern int GAB_ASIO_GetInputFormat(out int rate, out int bits, out int mismatch);
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern int GAB_ASIO_IsOpen();
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern int GAB_ASIO_GetOutputChannelCount();
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Unicode)] private static extern int GAB_ASIO_GetOutputChannelName(int index, StringBuilder dst, int chars);
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern int GAB_ASIO_SetRoutingMatrix(byte[] matrix, int sourceChannels, int outputChannels);
        [DllImport("GameAudioBridgeAsio.dll", CallingConvention = CallingConvention.StdCall)] private static extern void GAB_ASIO_Close();
        private readonly ComboBox drivers = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
        private readonly DataGridView matrix = new DataGridView();
        private readonly Label info = new Label();
        private readonly ComboBox rateMode = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
        private readonly CheckBox resampling = new CheckBox { Text = "Разрешить качественный SRC при несовпадении частот", Checked = true, AutoSize = true };
        private readonly Timer formatTimer = new Timer { Interval = 500 };
        private bool opened;
        private const int SourceChannels = 8;
        public AsioRoutingForm()
        {
            Text = "ASIO — устройство и матрица каналов (движок float64)"; StartPosition = FormStartPosition.CenterParent; Size = new Size(850, 600); MinimumSize = new Size(700, 450);
            Label l = new Label { Text = "ASIO-драйвер:", Left = 12, Top = 16, Width = 100 };
            drivers.SetBounds(115, 12, 470, 28); drivers.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            Button refresh = new Button { Text = "Обновить", Left = 595, Top = 11, Width = 100, Height = 30, Anchor = AnchorStyles.Top | AnchorStyles.Right }; refresh.Click += (s,e) => LoadDrivers();
            rateMode.SetBounds(12, 52, 130, 28); rateMode.Items.AddRange(new object[] { "Авто", "44100 Гц", "48000 Гц", "88200 Гц", "96000 Гц" }); rateMode.SelectedIndex = 0;
            Button open = new Button { Text = "Открыть драйвер", Left = 150, Top = 52, Width = 140, Height = 30 }; open.Click += (s,e) => OpenDriver();
            Button panel = new Button { Text = "Панель звуковой карты…", Left = 300, Top = 52, Width = 200, Height = 30 }; panel.Click += (s,e) => { if (!opened) OpenDriver(); if (opened && GAB_ASIO_OpenControlPanel()==0) MessageBox.Show(this,"Драйвер не смог открыть собственную панель управления.","ASIO",MessageBoxButtons.OK,MessageBoxIcon.Warning); };
            Button apply = new Button { Text = "Применить матрицу", Left = 510, Top = 52, Width = 160, Height = 30 }; apply.Click += (s,e) => ApplyMatrix();
            resampling.SetBounds(12, 84, 420, 24); resampling.CheckedChanged += (s,e) => { if(opened) GAB_ASIO_SetResamplingEnabled(resampling.Checked?1:0); };
            matrix.SetBounds(12, 114, 810, 390); matrix.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right; matrix.AllowUserToAddRows=false; matrix.AllowUserToDeleteRows=false; matrix.RowHeadersWidth=150; matrix.AutoSizeColumnsMode=DataGridViewAutoSizeColumnsMode.AllCells;
            info.SetBounds(12, 512, 810, 48); info.Anchor=AnchorStyles.Bottom|AnchorStyles.Left|AnchorStyles.Right; info.Text="ASIO принимает перехваченный WASAPI-поток через shared memory; внутренняя точность — float64. Несовпадение частоты будет выделено красным.";
            Controls.AddRange(new Control[]{l,drivers,refresh,rateMode,open,panel,apply,resampling,matrix,info}); LoadDrivers();
            formatTimer.Tick += (s,e) => UpdateInputFormatStatus(); formatTimer.Start();
        }
        private void LoadDrivers()
        {
            drivers.Items.Clear();
            try { int n=GAB_ASIO_GetDriverCount(); for(int i=0;i<n;i++){StringBuilder b=new StringBuilder(256); if(GAB_ASIO_GetDriverName(i,b,b.Capacity)!=0) drivers.Items.Add(new DriverItem(i,b.ToString()));} if(drivers.Items.Count>0)drivers.SelectedIndex=0; info.Text=drivers.Items.Count==0?"ASIO-драйверы не найдены или GameAudioBridgeAsio.dll не собрана.":"Найдено драйверов: "+drivers.Items.Count+". Открой драйвер для получения аппаратных выходов."; }
            catch(DllNotFoundException){ info.Text="Не найдена GameAudioBridgeAsio.dll. Соберите x64-проект с ASIO SDK."; }
            catch(BadImageFormatException){ info.Text="Разрядность GUI и ASIO движка не совпадает: ASIO движок рассчитан на x64."; }
        }
        private void OpenDriver()
        {
            DriverItem d = drivers.SelectedItem as DriverItem; if(d == null) return;
            try {
                double[] candidates = rateMode.SelectedIndex==0 ? new double[]{48000,44100,96000,88200} : new double[]{(new double[]{0,44100,48000,88200,96000})[rateMode.SelectedIndex]};
                bool didOpen=false; double chosen=0;
                foreach(double candidate in candidates) { if(GAB_ASIO_Open(d.Index,candidate,0,SourceChannels)!=0) { didOpen=true; chosen=candidate; break; } }
                if(!didOpen){info.ForeColor=System.Drawing.Color.Red; info.Text="Не удалось открыть ASIO на выбранной/поддерживаемой частоте. Проверь режим Auto или панель драйвера."; return;}
                opened=true; GAB_ASIO_SetResamplingEnabled(resampling.Checked?1:0); int outs=GAB_ASIO_GetOutputChannelCount(); matrix.Columns.Clear(); matrix.Rows.Clear();
                for(int o=0;o<outs;o++){StringBuilder b=new StringBuilder(256); GAB_ASIO_GetOutputChannelName(o,b,b.Capacity); matrix.Columns.Add(new DataGridViewCheckBoxColumn { Name="out"+o, HeaderText=String.IsNullOrEmpty(b.ToString())?"Выход "+(o+1):b.ToString(), ThreeState=false });}
                for(int s=0;s<SourceChannels;s++){int row=matrix.Rows.Add(); matrix.Rows[row].HeaderCell.Value="Игровой канал "+(s+1); for(int o=0;o<outs;o++) matrix.Rows[row].Cells[o].Value=(s==o);}
                info.ForeColor=System.Drawing.SystemColors.ControlText; info.Text="Драйвер открыт: "+d.Name+"; ASIO: "+chosen+" Гц; входов матрицы: "+SourceChannels+"; аппаратных выходов: "+outs+"; движок float64.";
            } catch(Exception ex){info.Text="Ошибка ASIO: "+ex.Message;}
        }
        private void UpdateInputFormatStatus()
        {
            if(!opened) return;
            try {
                int rate,bits,mismatch; if(GAB_ASIO_GetInputFormat(out rate,out bits,out mismatch)==0 || rate==0) return;
                if(mismatch!=0) {
                    info.ForeColor=System.Drawing.Color.Red;
                    int best=0; int[] candidates = rate==22050 ? new int[]{44100,88200} : (rate==11025 ? new int[]{44100,88200} : (rate==44100 ? new int[]{44100,88200,48000,96000} : (rate==48000 ? new int[]{48000,96000,44100,88200} : new int[]{48000,44100,96000,88200})));
                    foreach(int r in candidates) { try { if(GAB_ASIO_CanSampleRate(r)!=0) { best=r; break; } } catch {} }
                    info.Text="НЕСОВПАДЕНИЕ ФОРМАТА: игра "+rate+" Гц / "+bits+" бит; ASIO "+(rateMode.SelectedIndex==0?"Auto":"выбранная частота")+". SRC="+(resampling.Checked?"включён":"выключен")+". Рекомендуемая аппаратная частота: "+(best==0?"не найдена":best+" Гц")+". При выключенном SRC поток не перенаправляется.";
                } else { info.ForeColor=System.Drawing.SystemColors.ControlText; info.Text="Формат игры: "+rate+" Гц / "+bits+" бит. Частота совпадает с ASIO; преобразование разрядности выполняется движком float64."; }
            } catch { }
        }
        private void ApplyMatrix()
        {
            if(!opened){info.Text="Сначала открой ASIO-драйвер.";return;}
            int outs=GAB_ASIO_GetOutputChannelCount(); byte[] m=new byte[SourceChannels*outs];
            for(int s=0;s<SourceChannels;s++)for(int o=0;o<outs;o++)m[s*outs+o]=Convert.ToBoolean(matrix.Rows[s].Cells[o].Value??false)?(byte)1:(byte)0;
            info.Text=GAB_ASIO_SetRoutingMatrix(m,SourceChannels,outs)!=0?"Матрица применена к ASIO движку.":"Не удалось применить матрицу: проверь число каналов.";
        }
        // Deliberately keep the selected driver open after closing this settings dialog.
        // The main window checks GAB_ASIO_IsOpen before launching a game; closing the main
        // application releases the driver and shared mapping automatically.
        protected override void OnFormClosed(FormClosedEventArgs e){formatTimer.Stop(); base.OnFormClosed(e);}
        private sealed class DriverItem { public int Index; public string Name; public DriverItem(int i,string n){Index=i;Name=n;} public override string ToString(){return Name;} }
    }

    internal sealed class AudioMonitorForm : Form
    {
        private readonly string logPath;
        private readonly TextBox output = new TextBox();
        private readonly Label info = new Label();
        private readonly Label warningBanner = new Label();
        private readonly Timer refreshTimer = new Timer();
        private readonly HashSet<string> alreadyAlerted = new HashSet<string>(StringComparer.Ordinal);

        public AudioMonitorForm()
        {
            logPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "GameAudioBridge.log");
            Text = "Game Audio Bridge — Аудиомонитор";
            StartPosition = FormStartPosition.CenterParent;
            Size = new Size(1000, 680); MinimumSize = new Size(700, 450);
            warningBanner.SetBounds(12, 12, 960, 42); warningBanner.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            warningBanner.BorderStyle = BorderStyle.FixedSingle; warningBanner.BackColor = Color.FromArgb(255, 235, 190);
            warningBanner.ForeColor = Color.FromArgb(130, 55, 0); warningBanner.Font = new Font("Segoe UI", 9F, FontStyle.Bold);
            warningBanner.TextAlign = ContentAlignment.MiddleLeft; warningBanner.Padding = new Padding(8, 0, 8, 0);
            warningBanner.Text = "Контроль точности: ожидание данных. При несовместимости GAB должен оставить исходный путь и записать предупреждение.";
            output.Multiline = true; output.ReadOnly = true; output.WordWrap = false;
            output.ScrollBars = ScrollBars.Both; output.Font = new Font("Consolas", 9F);
            output.SetBounds(12, 62, 960, 520);
            output.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            Button refresh = new Button { Text = "Обновить", Left = 12, Top = 592, Width = 110, Height = 32, Anchor = AnchorStyles.Bottom | AnchorStyles.Left };
            refresh.Click += (s, e) => RefreshLog();
            Button save = new Button { Text = "Сохранить TXT…", Left = 132, Top = 592, Width = 140, Height = 32, Anchor = AnchorStyles.Bottom | AnchorStyles.Left };
            save.Click += SaveLog;
            Button clear = new Button { Text = "Очистить журнал", Left = 282, Top = 592, Width = 130, Height = 32, Anchor = AnchorStyles.Bottom | AnchorStyles.Left };
            clear.Click += ClearLog;
            info.SetBounds(422, 596, 544, 28); info.Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            Controls.Add(warningBanner); Controls.Add(output); Controls.Add(refresh); Controls.Add(save); Controls.Add(clear); Controls.Add(info);
            refreshTimer.Interval = 1000; refreshTimer.Tick += (s, e) => RefreshLog();
            Shown += (s, e) => { RefreshLog(); refreshTimer.Start(); };
            FormClosed += (s, e) => refreshTimer.Stop();
        }

        private void RefreshLog()
        {
            try
            {
                if (!File.Exists(logPath))
                {
                    output.Text = "Журнал пока не создан. Запусти игру через Game Audio Bridge, чтобы перехватчик записал диагностику.\r\n\r\nОжидаемый файл: " + logPath;
                    info.Text = "Нет журнала"; return;
                }
                string text;
                using (FileStream fs = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (StreamReader reader = new StreamReader(fs, Encoding.UTF8, true)) text = reader.ReadToEnd();
                if (output.Text != text)
                {
                    bool atEnd = output.SelectionStart >= output.TextLength - 2;
                    output.Text = text;
                    if (atEnd) { output.SelectionStart = output.TextLength; output.ScrollToCaret(); }
                }
                string[] lines = text.Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries);
                string latestProblem = null;
                for (int i = lines.Length - 1; i >= 0; --i)
                {
                    if (lines[i].IndexOf("Exact OpenAL redirect unavailable:", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        lines[i].IndexOf("ASIO FORMAT MISMATCH:", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        lines[i].IndexOf("OpenAL output thread did not become ready", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        lines[i].IndexOf("OpenAL source creation failed", StringComparison.OrdinalIgnoreCase) >= 0)
                    { latestProblem = lines[i]; break; }
                }
                if (latestProblem != null)
                {
                    warningBanner.BackColor = Color.FromArgb(255, 205, 205); warningBanner.ForeColor = Color.DarkRed;
                    warningBanner.Text = "ПРЕДУПРЕЖДЕНИЕ: точный вывод через выбранный путь не подтверждён. Исходный путь/статус см. в журнале.";
                    if (alreadyAlerted.Add(latestProblem))
                        MessageBox.Show(this, latestProblem + "\r\n\r\nGAB не должен молча сводить каналы или менять разрядность. Проверь статус исходного аудиопути и выбери совместимый вывод.", "GAB — несовместимость аудиоформата", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
                else
                {
                    warningBanner.BackColor = Color.FromArgb(220, 245, 220); warningBanner.ForeColor = Color.DarkGreen;
                    warningBanner.Text = "Критическая несовместимость не обнаружена в журнале. Это не доказывает побитовую точность физического выхода.";
                }
                FileInfo fi = new FileInfo(logPath);
                info.Text = "Автообновление 1 с | " + fi.Length + " байт | " + logPath;
            }
            catch (Exception ex) { info.Text = "Ошибка чтения журнала: " + ex.Message; }
        }

        private void ClearLog(object sender, EventArgs e)
        {
            DialogResult answer = MessageBox.Show(this,
                "Очистить текущий журнал диагностики Game Audio Bridge?\r\n\r\n" +
                "Содержимое файла GameAudioBridge.log в папке Game Audio Bridge будет удалено. Если игра продолжает работать, новые записи могут снова появиться сразу после очистки.",
                "Очистка журнала", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
            if (answer != DialogResult.Yes) return;

            try
            {
                // The hook opens the log in append mode for each line. Share read/write/delete
                // so clearing can usually proceed while a game is running; report any lock error.
                using (FileStream fs = new FileStream(logPath, FileMode.Create, FileAccess.Write,
                    FileShare.ReadWrite | FileShare.Delete)) { }
                alreadyAlerted.Clear();
                output.Clear();
                info.Text = "Журнал очищен: " + logPath;
                warningBanner.BackColor = Color.FromArgb(220, 245, 220);
                warningBanner.ForeColor = Color.DarkGreen;
                warningBanner.Text = "Журнал очищен. Новые диагностические записи появятся здесь автоматически.";
                RefreshLog();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Не удалось очистить журнал. Возможно, файл занят другим процессом.\r\n\r\n" + ex.Message,
                    "Ошибка очистки журнала", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void SaveLog(object sender, EventArgs e)
        {
            using (SaveFileDialog dlg = new SaveFileDialog())
            {
                dlg.Title = "Сохранить данные аудиомонитора";
                dlg.Filter = "Текстовый файл (*.txt)|*.txt|Все файлы (*.*)|*.*";
                dlg.DefaultExt = "txt"; dlg.AddExtension = true;
                dlg.FileName = "GameAudioBridge_AudioMonitor_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".txt";
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                try
                {
                    string header = "Game Audio Bridge — Audio Monitor\r\nВремя экспорта: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss zzz") +
                        "\r\nИсточник: " + logPath + "\r\nПримечание: монитор показывает только данные, которые перехватчик реально записал; он не может восстановить незарегистрированные параметры завершившегося потока.\r\n\r\n";
                    File.WriteAllText(dlg.FileName, header + output.Text, new UTF8Encoding(true));
                    info.Text = "Сохранено: " + dlg.FileName;
                }
                catch (Exception ex) { MessageBox.Show(this, ex.Message, "Не удалось сохранить журнал", MessageBoxButtons.OK, MessageBoxIcon.Error); }
            }
        }
    }

    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            try { Application.Run(new GameAudioBridgeForm()); }
            catch (Exception ex) { MessageBox.Show(ex.ToString(), "Game Audio Bridge — ошибка запуска", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }
    }
}
