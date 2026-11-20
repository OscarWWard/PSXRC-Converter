using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

internal sealed class ConverterForm : Form
{
    private readonly TextBox input = new TextBox(), output = new TextBox();
    private readonly Button browse = new Button { Text = "Browse...", AutoSize = true };
    private readonly Button save = new Button { Text = "Browse...", AutoSize = true };
    private readonly Button run = new Button { Text = "Convert", AutoSize = true, MinimumSize = new Size(105, 30) };
    private readonly TabControl tabs = new TabControl { Dock = DockStyle.Fill };
    private readonly TextBox log = new TextBox { Dock = DockStyle.Fill, ReadOnly = true, Multiline = true,
        ScrollBars = ScrollBars.Vertical, BackColor = Color.White, AccessibleName = "Conversion log" };
    private readonly Label status = Caption("Choose an input disc and output file.");
    private readonly ProgressBar progress = new ProgressBar { Width = 90, Height = 14, Style = ProgressBarStyle.Marquee,
        MarqueeAnimationSpeed = 0, Anchor = AnchorStyles.Left, Visible = false };
    private readonly CheckBox video = Toggle("Compress eligible movies with H.264");
    private readonly CheckBox xa = Toggle("Compress eligible XA audio");
    private readonly CheckBox cdda = Toggle("Compress eligible CD audio tracks");
    private readonly NumericUpDown discLevel = Number(1, 19, 8);
    private readonly NumericUpDown videoValue = Number(0, 51, 20);
    private readonly NumericUpDown mse = Number(1, 1024, 103);
    private readonly NumericUpDown refs = Number(0, 16, 0);
    private readonly NumericUpDown vbvRate = Number(0, 50000, 0), vbvBuffer = Number(0, 50000, 0);
    private readonly NumericUpDown audioBitrate = Number(16, 256, 112), audioRatio = Number(1, 1024, 128);
    private readonly ComboBox movieQuality = Choice(1, "High", "Balanced", "Compact", "Custom");
    private readonly ComboBox audioQuality = Choice(1, "High", "Balanced", "Compact", "Custom");
    private readonly ComboBox rate = Choice(0, "CRF (constant quality)", "QP (constant quantizer)", "ABR (average bitrate)");
    private readonly ComboBox preset = Choice(5, "ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo");
    private readonly ComboBox tune = Choice(0, "none", "film", "animation", "grain", "stillimage", "psnr", "ssim", "fastdecode");
    private readonly ComboBox profile = Choice(2, "baseline", "main", "high", "high444");
    private readonly ComboBox level = Choice(0, "auto", "1.0", "1b", "1.1", "1.2", "1.3", "2.0", "2.1", "2.2", "3.0",
        "3.1", "3.2", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2", "6.0", "6.1", "6.2");
    private readonly TextBox extra = new TextBox { MaxLength = 1024 };
    private readonly ToolTip tips = new ToolTip();
    private bool busy, applyingPreset;

    [STAThread]
    public static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new ConverterForm());
    }

    private ConverterForm()
    {
        Text = "PSXRC Converter";
        Font = new Font("Segoe UI", 9F);
        AutoScaleMode = AutoScaleMode.Font;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(780, 712);
        MinimumSize = new Size(635, 545);
        BackColor = Color.FromArgb(246, 248, 251);

        var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Padding = new Padding(16, 12, 16, 12) };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 75));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 25));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        Controls.Add(root);
        root.Controls.Add(new Label { Text = Text, AutoSize = true, Font = new Font("Segoe UI Semibold", 19),
            ForeColor = Color.FromArgb(29, 42, 61), Margin = new Padding(3, 0, 3, 9) }, 0, 0);
        var paths = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 3 };
        paths.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
        paths.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        paths.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        PathRow(paths, 0, "Input disc", input, browse);
        PathRow(paths, 1, "Output file", output, save);
        root.Controls.Add(paths, 0, 1);
        root.Controls.Add(tabs, 0, 2);
        root.Controls.Add(log, 0, 3);
        var footer = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 3 };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        footer.Controls.Add(status, 0, 0);
        footer.Controls.Add(progress, 1, 0);
        footer.Controls.Add(run, 2, 0);
        root.Controls.Add(footer, 0, 4);

        var disc = Tab("Disc");
        Row(disc, "Zstandard level", discLevel);
        Line(disc, Caption("Disc hunks are compressed losslessly. Higher levels usually take longer to encode."));
        BuildMovieTab();
        BuildAudioTab();
        tabs.SelectedIndexChanged += (sender, e) =>
        {
            var page = tabs.SelectedTab;
            if (page != null && IsHandleCreated)
                BeginInvoke(new Action(() => { if (!page.IsDisposed) page.AutoScrollPosition = Point.Empty; }));
        };
        browse.Click += (sender, e) => PickInput();
        save.Click += (sender, e) => PickOutput();
        run.Click += ConvertDisc;
        FormClosing += (sender, e) =>
        {
            e.Cancel = busy;
            if (busy)
                status.Text = "Conversion is running. This window will stay open until it finishes.";
        };
    }

    private static Label Caption(string text)
    {
        return new Label { Text = text, AutoSize = true, Anchor = AnchorStyles.Left, MaximumSize = new Size(610, 0) };
    }

    private static CheckBox Toggle(string text)
    {
        return new CheckBox { Text = text, Checked = true, AutoSize = true };
    }

    private static NumericUpDown Number(int min, int max, int value)
    {
        return new NumericUpDown { Minimum = min, Maximum = max, Value = value, Width = 113 };
    }

    private static ComboBox Choice(int selected, params string[] values)
    {
        var box = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
        box.Items.AddRange(values);
        box.SelectedIndex = selected;
        return box;
    }

    private static void PathRow(TableLayoutPanel panel, int row, string label, TextBox field, Button picker)
    {
        field.Dock = DockStyle.Fill;
        field.AccessibleName = label;
        picker.AccessibleName = "Browse for " + label.ToLowerInvariant();
        panel.Controls.Add(Caption(label), 0, row);
        panel.Controls.Add(field, 1, row);
        panel.Controls.Add(picker, 2, row);
    }

    private TableLayoutPanel Tab(string label)
    {
        var page = new TabPage(label) { BackColor = Color.White, AutoScroll = true, Padding = new Padding(12) };
        var fields = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 2 };
        fields.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
        fields.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        page.Controls.Add(fields);
        tabs.TabPages.Add(page);
        return fields;
    }

    private static Label Row(TableLayoutPanel fields, string label, Control control)
    {
        int row = fields.RowCount++;
        fields.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        control.AccessibleName = label;
        if (control is ComboBox || control is TextBox)
            control.Dock = DockStyle.Top;
        var caption = Caption(label);
        fields.Controls.Add(caption, 0, row);
        fields.Controls.Add(control, 1, row);
        return caption;
    }

    private static void Line(TableLayoutPanel fields, Control control)
    {
        int row = fields.RowCount++;
        fields.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        fields.Controls.Add(control, 0, row);
        fields.SetColumnSpan(control, 2);
    }

    private void BuildMovieTab()
    {
        var fields = Tab("Movie (H.264)");
        Line(fields, video);
        Row(fields, "Quality preset", movieQuality);
        Row(fields, "Rate control", rate);
        var amountLabel = Row(fields, "CRF", videoValue);
        Row(fields, "Encoder preset", preset);
        Row(fields, "Tune", tune);
        Row(fields, "Profile", profile);
        Row(fields, "Level", level);
        Row(fields, "Reference frames (0 = auto)", refs);
        Row(fields, "VBV max rate (kbps)", vbvRate);
        Row(fields, "VBV buffer (kbit)", vbvBuffer);
        Row(fields, "Reconstruction error limit", mse);
        Row(fields, "Advanced x264 options", extra);
        Line(fields, Caption("PSXRC uses closed groups of up to four frames. B-frames and lookahead are disabled."));
        tips.SetToolTip(mse, "Lower limits preserve more movies losslessly if the rebuilt PlayStation frame differs.");
        tips.SetToolTip(videoValue, "CRF or QP 0 requires the high444 profile. The PlayStation frame is still reconstructed.");
        tips.SetToolTip(extra, "Use colon-separated key=value pairs such as aq-mode=2:subme=7:trellis=2.");
        tips.SetToolTip(vbvRate, "Set both VBV values to zero to disable them. VBV applies to CRF or ABR.");

        rate.SelectedIndexChanged += (sender, e) =>
        {
            bool abr = rate.SelectedIndex == 2;
            videoValue.Maximum = abr ? 50000 : 51;
            videoValue.Minimum = abr ? 32 : 0;
            videoValue.Value = abr ? 650 : 20;
            amountLabel.Text = abr ? "Average bitrate (kbps)" : rate.SelectedIndex == 1 ? "QP" : "CRF";
            videoValue.AccessibleName = amountLabel.Text;
            SetCustom(movieQuality);
        };
        movieQuality.SelectedIndexChanged += (sender, e) =>
        {
            int selected = movieQuality.SelectedIndex;
            if (selected < 0 || selected >= 3)
                return;
            applyingPreset = true;
            rate.SelectedIndex = 0;
            videoValue.Value = new[] { 17, 20, 26 }[selected];
            mse.Value = new[] { 65, 103, 205 }[selected];
            applyingPreset = false;
        };
        videoValue.ValueChanged += (sender, e) => SetCustom(movieQuality);
        mse.ValueChanged += (sender, e) => SetCustom(movieQuality);
        video.CheckedChanged += (sender, e) =>
        {
            foreach (Control field in fields.Controls)
                if (field != video)
                    field.Enabled = video.Checked;
        };
    }

    private void BuildAudioTab()
    {
        var fields = Tab("Audio (Opus)");
        Line(fields, xa);
        Line(fields, cdda);
        Row(fields, "Quality preset", audioQuality);
        Row(fields, "Stereo bitrate (kbps)", audioBitrate);
        Row(fields, "Quality check ratio", audioRatio);
        Line(fields, Caption("Mono XA uses half the stereo bitrate. Higher quality check ratios are stricter."));
        audioQuality.SelectedIndexChanged += (sender, e) =>
        {
            int selected = audioQuality.SelectedIndex;
            if (selected < 0 || selected >= 3)
                return;
            applyingPreset = true;
            audioBitrate.Value = new[] { 160, 112, 64 }[selected];
            audioRatio.Value = new[] { 200, 128, 64 }[selected];
            applyingPreset = false;
        };
        audioBitrate.ValueChanged += (sender, e) => SetCustom(audioQuality);
        audioRatio.ValueChanged += (sender, e) => SetCustom(audioQuality);
    }

    private void SetCustom(ComboBox quality)
    {
        if (!applyingPreset)
            quality.SelectedIndex = 3;
    }

    private void PickInput()
    {
        using (var dialog = new OpenFileDialog { Filter = "Disc images (*.iso;*.chd;*.cue;*.bin)|*.iso;*.chd;*.cue;*.bin|All files (*.*)|*.*" })
        {
            if (dialog.ShowDialog(this) != DialogResult.OK)
                return;
            input.Text = dialog.FileName;
            if (String.IsNullOrWhiteSpace(output.Text))
                output.Text = Path.ChangeExtension(dialog.FileName, ".psxrc");
        }
    }

    private void PickOutput()
    {
        using (var dialog = new SaveFileDialog { Filter = "PSXRC images (*.psxrc)|*.psxrc", DefaultExt = "psxrc",
            AddExtension = true, OverwritePrompt = false, FileName = output.Text.Trim() })
            if (dialog.ShowDialog(this) == DialogResult.OK)
                output.Text = dialog.FileName;
    }

    private static string Quote(string arg)
    {
        var result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char ch in arg)
        {
            if (ch == '\\')
            {
                ++slashes;
                continue;
            }
            result.Append('\\', ch == '"' ? slashes * 2 + 1 : slashes).Append(ch);
            slashes = 0;
        }
        return result.Append('\\', slashes * 2).Append('"').ToString();
    }

    private List<string> Arguments()
    {
        if (String.IsNullOrWhiteSpace(input.Text) || String.IsNullOrWhiteSpace(output.Text))
            throw new ArgumentException("Choose an input disc and output file.");
        var inputPath = Path.GetFullPath(input.Text.Trim());
        var outputPath = Path.GetFullPath(output.Text.Trim());
        if (!File.Exists(inputPath) || !new[] { ".iso", ".chd", ".cue", ".bin" }
                .Contains(Path.GetExtension(inputPath), StringComparer.OrdinalIgnoreCase))
            throw new ArgumentException("Choose an existing ISO, CHD, CUE, or BIN image.");
        if (!String.Equals(Path.GetExtension(outputPath), ".psxrc", StringComparison.OrdinalIgnoreCase)
                || !Directory.Exists(Path.GetDirectoryName(outputPath)))
            throw new ArgumentException("Choose a .psxrc file in an existing folder.");
        if (File.Exists(outputPath) || Directory.Exists(outputPath))
            throw new ArgumentException("Choose a new output filename. Existing files are preserved.");
        if (video.Checked && ((vbvRate.Value == 0) != (vbvBuffer.Value == 0) ||
                (rate.SelectedIndex == 1 && vbvRate.Value != 0) ||
                (rate.SelectedIndex == 2 && vbvRate.Value > 0 && videoValue.Value > vbvRate.Value)))
            throw new ArgumentException("Check the VBV values. Supply both and use CRF or an average bitrate within the max rate.");

        var args = new List<string> { "convert", inputPath, outputPath, "--zstd-level", discLevel.Value.ToString() };
        if (video.Checked)
        {
            args.AddRange(new[] { new[] { "--video-crf", "--video-qp", "--video-kbps" }[rate.SelectedIndex],
                videoValue.Value.ToString(), "--h264-preset", preset.Text, "--h264-tune", tune.Text,
                "--h264-profile", profile.Text, "--h264-level", level.Text, "--video-refs", refs.Value.ToString(),
                "--video-vbv-rate", vbvRate.Value.ToString(), "--video-vbv-buffer", vbvBuffer.Value.ToString(),
                "--video-mse", mse.Value.ToString() });
            if (!String.IsNullOrWhiteSpace(extra.Text))
                args.AddRange(new[] { "--h264-extra", extra.Text.Trim() });
        }
        else
            args.Add("--no-video");
        if (!xa.Checked)
            args.Add("--no-xa");
        if (!cdda.Checked)
            args.Add("--no-cdda");
        if (xa.Checked || cdda.Checked)
            args.AddRange(new[] { "--audio-kbps", audioBitrate.Value.ToString(), "--audio-ratio", audioRatio.Value.ToString() });
        return args;
    }

    private async void ConvertDisc(object sender, EventArgs e)
    {
        try
        {
            var executable = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "psxrc.exe");
            if (!File.Exists(executable))
                throw new FileNotFoundException("Place psxrc.exe and its codec DLLs alongside the GUI.");
            var args = Arguments();
            log.Clear();
            status.Text = "Converting...";
            SetBusy(true);
            using (var process = new Process())
            {
                process.StartInfo = new ProcessStartInfo(executable, String.Join(" ", args.Select(Quote))) {
                    UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true,
                    RedirectStandardError = true, StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8
                };
                process.OutputDataReceived += AppendLog;
                process.ErrorDataReceived += AppendLog;
                if (!process.Start())
                    throw new InvalidOperationException("Could not start the converter.");
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                await Task.Run(() => process.WaitForExit());
                if (process.ExitCode == 0 && File.Exists(args[2]))
                    status.Text = String.Format("Verified output saved. {0:N1} MiB", new FileInfo(args[2]).Length / 1048576.0);
                else
                    status.Text = "Conversion failed. See the log.";
            }
        }
        catch (Exception error)
        {
            status.Text = error.Message;
            log.AppendText(error.Message + Environment.NewLine);
        }
        finally
        {
            SetBusy(false);
        }
    }

    private void AppendLog(object sender, DataReceivedEventArgs e)
    {
        if (e.Data != null)
            BeginInvoke(new Action(() => log.AppendText(e.Data + Environment.NewLine)));
    }

    private void SetBusy(bool value)
    {
        busy = value;
        input.Enabled = output.Enabled = browse.Enabled = save.Enabled = tabs.Enabled = run.Enabled = !value;
        progress.Visible = value;
        progress.MarqueeAnimationSpeed = value ? 35 : 0;
    }
}
