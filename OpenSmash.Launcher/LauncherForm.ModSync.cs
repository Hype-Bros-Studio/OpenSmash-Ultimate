using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace OpenSmash.Launcher
{
    // Mod sync between two PCs over HTTP (tools/modsync.py): one shares, the other matches it.
    // Same network or Tailscale; the sharing PC only reads, the pulling one can undo.
    public partial class LauncherForm
    {
        static Form ModSyncDialog(string title, int w, int h)
        {
            return new Form
            {
                Text = title,
                ClientSize = new Size(w, h),
                FormBorderStyle = FormBorderStyle.FixedDialog,
                MaximizeBox = false,
                MinimizeBox = false,
                StartPosition = FormStartPosition.CenterParent,
                BackColor = Theme.Bg2,
                ForeColor = Theme.Text,
                Font = Theme.Body(15),
            };
        }

        static Button ModSyncButton(string text, int x, int y, int w, bool accent = false)
        {
            var b = new Button
            {
                Text = text,
                Location = new Point(x, y),
                Size = new Size(w, 36),
                FlatStyle = FlatStyle.Flat,
                BackColor = accent ? Theme.Accent : Theme.Bg3,
                ForeColor = Theme.Text,
            };
            b.FlatAppearance.BorderColor = Theme.Dim;
            return b;
        }

        // ---- share ---------------------------------------------------------------------------
        void ModSyncShare()
        {
            if (_modsBusy) return;
            var psi = new ProcessStartInfo
            {
                FileName = _pythonExe,
                WorkingDirectory = _repo,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
            psi.ArgumentList.Add(Path.Combine(_repo, "tools", "modsync.py"));
            psi.ArgumentList.Add("serve");

            using var dlg = ModSyncDialog("OpenSmash - share mods", 560, 330);
            var info = new TextBox
            {
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical,
                Location = new Point(20, 20), Size = new Size(520, 230),
                BackColor = Theme.Bg, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle,
                Font = Theme.Mono(14), Text = "Reading your mods...",
            };
            var stop = ModSyncButton("STOP SHARING", 380, 270, 160, accent: true);
            stop.Click += (_, __) => dlg.Close();
            dlg.Controls.Add(info);
            dlg.Controls.Add(stop);

            var addresses = new List<string>();
            string key = "", summary = "", failure = "";
            void Show()
            {
                if (dlg.IsDisposed) return;
                var t = new StringBuilder();
                if (failure.Length > 0) t.AppendLine("Sharing stopped: " + failure).AppendLine();
                if (key.Length == 0) { t.AppendLine(failure.Length > 0 ? "" : "Reading your mods (a first share hashes every file; it's quicker after)..."); }
                else
                {
                    t.AppendLine(summary.Length > 0 ? summary : "Sharing your turned-on mods.");
                    t.AppendLine();
                    t.AppendLine("On the other PC: Mods > Share > SYNC FROM A PC, and enter");
                    t.AppendLine();
                    foreach (var a in addresses) t.AppendLine("  address  " + a);
                    t.AppendLine("  key      " + key);
                    t.AppendLine();
                    t.AppendLine("Same Wi-Fi/router: the 192.168/10.x address. Tailscale: the 100.x one.");
                    t.AppendLine("Windows may ask to let Python through the firewall: allow it on private networks.");
                    t.AppendLine("Sharing lasts while this window is open.");
                }
                info.Text = t.ToString();
            }

            Process? proc = null;
            var lines = new StringBuilder();
            void Take(string? line)
            {
                if (line == null) return;
                lock (lines) lines.AppendLine(line);
                if (!line.StartsWith("LOG ") && !line.StartsWith("RESULT ")) return;
                string text = line.StartsWith("LOG ") ? line.Substring(4) : "";
                try
                {
                    dlg.BeginInvoke(() =>
                    {
                        if (text.StartsWith("address ")) addresses.Add(text.Substring(8));
                        else if (text.StartsWith("key ")) key = text.Substring(4);
                        else if (text.StartsWith("sharing ")) summary = "Sharing " + text.Substring(8) + ".";
                        else if (line.StartsWith("RESULT "))
                        {
                            var r = OsModsResult(line);
                            if (r is JsonElement e && e.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.False)
                                failure = GetString(e, "error");
                        }
                        Show();
                    });
                }
                catch (InvalidOperationException) { }   // (the window closed)
            }
            dlg.Shown += (_, __) =>
            {
                try
                {
                    proc = Process.Start(psi);
                    if (proc == null) { failure = "python did not start"; Show(); return; }
                    proc.OutputDataReceived += (_, e) => Take(e.Data);
                    proc.ErrorDataReceived += (_, e) => Take(e.Data);
                    proc.EnableRaisingEvents = true;
                    proc.Exited += (_, __) =>
                    {
                        string all;
                        lock (lines) all = lines.ToString();
                        try
                        {
                            dlg.BeginInvoke(() =>
                            {
                                if (failure.Length == 0 && key.Length == 0) failure = Tail(all).Trim();
                                key = ""; Show();
                            });
                        }
                        catch (InvalidOperationException) { }
                    };
                    proc.BeginOutputReadLine();
                    proc.BeginErrorReadLine();
                }
                catch (Exception ex) { failure = ex.Message; Show(); }
            };
            dlg.ShowDialog(this);
            try { if (proc != null && !proc.HasExited) proc.Kill(); } catch { }
            proc?.Dispose();
        }

        // ---- pull ----------------------------------------------------------------------------
        void ModSyncPull()
        {
            if (_modsBusy) return;
            string ini = WindowSmash.ConfigFile(_repo, "launcher.ini");
            string lastAddress = "";
            try { lastAddress = IniValue(ini, "opensmash", "modsync_address") ?? ""; } catch { }
            bool canUndo = Directory.Exists(Path.Combine(_repo, "mods", ".modsync", "backup")) &&
                           Directory.EnumerateFiles(Path.Combine(_repo, "mods", ".modsync", "backup"), "undo.json", SearchOption.AllDirectories).Any();

            using var dlg = ModSyncDialog("OpenSmash - sync mods from a PC", 520, 250);
            var help = new Label
            {
                Text = "Enter what the other PC shows under SHARE OVER NETWORK. You'll see what changes before anything does.",
                Location = new Point(20, 16), Size = new Size(480, 44), ForeColor = Theme.Muted,
            };
            var addrLabel = new Label { Text = "Address", Location = new Point(20, 72), AutoSize = true };
            var addr = new TextBox { Text = lastAddress, Location = new Point(120, 68), Size = new Size(380, 30), BackColor = Theme.Bg, ForeColor = Theme.Text };
            var keyLabel = new Label { Text = "Key", Location = new Point(20, 116), AutoSize = true };
            var key = new TextBox { Location = new Point(120, 112), Size = new Size(180, 30), BackColor = Theme.Bg, ForeColor = Theme.Text, CharacterCasing = CharacterCasing.Upper };
            var go = ModSyncButton("CHECK", 380, 190, 120, accent: true);
            var cancel = ModSyncButton("CANCEL", 250, 190, 120);
            var undo = ModSyncButton("UNDO LAST SYNC", 20, 190, 170);
            undo.Enabled = canUndo;
            string choice = "";
            go.Click += (_, __) =>
            {
                if (addr.Text.Trim().Length == 0 || key.Text.Trim().Length == 0) return;
                choice = "check"; dlg.Close();
            };
            cancel.Click += (_, __) => dlg.Close();
            undo.Click += (_, __) => { choice = "undo"; dlg.Close(); };
            dlg.AcceptButton = go;
            dlg.CancelButton = cancel;
            dlg.Controls.AddRange(new Control[] { help, addrLabel, addr, keyLabel, key, go, cancel, undo });
            dlg.ShowDialog(this);

            if (choice == "undo")
            {
                if (MessageBox.Show("Put your mods back the way they were before the last sync?", "OpenSmash - mod sync",
                        MessageBoxButtons.YesNo) == DialogResult.Yes)
                    RunModSync(new[] { "undo" }, "UNDOING THE SYNC", "Undone: your mods are back as they were.");
                return;
            }
            if (choice != "check") return;
            string address = addr.Text.Trim(), code = key.Text.Trim();
            try { SetIni(ini, "opensmash", "modsync_address", address); } catch { }

            _modsBusy = true;
            SetLoad("ASKING THE OTHER PC", -1);
            Invalidate();
            _ = Task.Run(() =>
            {
                string output = "", why = "";
                try { RunPython(new[] { Path.Combine(_repo, "tools", "modsync.py"), "pull", address, code, "--dry-run" }, out output); }
                catch (Exception ex) { why = ex.Message; }
                BeginInvoke(() =>
                {
                    _modsBusy = false;
                    ClearLoad();
                    Invalidate();
                    var r = OsModsResult(output);
                    if (why.Length == 0 && r is JsonElement e0 && e0.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.False)
                        why = GetString(e0, "error");
                    if (why.Length == 0 && r == null) why = Tail(output).Trim();
                    if (why.Length > 0) { MessageBox.Show("Could not sync: " + why, "OpenSmash - mod sync"); return; }
                    var plan = r!.Value.GetProperty("plan");
                    string summary = ModSyncSummary(plan, out bool nothing);
                    if (nothing) { MessageBox.Show("Your mods already match.", "OpenSmash - mod sync"); return; }
                    if (MessageBox.Show(summary + "\n\nGo ahead? (You can undo it from the same window.)", "OpenSmash - mod sync",
                            MessageBoxButtons.YesNo) != DialogResult.Yes) return;
                    RunModSync(new[] { "pull", address, code }, "SYNCING MODS", "Done: your mods match the other PC's.");
                });
            });
        }

        static string ModSyncSummary(JsonElement plan, out bool nothing)
        {
            string[] Names(string key) => plan.TryGetProperty(key, out var a) && a.ValueKind == JsonValueKind.Array
                ? a.EnumerateArray().Select(x => x.GetString() ?? "").ToArray() : Array.Empty<string>();
            var add = Names("add");
            var off = Names("turn_off");
            var skipped = Names("skipped");
            var update = plan.TryGetProperty("update", out var u) && u.ValueKind == JsonValueKind.Object
                ? u.EnumerateObject().Select(p => p.Name).ToArray() : Array.Empty<string>();
            bool order = plan.TryGetProperty("state_changes", out var sc) && sc.ValueKind == JsonValueKind.True;
            double mb = plan.TryGetProperty("bytes", out var b) ? b.GetDouble() / 1e6 : 0;
            nothing = add.Length == 0 && update.Length == 0 && off.Length == 0 && !order;
            var t = new StringBuilder();
            if (add.Length > 0) t.AppendLine("New: " + string.Join(", ", add));
            if (update.Length > 0) t.AppendLine("Updated: " + string.Join(", ", update));
            if (off.Length > 0) t.AppendLine("Turned off (kept): " + string.Join(", ", off));
            if (skipped.Length > 0) t.AppendLine("Left alone (linked folders): " + string.Join(", ", skipped));
            if (add.Length == 0 && update.Length == 0 && off.Length == 0 && order) t.AppendLine("Only the order or on/off of mods changes.");
            t.AppendLine($"Download: {mb:0.0} MB");
            return t.ToString().Trim();
        }

        void RunModSync(string[] args, string busy, string done)
        {
            _modsBusy = true;
            SetLoad(busy, -1);
            Invalidate();
            _ = Task.Run(() =>
            {
                string output = "", why = "";
                try
                {
                    RunPython(new[] { Path.Combine(_repo, "tools", "modsync.py") }.Concat(args).ToArray(), out output, line =>
                    {
                        if (line.StartsWith("LOG percent ") && int.TryParse(line.Substring(12), out int pct))
                            BeginInvoke(() => { SetLoad(busy, pct / 100f); Invalidate(); });
                        else if (line.StartsWith("LOG ") && !line.StartsWith("LOG percent"))
                            BeginInvoke(() => { SetLoad(busy, -1); Invalidate(); });
                    });
                }
                catch (Exception ex) { why = ex.Message; }
                BeginInvoke(() =>
                {
                    _modsBusy = false;
                    ClearLoad();
                    UpdateMods();
                    _ = PushInstalledState();
                    Invalidate();
                    var r = OsModsResult(output);
                    if (why.Length == 0 && r is JsonElement e && e.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.False)
                        why = GetString(e, "error");
                    if (why.Length == 0 && r == null) why = Tail(output).Trim();
                    if (why.Length > 0) { MessageBox.Show("Mod sync failed: " + why, "OpenSmash - mod sync"); return; }
                    PlayDone();
                    MessageBox.Show(done, "OpenSmash - mod sync");
                });
            });
        }
    }
}
