// ESP USB Wi-Fi Adapter - Windows flasher.
//
// One self-contained EXE: esptool.exe and the firmware images are embedded as resources and unpacked
// to %TEMP% on first use. Written for the C# 5 compiler that ships with Windows
// (%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe), so it builds without any SDK.
// See scripts/build.ps1.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Ports;
using System.Linq;
using System.Management;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("ESP USB Wi-Fi Adapter Flasher")]
[assembly: AssemblyProduct("ESP USB Wi-Fi Adapter")]
[assembly: AssemblyDescription("Flashes the ESP USB Wi-Fi Adapter firmware onto ESP32-S2 / ESP32-S3 boards")]
[assembly: AssemblyCopyright("MIT License")]
[assembly: AssemblyVersion(EspUsbWifiFlasher.BuildInfo.Version + ".0")]
[assembly: AssemblyFileVersion(EspUsbWifiFlasher.BuildInfo.Version + ".0")]

namespace EspUsbWifiFlasher
{
    static class Program
    {
        [DllImport("user32.dll")]
        static extern bool SetProcessDPIAware();

        [STAThread]
        static int Main(string[] args)
        {
            if (args.Length > 0)
            {
                return Cli.Run(args);
            }
            try { SetProcessDPIAware(); } catch (Exception) { }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
            return 0;
        }
    }

    // ---------------------------------------------------------------- chips & firmware

    class ChipInfo
    {
        public string Id;           // esptool --chip value
        public string Name;         // as printed by esptool
        public bool Supported;
        public string Boards;

        public static readonly ChipInfo[] All =
        {
            new ChipInfo { Id = "esp32s3", Name = "ESP32-S3", Supported = true,
                           Boards = "ESP32-S3-DevKitC-1, N8R2/N16R8 boards, YD-ESP32-S3, XIAO ESP32S3, S3 Zero/Mini..." },
            new ChipInfo { Id = "esp32s2", Name = "ESP32-S2", Supported = true,
                           Boards = "ESP32-S2-Saola-1, ESP32-S2-DevKitM-1, LOLIN S2 Mini, S2 Zero..." },
            new ChipInfo { Id = "esp32c3", Name = "ESP32-C3" },
            new ChipInfo { Id = "esp32c5", Name = "ESP32-C5" },
            new ChipInfo { Id = "esp32c6", Name = "ESP32-C6" },
            new ChipInfo { Id = "esp32c2", Name = "ESP32-C2" },
            new ChipInfo { Id = "esp32h2", Name = "ESP32-H2" },
            new ChipInfo { Id = "esp32p4", Name = "ESP32-P4" },
            new ChipInfo { Id = "esp8266", Name = "ESP8266" },
            new ChipInfo { Id = "esp32", Name = "ESP32" },     // last: "ESP32" is a prefix of the others
        };

        public static ChipInfo FromEsptoolOutput(string output)
        {
            foreach (ChipInfo c in All)
            {
                if (Regex.IsMatch(output, @"\b" + Regex.Escape(c.Name) + @"(?![-\w])"))
                {
                    return c;
                }
            }
            return null;
        }

        public static ChipInfo ById(string id)
        {
            return All.FirstOrDefault(c => c.Id == id);
        }

        public string WhyUnsupported()
        {
            if (Id == "esp32p4")
            {
                return "The ESP32-P4 has USB device hardware but no Wi-Fi radio.";
            }
            return Name + " has no USB device (OTG) hardware: its USB port is either a separate USB-to-serial "
                 + "chip or a fixed serial/JTAG function, so it cannot appear to the PC as a network card. "
                 + "Only the ESP32-S2 and ESP32-S3 can do this.";
        }
    }

    static class BuildInfo
    {
        public const string Version = "1.0.0";
    }

    // ---------------------------------------------------------------- embedded payload

    static class Payload
    {
        static string dir;

