"""The launcher's palette and drawing helpers/widgets (moved from uncharted_launcher.pyw)."""
import ctypes
import tkinter as tk
from tkinter import font as tkfont

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


# ---------------------------------------------------------------------------------------------
# Messages: Dialog (modal, replaces the native message box) and Toast (a short acknowledgement)

def work_area(widget):
    """The primary screen without the taskbar (x, y, width, height), in Tk's pixels."""
    try:
        rect = (ctypes.c_long * 4)()
        if ctypes.windll.user32.SystemParametersInfoW(0x30, 0, rect, 0):  # SPI_GETWORKAREA
            return rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]
    except (AttributeError, OSError):
        pass
    return 0, 0, widget.winfo_screenwidth(), widget.winfo_screenheight()


def dark_title_bar(window):
    """Windows 10/11: the window's title bar in the dark theme (others: nothing happens)."""
    try:
        user32 = ctypes.windll.user32
        user32.GetAncestor.restype = ctypes.c_void_p
        hwnd = user32.GetAncestor(ctypes.c_void_p(window.winfo_id()), 2)  # GA_ROOT
        ctypes.windll.dwmapi.DwmSetWindowAttribute(ctypes.c_void_p(hwnd), 20, ctypes.byref(ctypes.c_int(1)), 4)
    except (AttributeError, OSError, ctypes.ArgumentError, tk.TclError):
        pass


def copy_text(root, text):
    """Text on the clipboard. Windows' own clipboard first: Tk renders its clipboard only on demand,
    so the text was lost when the launcher closed right after (--play direct mode)."""
    try:
        kernel32, user32 = ctypes.windll.kernel32, ctypes.windll.user32
        kernel32.GlobalAlloc.restype = kernel32.GlobalLock.restype = ctypes.c_void_p
        kernel32.GlobalLock.argtypes = kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
        user32.OpenClipboard.argtypes = [ctypes.c_void_p]
        user32.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
        data = (text.replace("\r\n", "\n").replace("\n", "\r\n") + "\0").encode("utf-16-le")
        handle = kernel32.GlobalAlloc(0x2, len(data))  # GMEM_MOVEABLE
        ctypes.memmove(kernel32.GlobalLock(handle), data, len(data))
        kernel32.GlobalUnlock(handle)
        if user32.OpenClipboard(None):  # 24 Oct 2026: owned by the Tk window, the text was gone once it was destroyed
            try:
                user32.EmptyClipboard()
                if user32.SetClipboardData(13, handle):  # CF_UNICODETEXT; Windows owns the memory now
                    return
            finally:
                user32.CloseClipboard()
    except (AttributeError, OSError, ctypes.ArgumentError):
        pass
    root.clipboard_clear()
    root.clipboard_append(text)


class DialogButton(tk.Label):
    STYLES = {"primary": (ACCENT, ACCENT_HI, INK), "secondary": (LINE, HOVER_LINE, TEXT),
              "danger": (STOP_RED, "#e46662", "#ffffff")}

    def __init__(self, parent, text, command, style, font, scale):
        bg, self.hover, fg = self.STYLES[style]
        super().__init__(parent, text=text, font=font, bg=bg, fg=fg, cursor="hand2",
                         padx=int(16 * scale), pady=int(7 * scale))
        self.normal, self.command = bg, command
        self.bind("<Button-1>", lambda e: self.invoke())
        self.bind("<Enter>", lambda e: self.config(bg=self.hover))
        self.bind("<Leave>", lambda e: self.config(bg=self.normal))

    def invoke(self):
        self.command()


