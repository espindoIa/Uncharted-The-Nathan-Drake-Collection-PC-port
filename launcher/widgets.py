"""The launcher's palette and drawing helpers/widgets (moved from uncharted_launcher.pyw)."""
import ctypes
import tkinter as tk
from tkinter import font as tkfont

import art  # launcher/art.py: anti-aliased shapes and icons (25 Oct 2026)

# Palette ("cinematic", 25 Oct 2026 redesign): near-black glass over the Collection's art, and each
# game's own colour (GAME_ACCENTS in the launcher, set_accent) for what is selected or pressed.
# tools/make_launcher_icon.py builds the icon from a master drawn in the same colours.
STAGE = "#07090c"  # the window behind the panels
BG = "#0e1318"  # the section panel
PANEL = "#151c23"
PANEL_HI = "#1e2730"
LINE = "#26313b"
HOVER_LINE = "#3a4855"
TEXT = "#eef3f6"
MUTED = "#8b9aa6"
DIM = "#5b6873"
ACCENT = "#e6b44c"
ACCENT_HI = "#ebc370"
INK = "#160b04"
STOP_RED = "#d9534f"
GOOD = "#6fcf97"  # a check that passed (Home's Ready to play list)
INFO = "#7fb6d9"


def set_accent(root, color):
    """The selected game's colour becomes the accent: widgets read ACCENT when they draw, and every
    Tk option still holding the old accent (a selected label, a focus ring) takes the new one."""
    global ACCENT, ACCENT_HI
    if color == ACCENT:
        return
    swap = {ACCENT: color, ACCENT_HI: art.mix(color, "#ffffff", 0.2)}
    ACCENT, ACCENT_HI = swap[ACCENT], swap[ACCENT_HI]
    stack = [root]
    while stack:
        widget = stack.pop()
        for option in ("bg", "fg", "highlightcolor", "highlightbackground", "activebackground", "selectbackground"):
            try:
                value = str(widget.cget(option))
            except tk.TclError:
                continue
            if value in swap:
                widget.configure(**{option: swap[value]})
        stack.extend(widget.winfo_children())

# ---------------------------------------------------------------------------------------------
# Drawing helpers and widgets

def scale_of(widget):
    return max(1.0, widget.winfo_fpixels("1i") / 96.0)


def draw_image(canvas, x, y, image, **kw):
    """An image at canvas (x, y), anchored at its top left by default, past the focus ring's border
    (a Tk canvas' coordinates start under its highlight border)."""
    edge = int(canvas.cget("highlightthickness")) + int(canvas.cget("bd"))
    return canvas.create_image(x + edge, y + edge, image=image, anchor=kw.pop("anchor", "nw"), **kw)


def on_keys(widget, handlers):
    """Keys for a widget that takes the keyboard focus ({"<Left>": handler}). Ctrl+key is left to
    the window's shortcuts (Ctrl+Enter plays); a handled key stops there ("break")."""
    def bind(sequence, handler):
        def run(event):
            if not event.state & 0x4:
                handler()
                return "break"
        widget.bind(sequence, run)
    for sequence, handler in handlers.items():
        bind(sequence, handler)


def on_activate(widget, action):
    """Space, Enter or the numpad Enter on the focused widget runs `action`."""
    on_keys(widget, {sequence: action for sequence in ("<space>", "<Return>", "<KP_Enter>")})


class FocusDrawn:
    """A widget that draws its own focus ring (a rounded outline, 25 Oct 2026) instead of Tk's square
    highlight border: draw() runs again when the focus comes or goes."""

    def watch_focus(self):
        self.bind("<FocusIn>", lambda e: self.draw(), add="+")
        self.bind("<FocusOut>", lambda e: self.draw(), add="+")

    def focused(self):
        try:
            return self.focus_get() is self
        except (KeyError, tk.TclError):  # a menu's popup has the focus
            return False


