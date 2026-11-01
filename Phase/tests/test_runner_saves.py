"""Keep interactive persistence separate from deterministic diagnostic sessions."""

import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run


class RunnerSaves(unittest.TestCase):
    def test_hd_scale_is_forwarded_and_validated(self):
        command, _ = self.command("--render-scale", "4")
        self.assertEqual(command[command.index("--render-scale") + 1], "4")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.command("--render-scale", "5")

    def command(self, *options):
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            (project / "build").mkdir()
            with (
                patch.object(run, "PROJECT", project),
                patch.object(run, "checked_image"),
                patch.object(
                    sys,
                    "argv",
                    ["run.py", "--image", "game.bin", "--assets", "assets", *options],
                ),
                patch.dict(run.os.environ, {"LOCALAPPDATA": str(project / "user")}),
                patch.object(
                    run.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0, "", ""),
                ) as child,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(run.main(), 0)
                self.child_options = child.call_args.kwargs
                return child.call_args.args[0], project

    def test_interactive_launch_has_no_implicit_play_limit(self):
        command, _ = self.command("--visible", "--audio", "--quiet-sdk")
        self.assertNotIn("--frames", command)
        self.assertIsNone(self.child_options["timeout"])

    def test_diagnostic_and_explicit_limits_remain_available(self):
        command, _ = self.command()
        self.assertEqual(command[command.index("--frames") + 1], "300")
        self.assertEqual(self.child_options["timeout"], 15)
        command, _ = self.command("--visible", "--frames", "120", "--timeout", "30")
        self.assertEqual(command[command.index("--frames") + 1], "120")
        self.assertEqual(self.child_options["timeout"], 30)

    def test_visible_defaults_to_user_save_directory(self):
        command, project = self.command("--visible")
        self.assertEqual(
            Path(command[command.index("--save-dir") + 1]),
            project / "user/iPodClickWheelRecomps/Phase",
        )

    def test_hidden_and_ephemeral_do_not_touch_user_saves(self):
        for options in ((), ("--visible", "--ephemeral")):
            command, _ = self.command(*options)
            self.assertNotIn("--save-dir", command)

    def test_explicit_directory_overrides_default(self):
        command, _ = self.command("--visible", "--save-dir", "portable-saves")
        self.assertEqual(
            Path(command[command.index("--save-dir") + 1]),
            Path("portable-saves").resolve(),
        )


if __name__ == "__main__":
    unittest.main()
