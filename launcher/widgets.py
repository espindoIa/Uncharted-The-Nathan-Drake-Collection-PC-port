"""The launcher's palette and drawing helpers/widgets (moved from uncharted_launcher.pyw)."""
import tkinter as tk

# Palette (the fork's own, "expedition at night"): night blue and slate panels, a signal-flare
# orange for what is selected or pressed. tools/make_launcher_icon.py draws the icon in the same
# colours.
BG = "#0b1116"
PANEL = "#111b22"
PANEL_HI = "#18262f"
LINE = "#22343f"
HOVER_LINE = "#3a5562"
TEXT = "#e6eeec"
MUTED = "#8ea3a6"
DIM = "#566b70"
ACCENT = "#ff8a3d"
ACCENT_HI = "#ffa45f"
INK = "#1c0d02"
STOP_RED = "#d9534f"
GOOD = "#5fbf8f"  # a check that passed (Home's Ready to play list)
# The hero's art in two tones (shadows, highlights) and its map grid.
HERO_DARK = "#071016"
HERO_LIGHT = "#6f9ea2"
HERO_GRID = "#1d3440"

# ---------------------------------------------------------------------------------------------
# Drawing helpers and widgets

def round_rect(canvas, x1, y1, x2, y2, r, **kw):
    points = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2, x2 - r, y2,
              x1 + r, y2, x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
    return canvas.create_polygon(points, smooth=True, **kw)


class Toggle(tk.Canvas):
    def __init__(self, parent, scale, value, command):
        self.s = scale
        w, h = int(46 * scale), int(24 * scale)
        super().__init__(parent, width=w, height=h, bg=parent["bg"], highlightthickness=0, cursor="hand2")
        self.w, self.h, self.value, self.command = w, h, bool(value), command
        self.bind("<Button-1>", lambda e: self.set(not self.value, notify=True))
        self.draw()

    def set(self, value, notify=False):
        self.value = bool(value)
        self.draw()
        if notify:
            self.command(self.value)

    def draw(self):
        self.delete("all")
        pad = max(1, int(2 * self.s))
        track = ACCENT if self.value else LINE
        round_rect(self, 1, 1, self.w - 1, self.h - 1, self.h // 2, fill=track, outline=track)
        d = self.h - 2 * pad - 2
        x = self.w - pad - d - 1 if self.value else pad + 1
        self.create_oval(x, pad + 1, x + d, pad + 1 + d, fill=INK if self.value else MUTED, outline="")


class Segmented(tk.Frame):
    """A row of choices; the selected one is gold."""

    def __init__(self, parent, options, value, command, font, recommended=None):
        super().__init__(parent, bg=LINE, padx=1, pady=1)
        self.command, self.labels, self.value, self.recommended = command, {}, value, recommended
        for i, (option, label) in enumerate(options):
            item = tk.Label(self, text=self.text(option, label), font=font, padx=12, pady=5, cursor="hand2", bd=0)
            item.grid(row=0, column=i, padx=(0 if i == 0 else 1, 0))
            item.bind("<Button-1>", lambda e, v=option: self.set(v, notify=True))
            item.bind("<Enter>", lambda e, w=item, v=option: v != self.value and w.config(bg=PANEL_HI))
            item.bind("<Leave>", lambda e: self.restyle())
            self.labels[option] = item
        self.restyle()

    def text(self, option, label):
        # The recommended choice carries a star.
        return f"{label}  ★" if option == self.recommended else label

    def relabel(self, options):
        """New labels for the same choices (the resolutions at another aspect ratio)."""
        for option, label in options:
            if option in self.labels:
                self.labels[option].config(text=self.text(option, label))

    def set(self, value, notify=False):
        self.value = value
        self.restyle()
        if notify:
            self.command(value)

    def restyle(self):
        for option, item in self.labels.items():
            selected = option == self.value
            item.config(bg=ACCENT if selected else PANEL, fg=INK if selected else TEXT)


class Dropdown(tk.Label):
    """A choice from a longer list: the current one, and a menu of all on click."""

    def __init__(self, parent, options, value, command, font):
        super().__init__(parent, font=font, bg=PANEL, fg=TEXT, padx=12, pady=5, cursor="hand2", anchor="w",
                         highlightthickness=1, highlightbackground=LINE)
        self.options, self.command, self.font = options, command, font
        self.set(value)
        self.bind("<Button-1>", lambda e: self.open())
        self.bind("<Enter>", lambda e: self.config(bg=PANEL_HI))
        self.bind("<Leave>", lambda e: self.config(bg=PANEL))

    def set(self, value, notify=False):
        self.value = value
        label = next((label for option, label in self.options if option == value), value)
        self.config(text=f"{label}   \u25be")
        if notify:
            self.command(value)

    def open(self):
        menu = tk.Menu(self, tearoff=0, bg=PANEL, fg=TEXT, activebackground=ACCENT, activeforeground=INK,
                       font=self.font, bd=0)
        for option, label in self.options:
            menu.add_command(label=("\u2713  " if option == self.value else "     ") + label,
                             command=lambda v=option: self.set(v, notify=True))
        menu.tk_popup(self.winfo_rootx(), self.winfo_rooty() + self.winfo_height())


class ScrollBar(tk.Canvas):
    """A thin scrollbar in the launcher's colors for a scrolling canvas (its yscrollcommand)."""

    def __init__(self, parent, scale, target):
        super().__init__(parent, width=max(6, int(10 * scale)), bg=parent["bg"], highlightthickness=0)
        self.target, self.first, self.last, self.hover, self.grab = target, 0.0, 1.0, False, None
        self.bind("<Configure>", lambda e: self.draw())
        self.bind("<Enter>", lambda e: self.highlight(True))
        self.bind("<Leave>", lambda e: self.highlight(False))
        self.bind("<Button-1>", self.press)
        self.bind("<B1-Motion>", self.drag)
        self.bind("<ButtonRelease-1>", self.release)

    def set(self, first, last):
        self.first, self.last = float(first), float(last)
        self.draw()

    def highlight(self, hover):
        self.hover = hover
        self.draw()

    def draw(self):
        self.delete("all")
        w, h = self.winfo_width(), self.winfo_height()
        if h <= 1 or self.last - self.first >= 1:
            return
        x1, x2, r = 2, w - 2, max(1, (w - 4) // 2)
        round_rect(self, x1, 1, x2, h - 1, r, fill=PANEL, outline=PANEL)
        top = min(int(self.first * h), h - 24)
        bottom = min(h, max(int(self.last * h), top + 24))
        color = MUTED if self.hover or self.grab is not None else DIM
        round_rect(self, x1, top + 1, x2, bottom - 1, r, fill=color, outline=color)

    def press(self, event):
        position = event.y / max(1, self.winfo_height())
        if self.first <= position <= self.last:
            self.grab = position - self.first
        else:  # on the track: jump there, then drag from the thumb's middle
            self.grab = (self.last - self.first) / 2
            self.target.yview_moveto(position - self.grab)

    def drag(self, event):
        if self.grab is not None:
            self.target.yview_moveto(event.y / max(1, self.winfo_height()) - self.grab)

    def release(self, event):
        self.grab = None
        self.draw()
