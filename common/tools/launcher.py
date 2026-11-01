"""Small Windows desktop launcher; no third-party Python packages required."""

import os
import subprocess
import sys
import tkinter as tk
import traceback
from datetime import UTC, datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from launcher_config import (
    GAMES,
    build_command,
    load_settings,
    save_settings,
    settings_directory,
)

ROOT = Path(__file__).resolve().parents[2]


class Launcher:
    def __init__(
        self, window: tk.Tk, root: Path = ROOT, config_dir: Path | None = None
    ):
        self.window, self.root = window, root
        self.directory = config_dir if config_dir is not None else settings_directory()
        self.settings_path = self.directory / "settings.json"
        self.settings = load_settings(self.settings_path, root)
        self.current = next(g for g in GAMES if g.key == self.settings.selected)
        self.variables: dict[str, tk.Variable] = {}
        self.processes: dict[str, subprocess.Popen] = {}
        self.logs: dict[str, Path] = {}
        self.pending_save = None
        window.title("iPod Games")
        window.geometry("920x680")
        window.minsize(820, 620)
        window.protocol("WM_DELETE_WINDOW", self.close)
        style = ttk.Style(window)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Title.TLabel", font=("Segoe UI", 19, "bold"))
        style.configure("Treeview", rowheight=36, font=("Segoe UI", 10))
        style.configure("Play.TButton", font=("Segoe UI", 11, "bold"), padding=(24, 10))
        shell = ttk.Frame(window, padding=20)
        shell.pack(fill="both", expand=True)
        ttk.Label(shell, text="iPod Games", style="Title.TLabel").pack(anchor="w")
        ttk.Label(
            shell, text="Choose a game. Set it up once. Play whenever you like."
        ).pack(anchor="w", pady=(4, 18))
        body = ttk.Frame(shell)
        body.pack(fill="both", expand=True)
        self.games = ttk.Treeview(body, show="tree", selectmode="browse", height=7)
        self.games.column("#0", width=185, stretch=False)
        self.games.pack(side="left", fill="y", padx=(0, 20))
        for game in GAMES:
            self.games.insert("", "end", iid=game.key, text=game.title)
        self.games.bind("<<TreeviewSelect>>", self.select_game)
        self.games.bind("<Double-1>", self.double_click)
        self.games.bind("<Return>", lambda _event: self.play())
        self.panel = ttk.Frame(body)
        self.panel.pack(side="left", fill="both", expand=True)
        self.status = tk.StringVar(
            value="Options save automatically. Double-click a game to play."
        )
        ttk.Label(shell, textvariable=self.status, wraplength=850).pack(
            anchor="w", pady=(14, 0)
        )
        self.games.selection_set(self.current.key)
        self.show_game()
        self.poll_timer = window.after(400, self.poll)

    def capture_options(self):
        # Read directly as well: Play can be invoked before Tk delivers a Text
        # modified notification (for example through a keyboard shortcut).
        if "extra" in self.variables and self.variables[
            "extra"
        ].get() != self.extra.get("1.0", "end-1c"):
            self.variables["extra"].set(self.extra.get("1.0", "end-1c"))
        options = self.settings.games[self.current.key]
        for name, variable in self.variables.items():
            setattr(options, name, variable.get())

    def save(self) -> bool:
        self.capture_options()
        if self.pending_save is not None:
            self.window.after_cancel(self.pending_save)
            self.pending_save = None
        self.settings.selected = self.current.key
        try:
            save_settings(self.settings_path, self.settings)
            return True
        except OSError as error:
            self.status.set(f"Could not save options: {error}")
            return False

    def changed(self, *_):
        if self.pending_save is not None:
            self.window.after_cancel(self.pending_save)
        self.pending_save = self.window.after(500, self.save)

    def select_game(self, _event=None):
        selection = self.games.selection()
        if not selection or selection[0] == self.current.key:
            return
        self.save()
        self.current = next(g for g in GAMES if g.key == selection[0])
        self.show_game()
        self.save()

    def double_click(self, event):
        if self.games.identify_row(event.y):
            self.select_game()
            self.play()

    def field(self, parent, name, label, browse=None):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=5)
        ttk.Label(row, text=label, width=18).pack(side="left")
        value = getattr(self.settings.games[self.current.key], name)
        variable = tk.StringVar(value=value)
        self.variables[name] = variable
        entry = ttk.Entry(row, textvariable=variable)
        entry.pack(side="left", fill="x", expand=True)
        if browse:

            def choose():
                if browse == "directory":
                    result = filedialog.askdirectory(parent=self.window, title=label)
                elif browse == "save":
                    result = filedialog.asksaveasfilename(
                        parent=self.window, title=label
                    )
                else:
                    result = filedialog.askopenfilename(parent=self.window, title=label)
                if result:
                    variable.set(result)

            ttk.Button(row, text="Browse…", command=choose).pack(
                side="left", padx=(6, 0)
            )
        variable.trace_add("write", self.changed)
        return entry

    def checkbox(self, parent, name, label):
        variable = tk.BooleanVar(
            value=getattr(self.settings.games[self.current.key], name)
        )
        self.variables[name] = variable
        ttk.Checkbutton(parent, text=label, variable=variable).pack(anchor="w", pady=5)
        variable.trace_add("write", self.changed)

    def show_game(self):
        for widget in self.panel.winfo_children():
            widget.destroy()
        self.variables.clear()
        phase = self.current.key == "phase"
        ttk.Label(self.panel, text=self.current.title, style="Title.TLabel").pack(
            anchor="w"
        )
        ttk.Label(
            self.panel,
            text="Windows preview · uses the verified Phase runner"
            if phase
            else "Leave optional paths blank to use the game's installed files.",
            wraplength=600,
        ).pack(anchor="w", pady=(5, 14))
        notebook = ttk.Notebook(self.panel)
        notebook.pack(fill="both", expand=True)
        setup, options, advanced = (ttk.Frame(notebook, padding=14) for _ in range(3))
        for tab, label in (
            (setup, "Game setup"),
            (options, "Launch options"),
            (advanced, "Advanced"),
        ):
            notebook.add(tab, text=label)
        if not phase:
            self.field(setup, "executable", "Game executable", "file")
        self.field(
            setup, "image", "Game image" + (" *" if phase else " (optional)"), "file"
        )
        self.field(
            setup,
            "assets",
            "Game files" + (" *" if phase else " (optional)"),
            "directory",
        )
        if phase:
            self.field(setup, "library", "Music library", "directory")
            self.field(setup, "save_dir", "Save folder", "directory")
            ttk.Label(
                setup,
                text="* Required. Game image: decrypted Phase .bin file.\nGame files: folder with main.hdr and main_0.ark.\nMusic library: iPod folder with iTunesDB and analyzed Phase data.\nBlank save folder uses your existing local profiles and scores.",
                wraplength=560,
            ).pack(anchor="w", pady=14)
            self.checkbox(options, "audio", "Enable music and sound")
            self.checkbox(options, "quiet_sdk", "Keep diagnostic logging quiet")
            self.field(options, "render_scale", "Render scale (1–4)")
            self.checkbox(
                options,
                "ephemeral",
                "Temporary saves — discard this session's profiles and scores",
            )
            ttk.Label(
                options,
                text="Render scale: blank or 1 = original; 2–4 = experimental HD.\n4 draws supported geometry at 1280×960. Bitmap detail is limited.\nResize the window to enlarge it. F6 cycles video filters.\nF7 toggles integer scaling.",
                wraplength=560,
            ).pack(anchor="w", pady=16)
        else:
            ttk.Label(
                setup,
                text="Choose the desktop executable, not a headless test build.\nIf you supply a game image, also supply its game files folder.\nThe game offers its own first-run installer when paths are blank.",
                wraplength=560,
            ).pack(anchor="w", pady=14)
            self.field(options, "fps", "Frame rate (FPS)")
            self.field(options, "render_scale", "Render scale (1–8)")
            ttk.Label(
                options,
                text="Blank values keep the game's saved settings.\nRender scale controls internal drawing resolution.\nUse F6 in-game to choose an upscaling filter.",
                wraplength=560,
            ).pack(anchor="w", pady=14)
        self.field(advanced, "frames", "Frame limit")
        if phase:
            self.field(advanced, "timeout", "Time limit (seconds)")
            self.field(advanced, "screenshot", "Screenshot on exit", "save")
        self.field(advanced, "input_script", "Input script", "file")
        ttk.Label(
            advanced, text="Blank limits let you play until you close the game."
        ).pack(anchor="w", pady=(2, 10))
        ttk.Label(
            advanced,
            text="Additional arguments — one argument per line, no surrounding quotes.",
            wraplength=560,
        ).pack(anchor="w")
        self.extra = tk.Text(
            advanced, height=4, wrap="none", font=("Consolas", 10), undo=True
        )
        self.extra.pack(fill="both", expand=True, pady=5)
        variable = tk.StringVar(value=self.settings.games[self.current.key].extra)
        self.variables["extra"] = variable
        self.extra.insert("1.0", variable.get())
        self.extra.edit_modified(False)

        def edit(_event):
            if self.extra.edit_modified():
                variable.set(self.extra.get("1.0", "end-1c"))
                self.extra.edit_modified(False)

        self.extra.bind("<<Modified>>", edit)
        variable.trace_add("write", self.changed)
        ttk.Label(
            advanced,
            text="For a separate value, put the flag and value on separate lines.\nOther games usually use --flag=value (for example --time=12:00).",
            wraplength=560,
        ).pack(anchor="w")
        footer = ttk.Frame(self.panel)
        footer.pack(fill="x", pady=(14, 0))
        self.play_button = ttk.Button(
            footer, text="Play", style="Play.TButton", command=self.play
        )
        self.play_button.pack(side="right")
        ttk.Button(footer, text="Open last log", command=self.open_log).pack(
            side="left"
        )
        ttk.Button(footer, text="Game guide", command=self.open_guide).pack(
            side="left", padx=6
        )
        self.update_play_button()

    def update_play_button(self):
        running = self.current.key in self.processes
        self.play_button.configure(
            text="Running…" if running else "Play",
            state="disabled" if running else "normal",
        )

    def play(self):
        if self.current.key in self.processes:
            return
        if not self.save():
            messagebox.showerror(
                "Could not save options", self.status.get(), parent=self.window
            )
            return
        try:
            command = build_command(
                self.root, self.current, self.settings.games[self.current.key]
            )
            log_dir = self.directory / "logs"
            log_dir.mkdir(parents=True, exist_ok=True)
            log = (
                log_dir / f"{self.current.key}-{datetime.now(UTC):%Y%m%d-%H%M%S-%f}.log"
            )
            with log.open("w", encoding="utf-8") as stream:
                stream.write(subprocess.list2cmdline(command) + "\n\n")
                stream.flush()
                process = subprocess.Popen(
                    command,
                    cwd=self.root,
                    stdin=subprocess.DEVNULL,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                )
            self.logs[self.current.key] = log
            self.processes[self.current.key] = process
            self.status.set(
                f"{self.current.title} is running. Closing the launcher leaves your game open."
            )
            self.update_play_button()
        except (OSError, ValueError) as error:
            messagebox.showerror("Cannot start game", str(error), parent=self.window)

    def poll(self):
        for key, process in list(self.processes.items()):
            code = process.poll()
            if code is not None:
                del self.processes[key]
                title = next(g.title for g in GAMES if g.key == key)
                self.status.set(
                    f"{title} closed."
                    if code == 0
                    else f"{title} exited with code {code}. Open its last log for details."
                )
        self.update_play_button()
        self.poll_timer = self.window.after(400, self.poll)

    def open_log(self):
        log = self.logs.get(self.current.key)
        if log is None:
            log = next(
                iter(
                    sorted(
                        (self.directory / "logs").glob(f"{self.current.key}-*.log"),
                        reverse=True,
                    )
                ),
                None,
            )
        if log is None:
            messagebox.showinfo(
                "No log yet", "Launch this game to create a log.", parent=self.window
            )
        else:
            self.open_path(log)

    def open_guide(self):
        self.open_path(self.root / self.current.folder / "README.md")

    def open_path(self, path):
        try:
            os.startfile(path)
        except OSError as error:
            messagebox.showerror("Cannot open file", str(error), parent=self.window)

    def close(self):
        if self.save():
            self.window.after_cancel(self.poll_timer)
            self.window.destroy()
        else:
            messagebox.showerror(
                "Could not save options", self.status.get(), parent=self.window
            )


def main():
    if "--detach" in sys.argv and os.name == "nt":
        directory = settings_directory()
        directory.mkdir(parents=True, exist_ok=True)
        pythonw = Path(sys.executable).with_name("pythonw.exe")
        with (directory / "launcher.log").open("w") as log:
            subprocess.Popen(
                [str(pythonw), str(Path(__file__).resolve())],
                cwd=ROOT,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                creationflags=subprocess.DETACHED_PROCESS
                | subprocess.CREATE_NEW_PROCESS_GROUP,
            )
        return
    window = tk.Tk()
    try:
        Launcher(window)
    except (OSError, ValueError) as error:
        window.withdraw()
        messagebox.showerror("Launcher setup", str(error), parent=window)
        window.destroy()
        return
    window.mainloop()


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        raise