class Toggle(FocusDrawn, tk.Canvas):
    def __init__(self, parent, scale, value, command):
        self.s = scale
        w, h = int(46 * scale), int(24 * scale)
        # 24 Oct 2026: keyboard focus (Tab, Space/Enter); 25 Oct: the ring is drawn round the track.
        super().__init__(parent, width=w, height=h, bg=parent["bg"], highlightthickness=0, bd=0, takefocus=1,
                         cursor="hand2")
        self.w, self.h, self.value, self.command, self.enabled = w, h, bool(value), command, True
        self.bind("<Button-1>", lambda e: self.click())
        on_activate(self, self.click)
        self.watch_focus()
        self.draw()

    def click(self):
        if self.enabled:
            self.set(not self.value, notify=True)

    def set(self, value, notify=False):
        self.value = bool(value)
        self.draw()
        if notify:
            self.command(self.value)

    def set_enabled(self, enabled):
        """Greyed out (a setting that does nothing now): shows its value, ignores clicks."""
        self.enabled = bool(enabled)
        self.config(cursor="hand2" if self.enabled else "arrow", takefocus=int(self.enabled))
        self.draw()

    def draw(self):
        self.delete("all")
        on, focus = self.value, self.focused()
        track = (ACCENT if on else PANEL_HI) if self.enabled else (LINE if on else PANEL)
        ring, width = (TEXT, 2 * self.s) if focus else (None if on else LINE, self.s)
        draw_image(self, 0, 0, art.rounded(self, self.w, self.h, self.h / 2, track, ring, width))
        pad = max(2, int(3 * self.s))
        d = self.h - 2 * pad
        knob = (INK if on else MUTED) if self.enabled else (MUTED if on else DIM)
        x = self.w - pad - d if on else pad
        draw_image(self, x, pad, art.photo(self, ("dot", d, knob), lambda: art.circle_image(d, knob)))


class Chip(FocusDrawn, tk.Canvas):
    """A small on/off pill, for the performance counter's items (Display section)."""

    def __init__(self, parent, scale, font, label, value, command):
        self.s, self.label, self.command = scale, label, command
        self.value = bool(value)
        self.font = tkfont.Font(font=font)
        pad_x, pad_y = int(12 * scale), int(5 * scale)
        w = self.font.measure(label) + 2 * pad_x
        h = self.font.metrics("linespace") + 2 * pad_y
        # Keyboard focus as Toggle (Tab, Space/Enter).
        super().__init__(parent, width=w, height=h, bg=parent["bg"], highlightthickness=0, bd=0, takefocus=1,
                         cursor="hand2")
        self.w, self.h = w, h
        self.bind("<Button-1>", lambda e: self.set(not self.value, notify=True))
        on_activate(self, lambda: self.set(not self.value, notify=True))
        self.watch_focus()
        self.draw()

    def set(self, value, notify=False):
        self.value = bool(value)
        self.draw()
        if notify:
            self.command(self.value)

    def draw(self):
        self.delete("all")
        ring = TEXT if self.focused() else (ACCENT if self.value else LINE)
        draw_image(self, 0, 0, art.rounded(self, self.w, self.h, self.h / 2, ACCENT if self.value else PANEL, ring,
                                           2 * self.s if self.focused() else self.s))
        self.create_text(self.w / 2, self.h / 2, text=self.label, font=self.font, fill=INK if self.value else MUTED)


