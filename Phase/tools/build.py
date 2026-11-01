"""Build the bounded Phase startup probe using installed Windows MSVC tools."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--vcvars", type=Path, help="explicit vcvars64.bat; otherwise use vswhere"
    )
    parser.add_argument(
        "--rtx-video-sdk",
        type=Path,
        default=Path(os.environ["RTX_VIDEO_SDK"])
        if os.environ.get("RTX_VIDEO_SDK")
        else None,
        help="optional external NVIDIA RTX Video SDK root for AI upscaling",
    )
    parser.add_argument(
        "--sdl-root",
        type=Path,
        default=Path(os.environ.get("SDL3_ROOT", PROJECT / "build/deps/SDL3-3.4.16")),
        help="SDL3 Visual C++ SDK root (or SDL3_ROOT environment variable)",
    )
    args = parser.parse_args()
    sdl = args.sdl_root.resolve()
    for relative in (
        "include/SDL3/SDL.h",
        "lib/x64/SDL3.lib",
        "lib/x64/SDL3.dll",
        "LICENSE.txt",
    ):
        if not (sdl / relative).is_file():
            raise SystemExit(
                f"Missing {sdl / relative}. Install the SDL3 VC SDK and pass --sdl-root."
            )
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
        f'/I"{sdl / "include"}"',
        *[f'"{p}"' for p in sources],
        f'"{PROJECT / "src/main.cpp"}"',
        f'"{PROJECT / "src/host.cpp"}"',
        f'"{PROJECT.parent / "common/src/ipod/platform/sdl3/presenter.cpp"}"',
        f'"{PROJECT.parent / "common/src/ipod/platform/sdl3/nis.cpp"}"',
        f'"{PROJECT.parent / "common/src/ipod/platform/sdl3/rtx_video.cpp"}"',
        f'"{PROJECT / "src/keyboard.cpp"}"',
        f'"{PROJECT / "src/library.cpp"}"',
        f'"{PROJECT / "src/audio.cpp"}"',
        f'"{PROJECT / "src/persistence.cpp"}"',
        f'"{PROJECT / "tests/host_contract.cpp"}"',
        f'"{PROJECT / "tests/persistence_contract.cpp"}"',
        f'"{PROJECT / "tests/library_contract.cpp"}"',
        "opengl32.lib",
        "gdi32.lib",
        "user32.lib",
        "mfplat.lib",
        "mfreadwrite.lib",
        "mfuuid.lib",
        "ole32.lib",
        "xaudio2.lib",
        f'"{sdl / "lib/x64/SDL3.lib"}"',
        "/Fe:phase-startup.exe",
        "/Fo:obj\\",
    ]
    if args.rtx_video_sdk:
        rtx = args.rtx_video_sdk.resolve()
        for relative in (
            "include/nvsdk_ngx_helpers_vsr.h",
            "lib/Windows/x64/nvsdk_ngx_s.lib",
            "bin/Windows/x64/rel/nvngx_vsr.dll",
            "NVIDIA_RTX_Video_SDK_License.pdf",
        ):
            if not (rtx / relative).is_file():
                raise SystemExit(f"Missing RTX Video SDK file: {rtx / relative}")
        arguments.extend(
            [
                "/DIPOD_HAS_RTX_VIDEO=1",
                f'/I"{rtx / "include"}"',
                f'"{rtx / "lib/Windows/x64/nvsdk_ngx_s.lib"}"',
                "advapi32.lib",
            ]
        )
        shutil.copy2(rtx / "bin/Windows/x64/rel/nvngx_vsr.dll", build)
        shutil.copy2(rtx / "NVIDIA_RTX_Video_SDK_License.pdf", build)
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
            check=False,
        )
    if result.returncode:
        print((build / "build.log").read_text(errors="replace")[-8000:])
        raise SystemExit(result.returncode)
    shutil.copy2(sdl / "lib/x64/SDL3.dll", build / "SDL3.dll")
    shutil.copy2(sdl / "LICENSE.txt", build / "SDL3-LICENSE.txt")
    shutil.copy2(
        PROJECT.parent / "common/licenses/NVIDIA-NIS-LICENSE.txt",
        build / "NVIDIA-NIS-LICENSE.txt",
    )
    print(build / "phase-startup.exe")


if __name__ == "__main__":
    main()