class Dialog(tk.Toplevel):
    """A modal message in the launcher's colors. Dialog.ask(...) blocks and returns the value of the
    clicked button, or `cancel` for Escape and the close button. Enter clicks the default button.
    `detail` is a read-only monospace box (scrolls when long) with a Copy button."""
    ICONS = {"info": ("ℹ", HERO_LIGHT), "warning": ("⚠", ACCENT), "error": ("✕", STOP_RED),
             "question": ("?", ACCENT)}
    WIDTH = 460

    def __init__(self, root, title, message, buttons, detail=None, default=0, cancel=None, scale=1.0, icon=None):
        super().__init__(root, bg=PANEL, highlightthickness=1, highlightbackground=LINE)
        self.withdraw()
        self.root, self.result, self.buttons = root, cancel, []
        px = lambda v: int(v * scale)  # noqa: E731
        width, pad = px(self.WIDTH), px(20)
        self.title(title)
        self.resizable(False, False)
        body_font, bold_font = ("Segoe UI", 10), ("Segoe UI Semibold", 11)
        tk.Frame(self, width=width, height=0, bg=PANEL).pack()  # the dialog's width
        header = tk.Frame(self, bg=PANEL_HI)
        header.pack(fill="x")
        if icon in self.ICONS:
            glyph, color = self.ICONS[icon]
            tk.Label(header, text=glyph, font=("Segoe UI Symbol", 15), bg=PANEL_HI, fg=color).pack(
                side="left", padx=(pad, px(10)), pady=px(12))
        tk.Label(header, text=title, font=bold_font, bg=PANEL_HI, fg=TEXT, anchor="w", justify="left",
                 wraplength=width - 2 * pad - px(40)).pack(side="left", fill="x", expand=True,
                                                          padx=(0 if icon in self.ICONS else pad, pad))
        tk.Label(self, text=message, font=body_font, bg=PANEL, fg=TEXT, anchor="w", justify="left",
                 wraplength=width - 2 * pad).pack(fill="x", padx=pad, pady=(pad, 0))
        if detail:
            self.detail = detail
            mono = tkfont.Font(root=root, family="Consolas", size=9)
            chars = max(20, (width - 2 * pad - px(40)) // max(1, mono.measure("0")))
            lines = sum(max(1, -(-len(line) // chars)) for line in detail.splitlines() or [""])
            frame = tk.Frame(self, bg=LINE, padx=1, pady=1)
            frame.pack(fill="x", padx=pad, pady=(px(14), 0))
            box = tk.Text(frame, font=mono, bg=BG, fg=MUTED, relief="flat", bd=0, width=1, height=min(9, lines),
                          wrap="word", padx=px(8), pady=px(6), selectbackground=ACCENT, selectforeground=INK,
                          highlightthickness=0, cursor="arrow")
            bar = ScrollBar(frame, scale, box)
            bar.config(height=1)  # a Canvas asks for ~265 px otherwise, stretching the box
            box.configure(yscrollcommand=bar.set)
            bar.pack(side="right", fill="y")
            box.pack(side="left", fill="x", expand=True)
            box.insert("1.0", detail)
            box.config(state="disabled")
        row = tk.Frame(self, bg=PANEL)
        row.pack(fill="x", padx=pad, pady=pad)
        if detail:
            self.copy_button = DialogButton(row, "Copy", self.copy_detail, "secondary", body_font, scale)
            self.copy_button.pack(side="left")
        for label, value, *style in reversed(buttons):
            button = DialogButton(row, label, lambda v=value: self.finish(v), style[0] if style else "secondary",
                                  body_font, scale)
            button.pack(side="right", padx=(px(8), 0))
            self.buttons.insert(0, button)
        self.default_button = self.buttons[default] if buttons else None
        self.bind("<Return>", lambda e: self.default_button and self.default_button.invoke())
        self.bind("<Escape>", lambda e: self.finish(cancel))
        self.protocol("WM_DELETE_WINDOW", lambda: self.finish(cancel))

    def copy_detail(self):
        copy_text(self.root, self.detail)
        self.copy_button.config(text="Copied")
        self.after(1500, lambda: self.winfo_exists() and self.copy_button.config(text="Copy"))

    def finish(self, value):
        self.result = value
        self.destroy()

    def grab(self, retry=True):
        try:
            self.grab_set()
        except tk.TclError:  # not on screen yet (grab_set needs a viewable window)
            if retry:
                self.after(50, lambda: self.winfo_exists() and self.grab(False))

    def show(self):
        root = self.root
        viewable = bool(root.winfo_viewable())
        dark_title_bar(self)
        self.update_idletasks()
        w, h = self.winfo_reqwidth(), self.winfo_reqheight()
        if viewable:  # over the launcher
            x = root.winfo_rootx() + (root.winfo_width() - w) // 2
            y = root.winfo_rooty() + (root.winfo_height() - h) // 2
            self.transient(root)
        else:  # --play direct mode (the root is withdrawn): a window of its own, in front
            left, top, area_w, area_h = work_area(self)
            x, y = left + (area_w - w) // 2, top + (area_h - h) // 2
            self.attributes("-topmost", True)
            root.update()
        self.geometry(f"{w}x{h}+{max(0, x)}+{max(0, y)}")
        self.deiconify()
        self.update()  # mapped now: the grab below needs a viewable window (wait_visibility could hang)
        self.grab()
        self.focus_force()
        try:
            self.wait_window()
        except tk.TclError:  # the root was destroyed under the dialog
            pass
        return self.result

    @classmethod
    def ask(cls, root, title, message, buttons, detail=None, default=0, cancel=None, scale=1.0, icon=None):
        return cls(root, title, message, buttons, detail, default, cancel, scale, icon).show()

    @classmethod
    def info(cls, root, title, message, detail=None, scale=1.0):
        cls.ask(root, title, message, [("OK", True, "primary")], detail, scale=scale, icon="info")

    @classmethod
    def warning(cls, root, title, message, detail=None, scale=1.0):
        cls.ask(root, title, message, [("OK", True, "primary")], detail, scale=scale, icon="warning")

    @classmethod
    def error(cls, root, title, message, detail=None, scale=1.0):
        cls.ask(root, title, message, [("OK", True, "primary")], detail, scale=scale, icon="error")

    @classmethod
    def yesno(cls, root, title, message, detail=None, scale=1.0):
        return cls.ask(root, title, message, [("Yes", True, "primary"), ("No", False)], detail, cancel=False,
                       scale=scale, icon="question")

    @classmethod
    def okcancel(cls, root, title, message, detail=None, scale=1.0):
        return cls.ask(root, title, message, [("OK", True, "primary"), ("Cancel", False)], detail, cancel=False,
                       scale=scale, icon="question")


class Toast(tk.Frame):
    """A short acknowledgement in the root's bottom-right corner (not a window): up to three stacked,
    newest on top, gone after DELAY ms or on a click. Does nothing while the root is withdrawn."""
    DELAY = 4000
    KINDS = {"info": ACCENT, "ok": GOOD, "error": STOP_RED}
    MAX = 3

    def __init__(self, root, text, kind, scale):
        super().__init__(root, bg=PANEL_HI, highlightthickness=1, highlightbackground=LINE)
        self.scale = scale
        tk.Frame(self, bg=self.KINDS.get(kind, ACCENT), width=max(3, int(4 * scale))).pack(side="left", fill="y")
        tk.Label(self, text=text, font=("Segoe UI", 10), bg=PANEL_HI, fg=TEXT, justify="left", anchor="w",
                 wraplength=int(340 * scale), padx=int(12 * scale), pady=int(10 * scale)).pack(side="left")
        for widget in (self, *self.winfo_children()):
            widget.bind("<Button-1>", lambda e: self.hide())
            widget.config(cursor="hand2")
        self.timer = self.after(self.DELAY, self.hide)

    @classmethod
    def stack(cls, root):
        return [t for t in getattr(root, "_toasts", []) if t.winfo_exists()]

    @classmethod
    def show(cls, root, text, kind="info", scale=1.0):
        if not root.winfo_viewable():
            return None
        toast = cls(root, text, kind, scale)
        stack = cls.stack(root) + [toast]
        root._toasts = stack
        for old in stack[:-cls.MAX]:
            old.hide()
        cls.layout(root)
        return toast

    @classmethod
    def layout(cls, root):
        stack = cls.stack(root)
        below = 0  # the oldest sits lowest; each newer one above it
        for toast in stack:
            scale = toast.scale
            toast.update_idletasks()
            toast.place(relx=1.0, rely=1.0, anchor="se", x=-int(24 * scale), y=-(int(100 * scale) + below))
            toast.lift()
            below += toast.winfo_reqheight() + int(8 * scale)

    def hide(self):
        try:
            root = self.master
            self.after_cancel(self.timer)
            self.destroy()
            root._toasts = [t for t in getattr(root, "_toasts", []) if t is not self]
            self.layout(root)
        except tk.TclError:  # the root is gone
            pass