class Segmented(tk.Canvas):
    """A row of choices in a rounded tray; the selected one is a pill in the game's colour. One Tab
    stop: Left/Right change the choice, Home/End pick the first/last. The choices are Labels (one
    per option, self.labels) placed on the tray, each with its pill as an image behind the text."""

    def __init__(self, parent, options, value, command, font, recommended=None):
        super().__init__(parent, bg=parent["bg"], highlightthickness=0, bd=0, takefocus=1)
        self.command, self.labels, self.value, self.recommended = command, {}, value, recommended
        self.enabled, self.hover = True, None
        self.font = tkfont.Font(font=font)
        self.s = scale_of(self)
        on_keys(self, {"<Left>": lambda: self.move(-1), "<Right>": lambda: self.move(1),
                       "<Home>": lambda: self.move(to=0), "<End>": lambda: self.move(to=-1)})
        for option, label in options:
            item = tk.Label(self, text=self.text(option, label), font=font, bd=0, padx=0, pady=0, compound="center",
                            cursor="hand2", bg=PANEL)
            item.bind("<Button-1>", lambda e, v=option: self.click(v))
            item.bind("<Enter>", lambda e, v=option: self.set_hover(v))
            item.bind("<Leave>", lambda e: self.set_hover(None))
            self.labels[option] = item
        self.bind("<FocusIn>", lambda e: self.draw_tray())
        self.bind("<FocusOut>", lambda e: self.draw_tray())
        self.layout()

    def text(self, option, label):
        # The recommended choice carries a star.
        return f"{label}  ★" if option == self.recommended else label

    def layout(self):
        """Sizes the tray to the labels and places them on it."""
        inset, pad_x = max(2, int(3 * self.s)), int(13 * self.s)
        self.item_h = self.font.metrics("linespace") + int(10 * self.s)
        self.widths = {o: self.font.measure(item.cget("text")) + 2 * pad_x for o, item in self.labels.items()}
        self.tray_w = sum(self.widths.values()) + 2 * inset
        self.tray_h = self.item_h + 2 * inset
        self.config(width=self.tray_w, height=self.tray_h)
        self.delete("all")
        self.draw_tray()
        x = inset
        for option, item in self.labels.items():
            self.create_window(x, inset, window=item, anchor="nw")
            x += self.widths[option]
        self.restyle()

    def draw_tray(self):
        self.delete("tray")
        focus = self.focus_get() is self if self.winfo_exists() else False
        image = art.rounded(self, self.tray_w, self.tray_h, self.tray_h / 2, PANEL, TEXT if focus else LINE,
                            2 * self.s if focus else self.s)
        self.create_image(0, 0, image=image, anchor="nw", tags="tray")
        self.tag_lower("tray")

    def relabel(self, options):
        """New labels for the same choices (the resolutions at another aspect ratio)."""
        for option, label in options:
            if option in self.labels:
                self.labels[option].config(text=self.text(option, label))
        self.layout()

    def set(self, value, notify=False):
        self.value = value
        self.restyle()
        if notify:
            self.command(value)

    def set_hover(self, option):
        self.hover = option if self.enabled else None
        self.restyle()

    def click(self, option):
        if self.enabled:
            self.set(option, notify=True)

    def move(self, by=0, to=None):
        """The keyboard: `by` choices on from the selected one, or the choice at index `to`."""
        options = list(self.labels)
        if to is None:
            current = options.index(self.value) if self.value in options else -1
            to = min(max(current + by, 0), len(options) - 1)
        if options[to] != self.value:  # Right on the last choice changes nothing
            self.click(options[to])

    def set_enabled(self, enabled):
        """Greyed out (a setting that does nothing now): shows its value, ignores clicks."""
        self.enabled = bool(enabled)
        self.config(takefocus=int(self.enabled))
        self.restyle()

    def restyle(self):
        for option, item in self.labels.items():
            selected, w = option == self.value, self.widths[option]
            if self.enabled:
                fill = ACCENT if selected else (PANEL_HI if option == self.hover else None)
                fg = INK if selected else TEXT
            else:  # the selected one stays readable, but is not in the game's colour
                fill, fg = (LINE if selected else None), (MUTED if selected else DIM)
            pill = art.rounded(self, w, self.item_h, self.item_h / 2, fill)
            item.config(image=pill, fg=fg, cursor="hand2" if self.enabled else "arrow")


