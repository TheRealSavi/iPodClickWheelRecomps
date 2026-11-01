"""Focused launcher tests; authored executables/paths only, no game assets."""

import json
import sys
import tempfile
import time
import tkinter as tk
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from launcher import Launcher
from launcher_config import (
    GAMES,
    GameOptions,
    build_command,
    load_settings,
    save_settings,
)


class LauncherCommands(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "games with spaces"
        for game in GAMES:
            exe = self.root / game.folder / "build" / game.executable
            exe.parent.mkdir(parents=True)
            exe.touch()
        runner = self.root / "Phase/tools/run.py"
        runner.parent.mkdir()
        runner.touch()
        self.image = self.root / "test image.bin"
        self.image.touch()
        self.assets = self.root / "game files"
        self.assets.mkdir()

    def phase_options(self):
        return GameOptions(
            executable=str(self.root / "Phase/build/phase-startup.exe"),
            image=str(self.image),
            assets=str(self.assets),
        )

    def test_phase_default_is_checked_visible_audible_and_unlimited(self):
        command = build_command(self.root, GAMES[0], self.phase_options())
        self.assertEqual(command[1], str(self.root / "Phase/tools/run.py"))
        self.assertIn("--visible", command)
        self.assertIn("--audio", command)
        self.assertIn(str(self.image), command)
        self.assertNotIn("--frames", command)
        self.assertNotIn("--timeout", command)
        self.assertNotIn("--ephemeral", command)
        self.assertNotIn("--save-dir", command)

    def test_all_six_desktop_commands_and_paths_with_spaces(self):
        settings = load_settings(self.root / "launcher.json", self.root)
        for game in GAMES[1:]:
            with self.subTest(game=game.key):
                options = settings.games[game.key]
                self.assertEqual(
                    build_command(self.root, game, options), [options.executable]
                )
                options.image, options.assets = str(self.image), str(self.assets)
                options.fps, options.render_scale = "60", "3"
                options.extra = (
                    "--time=12:00\n--call-log=C:\\path with spaces\\calls.log"
                )
                command = build_command(self.root, game, options)
                self.assertEqual(command[1], str(self.image))
                self.assertIn("--gamedir=" + str(self.assets), command)
                self.assertIn("--fps=60", command)
                self.assertIn("--render-scale=3", command)
                self.assertEqual(
                    command[-1], "--call-log=C:\\path with spaces\\calls.log"
                )

    def test_phase_optional_flags_and_limits(self):
        options = self.phase_options()
        options.library = str(self.assets)
        options.save_dir = str(self.root / "new saves")
        options.audio, options.quiet_sdk = False, False
        options.frames, options.timeout = "120", "2.5"
        options.render_scale = "4"
        options.screenshot = str(self.root / "image.ppm")
        command = build_command(self.root, GAMES[0], options)
        self.assertEqual(command[command.index("--render-scale") + 1], "4")
        for flag in (
            "--library",
            "--save-dir",
            "--frames",
            "--timeout",
            "--screenshot",
        ):
            self.assertIn(flag, command)
        self.assertNotIn("--audio", command)
        self.assertNotIn("--quiet-sdk", command)
        options.ephemeral = True
        with self.assertRaisesRegex(ValueError, "temporary saves"):
            build_command(self.root, GAMES[0], options)
        options.save_dir = ""
        self.assertIn("--ephemeral", build_command(self.root, GAMES[0], options))

    def test_bad_paths_and_invalid_numbers_fail_before_launch(self):
        with self.assertRaisesRegex(ValueError, "image"):
            build_command(self.root, GAMES[0], GameOptions())
        options = self.phase_options()
        for field, values in (
            ("timeout", ["NaN", "inf", "-1", "601"]),
            ("frames", ["0", "1.5", "36001"]),
            ("render_scale", ["0", "1.5", "5"]),
        ):
            for value in values:
                with self.subTest(field=field, value=value):
                    setattr(options, field, value)
                    with self.assertRaises(ValueError):
                        build_command(self.root, GAMES[0], options)
            setattr(options, field, "")
        options.image = str(self.root / "missing.bin")
        with self.assertRaisesRegex(ValueError, "does not exist"):
            build_command(self.root, GAMES[0], options)

    def test_settings_round_trip_and_corruption_preserved(self):
        path = self.root / "launcher/settings.json"
        settings = load_settings(path, self.root)
        settings.selected = "lost"
        settings.games["phase"] = self.phase_options()
        settings.games["lost"].extra = "--cheats=unlock-chapters"
        save_settings(path, settings)
        self.assertEqual(load_settings(path, self.root), settings)
        path.write_text('{"version":99,"games":{}}')
        with self.assertRaises(ValueError):
            load_settings(path, self.root)
        self.assertEqual(json.loads(path.read_text())["version"], 99)


class LauncherWindow(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.window = tk.Tk()
        self.window.withdraw()
        self.app = Launcher(
            self.window, root=self.root, config_dir=self.root / "config"
        )
        self.addCleanup(self.app.close)
        self.window.update()

    def test_selection_forms_and_saved_options(self):
        self.app.variables["image"].set("C:/music/a song.bin")
        self.app.extra.insert("1.0", "--example\nC:\\path with spaces")
        # No event-loop update: switching must still save the last edit.
        self.app.games.selection_set("lost")
        self.app.select_game()
        self.app.variables["fps"].set("60")
        self.app.games.selection_set("phase")
        self.app.select_game()
        self.assertEqual(self.app.variables["image"].get(), "C:/music/a song.bin")
        self.assertIn("path with spaces", self.app.extra.get("1.0", "end"))
        for game in GAMES:
            self.app.games.selection_set(game.key)
            self.app.select_game()
            self.window.update()
            self.assertEqual(self.app.current, game)
        self.app.save()
        loaded = load_settings(self.app.settings_path, self.root)
        self.assertEqual(loaded.games["lost"].fps, "60")
        self.assertIsNone(self.app.pending_save)

    def test_play_runs_child_without_blocking_and_reports_exit(self):
        child = [
            sys.executable,
            "-c",
            "import sys; print(sys.argv[1]); sys.exit(7)",
            "space & literal",
        ]
        with patch("launcher.build_command", return_value=child):
            self.app.play_button.invoke()
        self.assertIn("phase", self.app.processes)
        self.assertTrue(self.app.play_button.instate(["disabled"]))
        deadline = time.monotonic() + 5
        while self.app.processes and time.monotonic() < deadline:
            self.window.update()
            time.sleep(0.02)
        self.assertFalse(self.app.processes)
        self.assertIn("code 7", self.app.status.get())
        self.assertIn("space & literal", self.app.logs["phase"].read_text())
        self.assertFalse(self.app.play_button.instate(["disabled"]))

    def test_invalid_setup_shows_actionable_error(self):
        with patch("launcher.messagebox.showerror") as error:
            self.app.play()
        error.assert_called_once()
        self.assertIn("Build Phase first", error.call_args.args[1])
        self.assertFalse(self.app.processes)


if __name__ == "__main__":
    unittest.main()