        public static string Dir
        {
            get
            {
                if (dir == null)
                {
                    // One folder per build of this EXE, so a rebuilt flasher never reuses stale images
                    Assembly asm = Assembly.GetExecutingAssembly();
                    string build = BuildInfo.Version + "-" + File.GetLastWriteTimeUtc(asm.Location).Ticks.ToString("x");
                    string parent = Path.Combine(Path.GetTempPath(), "EspUsbWifiFlasher");
                    dir = Path.Combine(parent, build);
                    if (Directory.Exists(parent))
                    {
                        foreach (string old in Directory.GetDirectories(parent))
                        {
                            if (!old.Equals(dir, StringComparison.OrdinalIgnoreCase))
                            {
                                try { Directory.Delete(old, true); } catch (Exception) { }
                            }
                        }
                    }
                    Directory.CreateDirectory(dir);
                    foreach (string name in asm.GetManifestResourceNames())
                    {
                        string target = Path.Combine(dir, name);
                        using (Stream src = asm.GetManifestResourceStream(name))
                        {
                            if (File.Exists(target) && new FileInfo(target).Length == src.Length)
                            {
                                continue;   // already unpacked by this same build
                            }
                            using (FileStream dst = File.Create(target))
                            {
                                src.CopyTo(dst);
                            }
                        }
                    }
                }
                return dir;
            }
        }

        public static string Esptool { get { return Path.Combine(Dir, "esptool.exe"); } }

        public static string Firmware(string chip, bool full)
        {
            return Path.Combine(Dir, chip + (full ? "-full.bin" : "-app.bin"));
        }

        public static bool HasFirmware(string chip)
        {
            return File.Exists(Firmware(chip, true)) && File.Exists(Firmware(chip, false));
        }
    }

    // ---------------------------------------------------------------- serial ports

    enum PortKind { Other, UartBridge, EspBootloader, AdapterRunning, EspNative }

    class PortInfo
    {
        public string Port;
        public string Description;
        public string Vid = "", Pid = "";
        public PortKind Kind;

        public string Hint
        {
            get
            {
                switch (Kind)
                {
                    case PortKind.AdapterRunning:
                        return "The adapter firmware is running here - it will be switched to its bootloader automatically.";
                    case PortKind.EspBootloader:
                        return "ESP native USB in bootloader/serial-JTAG mode - ready to flash.";
                    case PortKind.EspNative:
                        return "ESP native USB port - if flashing fails, hold BOOT, tap RESET, release BOOT and retry.";
                    case PortKind.UartBridge:
                        return "USB-to-UART bridge (the board's \"COM\"/\"UART\" port) - ready to flash.";
                    default:
                        return "Not recognised as an ESP board, but you can try it.";
                }
            }
        }

        public override string ToString()
        {
            string tag = Kind == PortKind.AdapterRunning ? "  [adapter running]"
                       : Kind == PortKind.EspBootloader ? "  [ESP bootloader]"
                       : Kind == PortKind.EspNative ? "  [ESP native USB]"
                       : Kind == PortKind.UartBridge ? "  [USB-UART]" : "";
            return Port + "  -  " + Description + tag;
        }
    }

    static class Ports
    {
        public static List<PortInfo> List()
        {
            var result = new Dictionary<string, PortInfo>(StringComparer.OrdinalIgnoreCase);
            try
            {
                using (var searcher = new ManagementObjectSearcher(
                    "SELECT Name, DeviceID FROM Win32_PnPEntity WHERE Name LIKE '%(COM%'"))
                {
                    foreach (ManagementObject o in searcher.Get())
                    {
                        string name = (o["Name"] ?? "").ToString();
                        string id = (o["DeviceID"] ?? "").ToString();
                        Match m = Regex.Match(name, @"\((COM\d+)\)");
                        if (!m.Success)
                        {
                            continue;
                        }
                        var p = new PortInfo { Port = m.Groups[1].Value, Description = name.Replace(m.Value, "").Trim() };
                        Match ids = Regex.Match(id, @"VID_([0-9A-F]{4}).*PID_([0-9A-F]{4})", RegexOptions.IgnoreCase);
                        if (ids.Success)
                        {
                            p.Vid = ids.Groups[1].Value.ToUpperInvariant();
                            p.Pid = ids.Groups[2].Value.ToUpperInvariant();
                        }
                        p.Kind = Classify(p.Vid, p.Pid);
                        result[p.Port] = p;
                    }
                }
            }
            catch (Exception) { }
            foreach (string name in SerialPort.GetPortNames())
            {
                if (!result.ContainsKey(name))
                {
                    result[name] = new PortInfo { Port = name, Description = "Serial port" };
                }
            }
            return result.Values
                .OrderByDescending(p => p.Kind != PortKind.Other)
                .ThenBy(p => int.Parse(Regex.Match(p.Port, @"\d+").Value))
                .ToList();
        }