class Dropdown(tk.Label):
    """A choice from a longer list: the current one on a rounded field, and a menu of all on click."""

    def __init__(self, parent, options, value, command, font):
        super().__init__(parent, font=font, bg=parent["bg"], fg=TEXT, bd=0, padx=0, pady=0, compound="center",
                         cursor="hand2", highlightthickness=0, takefocus=1)
        self.options, self.command, self.font, self.enabled, self.hover = options, command, font, True, False
        self.measure, self.s = tkfont.Font(font=font), scale_of(self)
        self.set(value)
        self.bind("<Button-1>", lambda e: self.open())
        on_keys(self, {sequence: self.open for sequence in ("<space>", "<Return>", "<KP_Enter>", "<Down>")})
        on_keys(self, {"<Left>": lambda: self.move(-1), "<Right>": lambda: self.move(1)})
        self.bind("<Enter>", lambda e: self.set_hover(True))
        self.bind("<Leave>", lambda e: self.set_hover(False))
        self.bind("<FocusIn>", lambda e: self.draw(), add="+")
        self.bind("<FocusOut>", lambda e: self.draw(), add="+")

    def set_hover(self, hover):
        self.hover = hover and self.enabled
        self.draw()

    def draw(self):
        """The field behind the text: as wide as the text, at least 140 px."""
        try:
            focus = self.focus_get() is self
        except (KeyError, tk.TclError):
            focus = False
        w = max(int(140 * self.s), self.measure.measure(self.cget("text")) + int(28 * self.s))
        h = self.measure.metrics("linespace") + int(14 * self.s)
        field = art.rounded(self, w, h, int(9 * self.s), PANEL_HI if self.hover else PANEL, TEXT if focus else LINE,
                            2 * self.s if focus else self.s)
        self.config(image=field)

    def set_enabled(self, enabled):
        """Greyed out (a setting that does nothing now): shows its value, does not open."""
        self.enabled = bool(enabled)
        self.config(fg=TEXT if self.enabled else DIM, cursor="hand2" if self.enabled else "arrow",
                    takefocus=int(self.enabled))
        self.draw()

    def set(self, value, notify=False):
        self.value = value
        label = next((label for option, label in self.options if option == value), value)
        self.config(text=f"{label}   ▾")
        self.draw()
        if notify:
            self.command(value)

    def move(self, by):
        """The keyboard and the controller: `by` options on from the current one, clamped at the ends."""
        if not self.enabled:
            return
        values = [option for option, _ in self.options]
        current = values.index(self.value) if self.value in values else 0
        new = values[min(max(current + by, 0), len(values) - 1)]
        if new != self.value:
            self.set(new, notify=True)

    def open(self):
        if not self.enabled:
            return
        menu = tk.Menu(self, tearoff=0, bg=PANEL, fg=TEXT, activebackground=ACCENT, activeforeground=INK,
                       font=self.font, bd=0)
        for option, label in self.options:
            menu.add_command(label=("✓  " if option == self.value else "     ") + label,
                             command=lambda v=option: self.set(v, notify=True))
        menu.tk_popup(self.winfo_rootx(), self.winfo_rooty() + self.winfo_height())


class ScrollBar(tk.Canvas):
    """A thin scrollbar in the launcher's colors for a scrolling canvas (its yscrollcommand)."""

    def __init__(self, parent, scale, target):
        super().__init__(parent, width=max(6, int(8 * scale)), bg=parent["bg"], highlightthickness=0)
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
        least = 3 * w
        top = min(int(self.first * h), h - least)
        bottom = min(h, max(int(self.last * h), top + least))
        color = MUTED if self.hover or self.grab is not None else DIM
        self.create_image(0, top, image=art.rounded(self, w, bottom - top, w / 2, color), anchor="nw")

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


