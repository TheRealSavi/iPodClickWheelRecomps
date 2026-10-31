"""Build the bounded Phase startup probe using installed Windows MSVC tools."""

import argparse
import os
from pathlib import Path
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--vcvars", type=Path, help="explicit vcvars64.bat; otherwise use vswhere"
    )
    args = parser.parse_args()
    vcvars = args.vcvars
    if vcvars is None:
        vswhere = (
            Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
            / "Microsoft Visual Studio/Installer/vswhere.exe"
        )
        result = subprocess.run(
            [
                str(vswhere),
                "-latest",
                "-products",
                "*",
                "-requires",
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-property",
                "installationPath",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        if not result.stdout.strip():
            raise SystemExit("Install MSVC x64 Build Tools or pass --vcvars.")
        vcvars = Path(result.stdout.strip()) / "VC/Auxiliary/Build/vcvars64.bat"
    sources = sorted((PROJECT / "gen").glob("*.cpp"))
    if not sources:
        raise SystemExit("Run tools/generate.py first.")
    subprocess.run(
        [sys.executable, str(PROJECT / "tests/verify_instructions.py"), "--generate"],
        check=True,
    )
    build = PROJECT / "build"
    build.mkdir(exist_ok=True)
    (build / "obj").mkdir(exist_ok=True)
    # Response files preserve spaces without constructing a long shell command.
    arguments = [
        "/nologo",
        "/EHsc",
        "/std:c++17",
        "/Od",
        "/MP8",
        "/utf-8",
        f'/I"{PROJECT.parent / "common/src"}"',
        f'/I"{PROJECT / "src"}"',
        f'/I"{PROJECT / "gen"}"',
        *[f'"{p}"' for p in sources],
        f'"{PROJECT / "src/main.cpp"}"',
        "/Fe:phase-startup.exe",
        "/Fo:obj\\",
    ]
    (build / "compile.rsp").write_text("\n".join(arguments) + "\n")
    # These are local tool paths, never input read from the game.
    if any(c in str(vcvars) for c in '\r\n%"'):
        raise SystemExit("Unsupported characters in compiler setup path")
    (build / "compile.cmd").write_text(
        f'@echo off\ncall "{vcvars}" >nul\nif errorlevel 1 exit /b 1\ncl @compile.rsp\nexit /b %errorlevel%\n'
    )
    with (build / "build.log").open("w") as log:
        result = subprocess.run(
            ["cmd", "/d", "/c", "compile.cmd"],
            cwd=build,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
    if result.returncode:
        print((build / "build.log").read_text(errors="replace")[-8000:])
        raise SystemExit(result.returncode)
    print(build / "phase-startup.exe")


if __name__ == "__main__":
    main()