        static PortKind Classify(string vid, string pid)
        {
            if (vid == "303A")
            {
                if (pid == "4001") return PortKind.AdapterRunning;
                if (pid == "1001" || pid == "0002") return PortKind.EspBootloader;
                return PortKind.EspNative;
            }
            if (vid == "1A86" || vid == "10C4" || vid == "0403" || vid == "067B")
            {
                return PortKind.UartBridge;     // WCH CH34x, Silabs CP210x, FTDI, Prolific
            }
            return PortKind.Other;
        }

        // Arduino-style "1200 baud touch": the adapter firmware reboots into the ROM bootloader, which
        // shows up as a new port. Returns that port, or null.
        public static string EnterBootloader(PortInfo port, Action<string> log)
        {
            log("Asking the running firmware on " + port.Port + " to enter its bootloader...");
            try
            {
                using (var sp = new SerialPort(port.Port, 1200))
                {
                    sp.Open();
                    Thread.Sleep(200);
                    sp.DtrEnable = false;
                    sp.Close();
                }
            }
            catch (Exception) { }   // the port disappears as the board reboots
            DateTime deadline = DateTime.Now.AddSeconds(15);
            while (DateTime.Now < deadline)
            {
                Thread.Sleep(500);
                PortInfo boot = List().FirstOrDefault(p => p.Kind == PortKind.EspBootloader);
                if (boot != null)
                {
                    Thread.Sleep(1000);         // let Windows finish installing the port
                    log("Bootloader is on " + boot.Port + ".");
                    return boot.Port;
                }
            }
            return null;
        }
    }

    // ---------------------------------------------------------------- esptool runner

    class Esptool
    {
        public Action<string> Log = delegate { };
        public Action<double> Progress = delegate { };

        static readonly Regex ProgressRx = new Regex(@"(\d{1,3}(?:\.\d+)?)\s?%");

        // Runs esptool and returns (exit code, full output)
        public int Run(string args, out string output)
        {
            Log("> esptool " + args);
            var psi = new ProcessStartInfo(Payload.Esptool, args)
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            psi.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
            psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
            psi.EnvironmentVariables["NO_COLOR"] = "1";
            var all = new StringBuilder();
            using (Process p = Process.Start(psi))
            {
                Thread err = new Thread(() => Pump(p.StandardError, all)) { IsBackground = true };
                err.Start();
                Pump(p.StandardOutput, all);
                err.Join();
                p.WaitForExit();
                output = all.ToString();
                return p.ExitCode;
            }
        }

        void Pump(StreamReader reader, StringBuilder all)
        {
            var line = new StringBuilder();
            var buf = new char[256];
            int n;
            while ((n = reader.Read(buf, 0, buf.Length)) > 0)
            {
                for (int i = 0; i < n; i++)
                {
                    char c = buf[i];
                    if (c == '\r' || c == '\n')
                    {
                        Emit(line, all);
                    }
                    else
                    {
                        line.Append(c);
                        if (c == '%')           // progress updates may not end in a newline
                        {
                            MatchCollection ms = ProgressRx.Matches(line.ToString());
                            if (ms.Count > 0)
                            {
                                Progress(double.Parse(ms[ms.Count - 1].Groups[1].Value,
                                                      System.Globalization.CultureInfo.InvariantCulture));
                            }
                        }
                    }
                }
            }
            Emit(line, all);
        }