class Badge(tk.Canvas):
    """A small label with a rounded border ("ALL GAMES" next to a setting that all three games share)."""

    def __init__(self, parent, text, scale, font):
        pad = int(8 * scale)
        measure = tkfont.Font(root=parent, font=font)
        w, h = measure.measure(text) + 2 * pad, measure.metrics("linespace") + int(4 * scale)
        super().__init__(parent, width=w, height=h, bg=parent["bg"], highlightthickness=0)
        self.create_image(0, 0, image=art.rounded(self, w, h, h / 2, None, HOVER_LINE, scale), anchor="nw")
        self.create_text(w // 2, h // 2, text=text, font=font, fill=MUTED)
        self.text = text


class InfoTip(tk.Canvas):
    """A round "?" that explains a setting: the full text opens in a popover (a frame placed inside the
    window, like Toast) on hover after DELAY ms, on a click (stays until closed) or on Space/Enter.
    Closes on leaving (unless pinned), Esc, losing the focus, a click elsewhere (the launcher's button
    router calls dismiss) and when another tip opens: only one is open at a time."""
    DELAY = 250
    current = None

    def __init__(self, parent, text, scale, font):
        size = int(18 * scale)
        super().__init__(parent, width=size, height=size, bg=parent["bg"], highlightthickness=2,
                         highlightbackground=parent["bg"], highlightcolor=ACCENT, takefocus=1, cursor="hand2")
        self.text, self.scale, self.font, self.size = text, scale, font, size
        self.popover, self.pinned, self.timer = None, False, None
        self.draw(MUTED)
        self.bind("<Enter>", self.enter)
        self.bind("<Leave>", self.leave)
        self.bind("<Button-1>", self.click)
        on_activate(self, self.toggle)  # not with Ctrl: Ctrl+Enter plays
        self.bind("<Escape>", lambda e: self.close())
        self.bind("<FocusOut>", lambda e: self.close())
        self.bind("<Destroy>", lambda e: self.close(), add="+")

    def draw(self, color):
        self.delete("all")
        draw_image(self, 0, 0, art.icon(self, "help", self.size, color))

    def enter(self, event=None):
        self.draw(TEXT)
        if self.popover is None:
            self.timer = self.after(self.DELAY, self.open)

    def leave(self, event=None):
        self.draw(MUTED)
        self.cancel_timer()
        if not self.pinned:
            self.close()

    def click(self, event=None):
        self.cancel_timer()
        self.focus_set()  # Esc and FocusOut then reach this tip
        if self.popover is not None and self.pinned:
            self.close()
        else:
            self.open(pin=True)

    def toggle(self, event=None):
        if self.popover is not None:
            self.close()
        else:
            self.open(pin=True)

    def cancel_timer(self):
        if self.timer is not None:
            try:
                self.after_cancel(self.timer)
            except tk.TclError:
                pass
            self.timer = None

    def open(self, pin=False):
        self.cancel_timer()
        if self.popover is not None:
            self.pinned = self.pinned or pin
            return
        InfoTip.dismiss()
        top, px = self.winfo_toplevel(), lambda v: int(v * self.scale)  # noqa: E731
        frame = tk.Frame(top, bg=LINE, padx=1, pady=1)
        tk.Label(frame, text=self.text, font=self.font, bg=PANEL_HI, fg=TEXT, justify="left", anchor="w",
                 wraplength=px(420), padx=px(12), pady=px(10)).pack()
        frame.update_idletasks()
        w, h = frame.winfo_reqwidth(), frame.winfo_reqheight()
        x = self.winfo_rootx() - top.winfo_rootx()
        y = self.winfo_rooty() - top.winfo_rooty() + self.winfo_height() + px(4)
        if y + h > top.winfo_height() - px(4):  # no room below: above the "?"
            y = self.winfo_rooty() - top.winfo_rooty() - h - px(4)
        x = max(px(4), min(x, top.winfo_width() - w - px(4)))
        y = max(px(4), min(y, top.winfo_height() - h - px(4)))  # also for a "?" scrolled out of view (Tab)
        frame.place(x=x, y=y)
        frame.lift()
        self.popover, self.pinned = frame, pin
        InfoTip.current = self

    def close(self):
        self.cancel_timer()
        self.pinned = False
        popover, self.popover = self.popover, None
        if InfoTip.current is self:
            InfoTip.current = None
        if popover is not None:
            try:
                popover.destroy()
            except tk.TclError:
                pass

    @classmethod
    def dismiss(cls, widget=None):
        """Closes the open popover, unless `widget` is its own "?" (its click handler decides)."""
        tip = cls.current
        if tip is not None and widget is not tip:
            tip.close()


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
        kernel32.GlobalLock.argtypes = kernel32.GlobalUnlock.argtypes = kernel32.GlobalFree.argtypes = [ctypes.c_void_p]
        user32.OpenClipboard.argtypes = [ctypes.c_void_p]
        user32.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
        user32.GetParent.restype = ctypes.c_void_p
        user32.GetParent.argtypes = [ctypes.c_void_p]
        window = root.winfo_id()
        owner = user32.GetParent(window) or window
        data = (text.replace("\r\n", "\n").replace("\n", "\r\n") + "\0").encode("utf-16-le")
        handle = kernel32.GlobalAlloc(0x2, len(data))  # GMEM_MOVEABLE
        ctypes.memmove(kernel32.GlobalLock(handle), data, len(data))
        kernel32.GlobalUnlock(handle)
        # 24 Oct 2026: owned by the Tk window: with a null owner SetClipboardData fails after EmptyClipboard
        if user32.OpenClipboard(owner):
            try:
                user32.EmptyClipboard()
                if user32.SetClipboardData(13, handle):  # CF_UNICODETEXT; Windows owns the memory now
                    return
            finally:
                user32.CloseClipboard()
        kernel32.GlobalFree(handle)  # not taken by the clipboard
    except (AttributeError, OSError, ctypes.ArgumentError):
        pass
    root.clipboard_clear()
    root.clipboard_append(text)


class DialogButton(tk.Label):
    """A rounded button (its pill is the Label's image, behind the text)."""

    @staticmethod
    def colors(style):
        """(fill, hover fill, text) of a style, read when drawn: the accent follows the game."""
        return {"primary": (ACCENT, ACCENT_HI, INK), "secondary": (PANEL_HI, HOVER_LINE, TEXT),
                "danger": (STOP_RED, "#e46662", "#ffffff")}[style]

    def __init__(self, parent, text, command, style, font, scale):
        fill, _, fg = self.colors(style)
        super().__init__(parent, text=text, font=font, bg=parent["bg"], fg=fg, cursor="hand2", bd=0, padx=0, pady=0,
                         compound="center", highlightthickness=0, takefocus=1)
        self.style, self.command, self.s, self.hover = style, command, scale, False
        measure = tkfont.Font(font=font)
        self.w = measure.measure(text) + int(36 * scale)
        self.h = measure.metrics("linespace") + int(16 * scale)
        self.bind("<Button-1>", lambda e: self.invoke())
        self.bind("<Enter>", lambda e: self.set_hover(True))
        self.bind("<Leave>", lambda e: self.set_hover(False))
        # 24 Oct 2026: the focus ring shows where the controller's D-pad left/right is (drawn on the pill).
        self.bind("<FocusIn>", lambda e: self.draw(), add="+")
        self.bind("<FocusOut>", lambda e: self.draw(), add="+")
        self.draw()

    def set_hover(self, hover):
        self.hover = hover
        self.draw()

    def draw(self):
        fill, hover, fg = self.colors(self.style)
        try:
            focus = self.focus_get() is self
        except (KeyError, tk.TclError):
            focus = False
        self.config(fg=fg, image=art.rounded(self, self.w, self.h, self.h / 2, hover if self.hover else fill,
                                             TEXT if focus else None, 2 * self.s))

    def invoke(self):
        self.command()


class Dialog(tk.Toplevel):
    """A modal message in the launcher's colors. Dialog.ask(...) blocks and returns the value of the
    clicked button, or `cancel` for Escape and the close button. Enter clicks the default button.
    `detail` is a read-only monospace box (scrolls when long) with a Copy button."""
    ICONS = {"info": "info", "warning": "warning", "error": "error", "question": "help"}  # assets/icons
    WIDTH = 460
    current = None  # the dialog on screen (the controller navigation in the launcher sends its buttons here)

    def __init__(self, root, title, message, buttons, detail=None, default=0, cancel=None, scale=1.0, icon=None):
        super().__init__(root, bg=PANEL, highlightthickness=1, highlightbackground=LINE)
        self.withdraw()
        self.root, self.result, self.buttons, self.cancel_value = root, cancel, [], cancel
        px = lambda v: int(v * scale)  # noqa: E731
        width, pad = px(self.WIDTH), px(20)
        self.title(title)
        self.resizable(False, False)
        body_font, bold_font = ("Segoe UI", 10), ("Segoe UI Semibold", 11)
        tk.Frame(self, width=width, height=0, bg=PANEL).pack()  # the dialog's width
        header = tk.Frame(self, bg=PANEL_HI)
        header.pack(fill="x")
        if icon in self.ICONS:
            color = {"info": INFO, "error": STOP_RED}.get(icon, ACCENT)
            tk.Label(header, image=art.icon(self, self.ICONS[icon], px(24), color), bg=PANEL_HI, bd=0).pack(
                side="left", padx=(pad, px(10)), pady=px(14))
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
        # Left/Right/Tab walk the buttons, so every one is reachable without a controller
        for sequence, by in (("<Left>", -1), ("<Right>", 1), ("<Tab>", 1), ("<Shift-Tab>", -1), ("<ISO_Left_Tab>", -1)):
            self.bind(sequence, lambda e, by=by: (self.step_button(by), "break")[1])
        self.protocol("WM_DELETE_WINDOW", lambda: self.finish(cancel))

    def copy_detail(self):
        copy_text(self.root, self.detail)
        self.copy_button.config(text="Copied")
        self.after(1500, lambda: self.winfo_exists() and self.copy_button.config(text="Copy"))

    def finish(self, value):
        self.result = value
        self.destroy()

    def press_default(self):
        """The controller's A: the same as Enter."""
        if self.default_button:
            self.default_button.invoke()

    def press_cancel(self):
        """The controller's B: the same as Escape."""
        self.finish(self.cancel_value)

    def step_button(self, by):
        """The controller's D-pad left/right: the next or previous button becomes the default (and
        shows the focus ring), so A and Enter press it."""
        if self.default_button in self.buttons:
            index = min(max(self.buttons.index(self.default_button) + by, 0), len(self.buttons) - 1)
            self.default_button = self.buttons[index]
            self.default_button.focus_set()

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
        if not self.winfo_exists():  # answered (or closed) during that update
            return self.result
        self.grab()
        self.focus_force()
        previous, Dialog.current = Dialog.current, self
        try:
            self.wait_window()
        except tk.TclError:  # the root was destroyed under the dialog
            pass
        Dialog.current = previous
        if previous is not None:  # a dialog over a dialog: the first one gets its grab back
            try:
                if previous.winfo_exists():
                    previous.grab_set()
            except tk.TclError:
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
    KINDS = {"info": ("info", None), "ok": ("check", GOOD), "error": ("error", STOP_RED)}  # icon, colour (None: accent)
    MAX = 3

    def __init__(self, root, text, kind, scale):
        super().__init__(root, bg=PANEL_HI, highlightthickness=1, highlightbackground=LINE)
        self.scale = scale
        name, color = self.KINDS.get(kind, self.KINDS["info"])
        tk.Label(self, image=art.icon(self, name, int(20 * scale), color or ACCENT), bg=PANEL_HI, bd=0).pack(
            side="left", padx=(int(12 * scale), 0))
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
