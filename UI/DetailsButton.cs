namespace TaskManager.UI;

// The circled-chevron "Fewer details" / "More details" button from the Windows 10 Task Manager footer.
internal sealed class DetailsButton : Button
{
    private bool hot;

    public DetailsButton()
    {
        Text = "Fewer details";
        FlatStyle = FlatStyle.Flat;
        FlatAppearance.BorderSize = 0;
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
    }

    public override Size GetPreferredSize(Size proposedSize) =>
        new(TextRenderer.MeasureText(Text, Font).Width + Scale(32), Scale(28));

    private int Scale(int value) => (int)Math.Round(value * DeviceDpi / 96d);

    protected override void OnMouseEnter(EventArgs args) { hot = true; Invalidate(); base.OnMouseEnter(args); }
    protected override void OnMouseLeave(EventArgs args) { hot = false; Invalidate(); base.OnMouseLeave(args); }

    protected override void OnPaint(PaintEventArgs args)
    {
        var graphics = args.Graphics;
        graphics.Clear(Parent?.BackColor ?? Color.White);
        var scale = DeviceDpi / 96f;
        var center = new PointF(11 * scale, Height / 2f);
        graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        using (var fill = new SolidBrush(hot ? Color.FromArgb(229, 243, 255) : Color.White))
            graphics.FillEllipse(fill, center.X - 9 * scale, center.Y - 9 * scale, 18 * scale, 18 * scale);
        using var pen = new Pen(hot ? Color.FromArgb(0, 120, 215) : Color.FromArgb(135, 135, 135), scale);
        graphics.DrawEllipse(pen, center.X - 9 * scale, center.Y - 9 * scale, 18 * scale, 18 * scale);
        var direction = Text.StartsWith("More", StringComparison.Ordinal) ? 1 : -1;
        PointF[] points = [new(center.X - 4 * scale, center.Y - direction * 2 * scale),
            new(center.X, center.Y + direction * 2 * scale), new(center.X + 4 * scale, center.Y - direction * 2 * scale)];
        graphics.DrawLines(pen, points);
        graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.Default;
        TextRenderer.DrawText(graphics, Text, Font, new Rectangle((int)(27 * scale), 0, Width - (int)(27 * scale), Height), Color.Black,
            TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.SingleLine);
        if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(graphics, new Rectangle((int)(24 * scale), 3, Width - (int)(24 * scale), Height - 6));
    }
}