        void Emit(StringBuilder line, StringBuilder all)
        {
            if (line.Length == 0)
            {
                return;
            }
            string text = line.ToString();
            line.Clear();
            lock (all)
            {
                all.AppendLine(text);
            }
            if (!ProgressRx.IsMatch(text) || !text.StartsWith("Writing at"))
            {
                Log(text);
            }
        }
    }

    // ---------------------------------------------------------------- flashing job

    class FlashJob
    {
        public PortInfo Port;
        public string ForcedChip;       // null = auto-detect
        public bool UpdateOnly;         // keep Wi-Fi credentials and settings
        public bool EraseAll;
        public int Baud = 921600;

        public Action<string> Log = delegate { };
        public Action<double> Progress = delegate { };
        public Action<string> Status = delegate { };

        public string Error;
        public ChipInfo Chip;

        public bool Run()
        {
            try
            {
                return RunInner();
            }
            catch (Exception ex)
            {
                Error = ex.Message;
                return false;
            }
        }

        bool RunInner()
        {
            Status("Preparing...");
            if (!File.Exists(Payload.Esptool))
            {
                Error = "esptool.exe is missing from the flasher package.";
                return false;
            }
            var esptool = new Esptool { Log = Log, Progress = Progress };
            string port = Port.Port;
            string output;

            if (Port.Kind == PortKind.AdapterRunning || Port.Kind == PortKind.EspNative)
            {
                Status("Switching the board to its bootloader...");
                string boot = Ports.EnterBootloader(Port, Log);
                if (boot == null)
                {
                    if (Port.Kind == PortKind.AdapterRunning)
                    {
                        Error = "The board did not enter its bootloader. Hold BOOT, tap RESET (or re-plug the cable "
                              + "while holding BOOT), release BOOT, pick the new port and try again.";
                        return false;
                    }
                    Log("No bootloader port appeared; trying " + port + " directly.");
                }
                else
                {
                    port = boot;
                }
            }

            Status("Detecting the chip...");
            int rc = esptool.Run(string.Format("--port {0} --before default-reset --after no-reset chip-id", port), out output);
            Chip = ChipInfo.FromEsptoolOutput(output);
            if (rc != 0 || Chip == null)
            {
                Error = "Could not talk to the board on " + port + ".\r\n\r\n"
                      + "- Make sure the right port is selected and no other program (Arduino IDE serial monitor, etc.) is using it.\r\n"
                      + "- Put the board in download mode: hold BOOT, tap RESET (or plug the cable in while holding BOOT), release BOOT.\r\n"
                      + "- Try a lower baud rate or another USB cable (some cables are charge-only).";
                return false;
            }
            Log("Detected " + Chip.Name + ".");
            if (!Chip.Supported)
            {
                Error = "This board has an " + Chip.Name + " chip, which is not supported.\r\n\r\n" + Chip.WhyUnsupported();
                return false;
            }
            if (ForcedChip != null && ForcedChip != Chip.Id)
            {
                Error = "You selected " + ChipInfo.ById(ForcedChip).Name + " but the board is an " + Chip.Name
                      + ". Select \"Auto-detect\" or the right chip.";
                return false;
            }
            if (!Payload.HasFirmware(Chip.Id))
            {
                Error = "This flasher package has no firmware for " + Chip.Name + ".";
                return false;
            }

            string image = Payload.Firmware(Chip.Id, !UpdateOnly);
            string offset = UpdateOnly ? "0x10000" : "0x0";
            Status(EraseAll ? "Erasing and writing flash..." : "Writing flash...");
            Progress(0);
            rc = esptool.Run(string.Format(
                "--chip {0} --port {1} --baud {2} --before no-reset --after hard-reset write-flash {3}-z {4} \"{5}\"",
                Chip.Id, port, Baud, EraseAll ? "--erase-all " : "", offset, image), out output);
            if (rc != 0)
            {
                if (Baud > 115200 && Regex.IsMatch(output, "(?i)timed out|serial|packet|invalid head"))
                {
                    Log("Write failed at " + Baud + " baud, retrying at 115200...");
                    rc = esptool.Run(string.Format(
                        "--chip {0} --port {1} --baud 115200 --before default-reset --after hard-reset write-flash {2}-z {3} \"{4}\"",
                        Chip.Id, port, EraseAll ? "--erase-all " : "", offset, image), out output);
                }
                if (rc != 0)
                {
                    Error = "Writing the flash failed (see the log). Re-plug the board, put it in download mode "
                          + "(hold BOOT, tap RESET, release BOOT) and try again, or use a lower baud rate.";
                    return false;
                }
            }
            Progress(100);
            Status("Done.");
            return true;
        }
    }

