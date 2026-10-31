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
                return child.call_args.args[0], project

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
