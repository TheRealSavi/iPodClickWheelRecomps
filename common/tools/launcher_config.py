"""Game catalog, saved launcher options, and shell-free launch commands."""

import json
import os
import sys
import tempfile
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path


@dataclass(frozen=True)
class Game:
    key: str
    title: str
    folder: str
    executable: str


GAMES = (
    Game("phase", "Phase", "Phase", "phase-startup.exe"),
    Game("minigolf", "Mini Golf", "Mini Golf", "minigolf.exe"),
    Game("cubis", "Cubis 2", "Cubis2", "cubis.exe"),
    Game("holdem", "Texas Hold'em", "HoldEm", "holdem.exe"),
    Game("lost", "Lost", "Lost", "lost.exe"),
    Game("bowling", "The Sims Bowling", "Sims Bowling", "bowling.exe"),
    Game("vortex", "Vortex", "Vortex", "vortex.exe"),
)


@dataclass
class GameOptions:
    executable: str = ""
    image: str = ""
    assets: str = ""
    library: str = ""
    save_dir: str = ""
    audio: bool = True
    quiet_sdk: bool = True
    ephemeral: bool = False
    fps: str = ""
    render_scale: str = ""
    frames: str = ""
    timeout: str = ""
    screenshot: str = ""
    input_script: str = ""
    extra: str = ""


@dataclass
class Settings:
    selected: str = "phase"
    games: dict[str, GameOptions] = field(default_factory=dict)


def settings_directory() -> Path:
    return (
        Path(os.environ.get("LOCALAPPDATA", Path.home() / ".local/share"))
        / "iPodClickWheelRecomps/Launcher"
    )


def discover_executable(root: Path, game: Game) -> str:
    for directory in (
        "build/Release",
        "build",
        "build/RelWithDebInfo",
        "build/Debug",
        "",
    ):
        candidate = root / game.folder / directory / game.executable
        if candidate.is_file():
            return str(candidate)
    return ""


def load_settings(path: Path, root: Path) -> Settings:
    defaults = {
        g.key: GameOptions(executable=discover_executable(root, g)) for g in GAMES
    }
    if not path.exists():
        return Settings(games=defaults)
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        if document["version"] != 1 or not isinstance(document["games"], dict):
            raise ValueError("unsupported settings format")
        for game in GAMES:
            saved = document["games"].get(game.key, {})
            if not isinstance(saved, dict):
                raise TypeError("invalid game settings")
            options = defaults[game.key]
            for item in fields(GameOptions):
                if item.name in saved:
                    value = saved[item.name]
                    if type(value) is not type(getattr(options, item.name)):
                        raise ValueError(f"invalid {game.title} {item.name}")
                    if item.name == "executable" and not value:
                        continue  # Pick up a newly built game on the next launch.
                    setattr(options, item.name, value)
        selected = document.get("selected", "phase")
        return Settings(selected if selected in defaults else "phase", defaults)
    except (ValueError, KeyError, TypeError) as error:
        raise ValueError(
            f"Cannot read launcher settings at {path}: {error}. The file has been left intact."
        ) from error


def save_settings(path: Path, settings: Settings) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {"version": 1, **asdict(settings)}
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=path.parent, delete=False
        ) as stream:
            temporary = Path(stream.name)
            json.dump(data, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def local_path(value: str, root: Path) -> Path:
    path = Path(os.path.expandvars(value)).expanduser()
    return path if path.is_absolute() else root / path


def build_command(root: Path, game: Game, options: GameOptions) -> list[str]:
    """Validate only supplied options. No frame/time limit is added implicitly."""
    phase = game.key == "phase"
    executable = (
        root / "Phase/build/phase-startup.exe"
        if phase
        else local_path(options.executable, root)
    )
    if not executable.is_file():
        raise ValueError(
            f"{game.title} is not built or its executable is missing. "
            + (
                "Build Phase first; see Phase/README.md."
                if phase
                else "Choose its desktop .exe in Game setup."
            )
        )
    command = [str(executable)]
    if phase:
        runner = root / "Phase/tools/run.py"
        if not runner.is_file():
            raise ValueError("Phase/tools/run.py is missing.")
        python = (
            Path(sys.executable).with_name("python.exe")
            if os.name == "nt"
            else Path(sys.executable)
        )
        command = [str(python), str(runner), "--visible"]

    def path_option(
        name: str,
        flag: str,
        *,
        required: bool = False,
        directory: bool = False,
        output: bool = False,
    ) -> None:
        value = getattr(options, name).strip()
        if not value:
            if required:
                raise ValueError(f"Choose the {name} path in Game setup.")
            return
        path = local_path(value, root)
        if not output and not (path.is_dir() if directory else path.is_file()):
            raise ValueError(f"The {name} path does not exist: {path}")
        if not flag:
            command.append(str(path))
        elif flag.endswith("="):
            command.append(flag + str(path))
        else:
            command.extend([flag, str(path)])

    path_option("image", "--image" if phase else "", required=phase)
    path_option(
        "assets",
        "--assets" if phase else "--gamedir=",
        required=phase or bool(options.image),
        directory=True,
    )
    if phase:
        path_option("library", "--library", directory=True)
        if options.ephemeral and options.save_dir.strip():
            raise ValueError("Clear the save folder or turn off temporary saves.")
        path_option("save_dir", "--save-dir", output=True)
        path_option("screenshot", "--screenshot", output=True)
        path_option("input_script", "--input-script")
        for enabled, flag in (
            (options.audio, "--audio"),
            (options.quiet_sdk, "--quiet-sdk"),
            (options.ephemeral, "--ephemeral"),
        ):
            if enabled:
                command.append(flag)
    else:
        path_option("input_script", "--script=")

    def number(
        value: str,
        label: str,
        flag: str,
        maximum: float | None = None,
        fractional: bool = False,
    ) -> None:
        if not value.strip():
            return
        try:
            parsed = float(value) if fractional else int(value)
            if not 0 < parsed < float("inf") or (
                maximum is not None and parsed > maximum
            ):
                raise ValueError
        except ValueError as error:
            raise ValueError(
                f"{label} must be a positive {'number' if fractional else 'whole number'}"
                + (f" up to {maximum:g}." if maximum else ".")
            ) from error
        if flag.endswith("="):
            command.append(flag + str(parsed))
        else:
            command.extend([flag, str(parsed)])

    number(
        options.frames,
        "Frame limit",
        "--frames" if phase else "--frames=",
        36000 if phase else None,
    )
    if phase:
        number(options.timeout, "Time limit", "--timeout", 600, True)
        number(options.render_scale, "Render scale", "--render-scale", 4)
    else:
        number(options.fps, "Frame rate", "--fps=")
        number(options.render_scale, "Render scale", "--render-scale=", 8)
    # One entry per line avoids shell parsing and Windows backslash/quote ambiguity.
    command.extend(line.strip() for line in options.extra.splitlines() if line.strip())
    return command