    // ---------------------------------------------------------------- command line

    static class Cli
    {
        [DllImport("kernel32.dll")]
        static extern bool AttachConsole(int pid);

        public static int Run(string[] args)
        {
            AttachConsole(-1);  // show output when started from a console (no-op when piped)
            var job = new FlashJob();
            string portName = null;
            for (int i = 0; i < args.Length; i++)
            {
                string a = args[i].ToLowerInvariant();
                string next = i + 1 < args.Length ? args[i + 1] : null;
                if (a == "--list")
                {
                    foreach (PortInfo p in Ports.List())
                    {
                        Console.WriteLine(p);
                    }
                    return 0;
                }
                else if (a == "--port" && next != null) { portName = next; i++; }
                else if (a == "--chip" && next != null) { job.ForcedChip = next.ToLowerInvariant() == "auto" ? null : next.ToLowerInvariant(); i++; }
                else if (a == "--baud" && next != null) { job.Baud = int.Parse(next); i++; }
                else if (a == "--update") { job.UpdateOnly = true; }
                else if (a == "--erase") { job.EraseAll = true; }
                else
                {
                    Console.WriteLine("ESP USB Wi-Fi Adapter Flasher v" + BuildInfo.Version);
                    Console.WriteLine();
                    Console.WriteLine("Usage: ESP-USB-WiFi-Flasher.exe                   (opens the window)");
                    Console.WriteLine("       ESP-USB-WiFi-Flasher.exe --list            (lists serial ports)");
                    Console.WriteLine("       ESP-USB-WiFi-Flasher.exe --port COMx [--chip auto|esp32s3|esp32s2]");
                    Console.WriteLine("                                [--update] [--erase] [--baud 921600]");
                    Console.WriteLine();
                    Console.WriteLine("  --update  only replace the application, keeping Wi-Fi credentials and settings");
                    Console.WriteLine("  --erase   erase the whole flash before writing");
                    return a == "--help" || a == "-h" || a == "/?" ? 0 : 2;
                }
            }
            if (portName == null)
            {
                Console.WriteLine("Missing --port COMx (use --list to see the ports).");
                return 2;
            }
            job.Port = Ports.List().FirstOrDefault(p => p.Port.Equals(portName, StringComparison.OrdinalIgnoreCase))
                       ?? new PortInfo { Port = portName, Description = "" };
            job.Log = s => Console.WriteLine(s);
            job.Status = s => Console.WriteLine("== " + s);
            int lastPct = -10;
            job.Progress = pct =>
            {
                if ((int)pct >= lastPct + 10 || pct >= 100)
                {
                    lastPct = (int)pct;
                    Console.WriteLine("   " + (int)pct + "%");
                }
            };
            if (job.Run())
            {
                Console.WriteLine("Flashed " + job.Chip.Name + " successfully.");
                return 0;
            }
            Console.WriteLine("ERROR: " + job.Error);
            return 1;
        }
    }

    // ---------------------------------------------------------------- window

    class MainForm : Form
    {
        ComboBox portBox, chipBox, baudBox;
        RadioButton fullRadio, updateRadio;
        CheckBox eraseBox;
        Button flashButton, refreshButton, setupButton;
        ProgressBar progress;
        Label statusLabel, portHint;
        TextBox logBox;
        bool busy;

        public MainForm()
        {
            Text = "ESP USB Wi-Fi Adapter Flasher  v" + BuildInfo.Version;
            Font = new Font("Segoe UI", 9f);
            AutoScaleMode = AutoScaleMode.Dpi;
            AutoScaleDimensions = new SizeF(96f, 96f);
            ClientSize = new Size(700, 640);
            MinimumSize = new Size(600, 560);
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }

            var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Padding = new Padding(14) };
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            Controls.Add(root);

            var title = new Label
            {
                Text = "Turn an ESP32-S3 / ESP32-S2 board into a USB Wi-Fi adapter",
                Font = new Font("Segoe UI Semibold", 12f),
                AutoSize = true,
                Margin = new Padding(0, 0, 0, 8),
            };
            root.Controls.Add(title);

            // 1. Board
            var boardGroup = new GroupBox { Text = "1. Board", Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(10) };
            var bg = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, AutoSize = true };
            bg.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            bg.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            bg.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            bg.Controls.Add(new Label { Text = "Port:", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 0);
            portBox = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
            portBox.SelectedIndexChanged += (s, e) => UpdatePortHint();
            bg.Controls.Add(portBox, 1, 0);
            refreshButton = new Button { Text = "Refresh", AutoSize = true };
            refreshButton.Click += (s, e) => RefreshPorts();
            bg.Controls.Add(refreshButton, 2, 0);
            portHint = new Label { AutoSize = true, ForeColor = SystemColors.GrayText, Margin = new Padding(3, 2, 3, 8) };
            bg.Controls.Add(portHint, 1, 1);
            bg.Controls.Add(new Label { Text = "Chip:", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 2);
            chipBox = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
            chipBox.Items.Add("Auto-detect (recommended)");
            foreach (ChipInfo c in ChipInfo.All.Where(c => c.Supported))
            {
                chipBox.Items.Add(c.Name + "  -  " + c.Boards);
            }
            chipBox.SelectedIndex = 0;
            bg.Controls.Add(chipBox, 1, 2);
            boardGroup.Controls.Add(bg);
            root.Controls.Add(boardGroup);

            // 2. Options
            var optGroup = new GroupBox { Text = "2. Options", Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(10) };
            var og = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
            fullRadio = new RadioButton { Text = "Full install  (new board, or start over: clears saved Wi-Fi and settings)", AutoSize = true, Checked = true };
            updateRadio = new RadioButton { Text = "Update firmware  (keeps saved Wi-Fi and settings)", AutoSize = true };
            eraseBox = new CheckBox { Text = "Erase the whole flash first  (if the board previously ran something else and misbehaves)", AutoSize = true };
            updateRadio.CheckedChanged += (s, e) => { eraseBox.Enabled = !updateRadio.Checked; if (updateRadio.Checked) eraseBox.Checked = false; };
            var baudRow = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, Margin = new Padding(0, 6, 0, 0) };
            baudRow.Controls.Add(new Label { Text = "Speed (baud):", AutoSize = true, Margin = new Padding(0, 6, 6, 0) });
            baudBox = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 110 };
            baudBox.Items.AddRange(new object[] { "921600", "460800", "230400", "115200" });
            baudBox.SelectedIndex = 0;
            baudRow.Controls.Add(baudBox);
            og.Controls.Add(fullRadio);
            og.Controls.Add(updateRadio);
            og.Controls.Add(eraseBox);
            og.Controls.Add(baudRow);
            optGroup.Controls.Add(og);
            root.Controls.Add(optGroup);

            // 3. Flash
            var actions = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Top, Margin = new Padding(0, 8, 0, 4) };
            flashButton = new Button { Text = "Flash", AutoSize = true, Font = new Font("Segoe UI Semibold", 10f), Padding = new Padding(18, 4, 18, 4) };
            flashButton.Click += (s, e) => StartFlash();
            setupButton = new Button { Text = "Open setup page", AutoSize = true, Padding = new Padding(6, 4, 6, 4) };
            setupButton.Click += (s, e) => OpenUrl("http://192.168.7.1/");
            var helpButton = new Button { Text = "Help", AutoSize = true, Padding = new Padding(6, 4, 6, 4) };
            helpButton.Click += (s, e) => ShowHelp();
            actions.Controls.Add(flashButton);
            actions.Controls.Add(setupButton);
            actions.Controls.Add(helpButton);
            root.Controls.Add(actions);

            var progressRow = new TableLayoutPanel { Dock = DockStyle.Top, ColumnCount = 1, AutoSize = true };
            progress = new ProgressBar { Dock = DockStyle.Top, Height = 18, Maximum = 1000 };
            statusLabel = new Label { Text = "Connect the board and pick its port.", AutoSize = true, Margin = new Padding(0, 4, 0, 4) };
            progressRow.Controls.Add(progress);
            progressRow.Controls.Add(statusLabel);
            root.Controls.Add(progressRow);

            logBox = new TextBox
            {
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Vertical,
                Dock = DockStyle.Fill,
                Font = new Font("Consolas", 8.5f),
                BackColor = SystemColors.Window,
            };
            root.Controls.Add(logBox);

            var footer = new LinkLabel
            {
                Text = "Supported: ESP32-S3, ESP32-S2.  Not possible: ESP32, ESP32-C3/C6, ESP8266 - why?",
                AutoSize = true,
                Margin = new Padding(0, 8, 0, 0),
            };
            footer.LinkArea = new LinkArea(footer.Text.Length - 4, 4);
            footer.LinkClicked += (s, e) => MessageBox.Show(this,
                "This works by making the board appear to the PC as a USB network card. That needs USB device (OTG) "
                + "hardware inside the chip, which only the ESP32-S2 and ESP32-S3 have (among ESP chips with Wi-Fi).\r\n\r\n"
                + "On the classic ESP32, ESP8266 and most ESP32-C3/C6 boards, the USB connector goes to a separate "
                + "USB-to-serial chip (or a fixed serial/JTAG unit), so the PC can only ever see a COM port.",
                "Why only ESP32-S2 / ESP32-S3?", MessageBoxButtons.OK, MessageBoxIcon.Information);
            root.Controls.Add(footer);

            Shown += (s, e) =>
            {
                RefreshPorts();
                Log("ESP USB Wi-Fi Adapter Flasher v" + BuildInfo.Version);
                Log("1) Plug the board in (any USB port of the board works for flashing).");
                Log("2) Pick its port, click Flash.");
                Log("3) Afterwards, connect the board's native USB port (often labelled \"USB\") to the PC.");
                Log("");
            };
        }

        void RefreshPorts()
        {
            string previous = portBox.SelectedItem is PortInfo ? ((PortInfo)portBox.SelectedItem).Port : null;
            portBox.Items.Clear();
            foreach (PortInfo p in Ports.List())
            {
                portBox.Items.Add(p);
            }
            int select = -1;
            for (int i = 0; i < portBox.Items.Count; i++)
            {
                var p = (PortInfo)portBox.Items[i];
                if (p.Port == previous) { select = i; break; }
                if (select < 0 && p.Kind != PortKind.Other) select = i;
            }
            if (select < 0 && portBox.Items.Count > 0) select = 0;
            portBox.SelectedIndex = select;
            if (portBox.Items.Count == 0)
            {
                portHint.Text = "No serial ports found. Plug the board in (try another cable if nothing appears).";
            }
            UpdatePortHint();
        }

        void UpdatePortHint()
        {
            var p = portBox.SelectedItem as PortInfo;
            if (p != null)
            {
                portHint.Text = p.Hint;
            }
        }

        void Log(string text)
        {
            if (InvokeRequired)
            {
                BeginInvoke(new Action<string>(Log), text);
                return;
            }
            logBox.AppendText(text + "\r\n");
        }

        void SetProgress(double pct)
        {
            if (InvokeRequired)
            {
                BeginInvoke(new Action<double>(SetProgress), pct);
                return;
            }
            progress.Value = Math.Max(0, Math.Min(1000, (int)(pct * 10)));
        }

        void SetStatus(string text)
        {
            if (InvokeRequired)
            {
                BeginInvoke(new Action<string>(SetStatus), text);
                return;
            }
            statusLabel.Text = text;
        }

        void SetBusy(bool value)
        {
            busy = value;
            flashButton.Enabled = refreshButton.Enabled = portBox.Enabled = chipBox.Enabled = !value;
            fullRadio.Enabled = updateRadio.Enabled = baudBox.Enabled = !value;
            eraseBox.Enabled = !value && !updateRadio.Checked;
            UseWaitCursor = value;
        }

        void StartFlash()
        {
            var port = portBox.SelectedItem as PortInfo;
            if (port == null)
            {
                MessageBox.Show(this, "Pick the board's port first.", Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            var supported = ChipInfo.All.Where(c => c.Supported).ToArray();
            var job = new FlashJob
            {
                Port = port,
                ForcedChip = chipBox.SelectedIndex > 0 ? supported[chipBox.SelectedIndex - 1].Id : null,
                UpdateOnly = updateRadio.Checked,
                EraseAll = eraseBox.Checked,
                Baud = int.Parse((string)baudBox.SelectedItem),
                Log = Log,
                Progress = SetProgress,
                Status = SetStatus,
            };
            SetBusy(true);
            progress.Value = 0;
            Log("---- " + DateTime.Now.ToString("HH:mm:ss") + "  " + (job.UpdateOnly ? "update" : "full install") + " on " + port.Port);
            var thread = new Thread(() =>
            {
                bool ok = job.Run();
                BeginInvoke(new Action(() => Finish(job, ok)));
            }) { IsBackground = true };
            thread.Start();
        }

        void Finish(FlashJob job, bool ok)
        {
            SetBusy(false);
            RefreshPorts();
            if (ok)
            {
                SetStatus("Done - " + job.Chip.Name + " flashed.");
                Log("Success.");
                MessageBox.Show(this,
                    "The " + job.Chip.Name + " is flashed and restarting.\r\n\r\n"
                    + "Next:\r\n"
                    + "1. Connect the board's native USB port (often labelled \"USB\") to this PC.\r\n"
                    + "2. On first start it opens a Wi-Fi hotspot \"ESP-USB-WiFi-XXXX\". Join it from your phone, or open "
                    + "http://192.168.7.1 on this PC, and pick your Wi-Fi network.\r\n"
                    + "3. The PC gets internet through the board. The LED turns green when everything works.",
                    "Flashed successfully", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            else
            {
                SetStatus("Failed.");
                Log("ERROR: " + job.Error);
                MessageBox.Show(this, job.Error, "Flashing failed", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        void ShowHelp()
        {
            MessageBox.Show(this,
                "Which port?\r\n"
                + "Boards with two USB connectors: either works for flashing. The one labelled \"COM\"/\"UART\" shows up as "
                + "CH343/CP210x; the one labelled \"USB\" is the ESP's own USB, which is the one that gives the PC internet.\r\n\r\n"
                + "\"Could not talk to the board\"\r\n"
                + "Put it in download mode: hold BOOT, tap RESET (or plug in while holding BOOT), release BOOT, click Refresh, "
                + "pick the new port, Flash.\r\n\r\n"
                + "After flashing\r\n"
                + "The PC shows a new network adapter \"ESP USB Wi-Fi Adapter\". Setup page: http://192.168.7.1 (from the PC) "
                + "or http://192.168.4.1 (from the setup hotspot).\r\n\r\n"
                + "LED\r\n"
                + "Blue blink: choose a Wi-Fi network.  Yellow: connecting.  Red: Wi-Fi has no internet.  "
                + "Cyan: online but no PC on the USB port.  Green: all good.  The LED GPIO can be changed in the setup page's "
                + "Settings if your board's LED stays dark.",
                "Help", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        void OpenUrl(string url)
        {
            try { Process.Start(url); } catch (Exception) { }
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (busy && MessageBox.Show(this, "Flashing is in progress. Interrupting it can leave the board unusable until it is flashed again. Quit anyway?",
                                        Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
            {
                e.Cancel = true;
            }
            base.OnFormClosing(e);
        }
    }
}
