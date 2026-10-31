"""Create a private local test iPod using iOpenPod's real analyzer and writers.

Run with ``uv run --project PATH_TO_IOPENPOD python ...``. Input is a JSON list
of source audio paths with optional offset_s/length_ms excerpt bounds. Source
files are read only. Output must be a new directory; never point at an iPod.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument(
        "--cached-parts",
        type=Path,
        help="optional iOpenPod research NPZ directory; source SHA256 must match manifest",
    )
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output must not exist (create a new local test directory)")
    import librosa
    import mutagen
    import numpy as np
    from iOpenPod.app.phase_audio import PHASE_ANALYSIS_VERSION, analyze_phase_pcm
    from iOpenPod.app.phase_parts import PhasePart, PhasePartKind
    from iPodDB.games.phase import (
        PhaseLibrary,
        PhaseTrack,
        analysis_relative_path,
        build_analysis,
        build_library,
    )
    from iPodDB.iTunesDB.builder.build_iTunesDB import new_itunes_chunk as chunk
    from iPodDB.iTunesDB.builder.build_iTunesDB import new_iTunesDB, new_string_mhod
    from iPodDB.iTunesDB.shared.chunk_defs.mhbd import MhbdHeader
    from iPodDB.iTunesDB.shared.chunk_defs.mhip import DEFINITION as MHIP
    from iPodDB.iTunesDB.shared.chunk_defs.mhip import MhipHeader
    from iPodDB.iTunesDB.shared.chunk_defs.mhit import DEFINITION as MHIT
    from iPodDB.iTunesDB.shared.chunk_defs.mhit import MhitHeader
    from iPodDB.iTunesDB.shared.chunk_defs.mhlp import DEFINITION as MHLP
    from iPodDB.iTunesDB.shared.chunk_defs.mhlt import DEFINITION as MHLT
    from iPodDB.iTunesDB.shared.chunk_defs.mhsd import DEFINITION as MHSD
    from iPodDB.iTunesDB.shared.chunk_defs.mhsd import MhsdHeader
    from iPodDB.iTunesDB.shared.chunk_defs.mhyp import DEFINITION as MHYP
    from iPodDB.iTunesDB.shared.chunk_defs.mhyp import MhypHeader
    from iPodDB.iTunesDB.writer.write_iTunesDB import write_iTunesDB
    from iPodDB.library import IPodLibrary
    from iPodDB.shared.chunk import EmptyChunkHeader

    cases = json.loads(args.manifest.read_text())
    if not 1 <= len(cases) <= 100:
        parser.error("manifest must contain 1..100 tracks")
    ffmpeg = shutil.which(args.ffmpeg)
    if not ffmpeg:
        parser.error("FFmpeg not found")
    args.output.mkdir(parents=True)
    root = args.output.resolve()
    game = root / "iPod_Control/GameData_RW/1D000"
    playlist_pid = 0x50484153454C4942
    tracks, analysis_tracks, receipts = [], [], []
    for index, case in enumerate(cases, 1):
        source = Path(case["source"])
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        if "sha256" in case and case["sha256"] != digest:
            raise ValueError(f"Source changed: {source}")
        offset = float(case.get("offset_s", 0))
        duration = case.get("length_ms")
        info = mutagen.File(source, easy=True)
        title = info.get("title", [source.stem])[0]
        artist = info.get("artist", [""])[0]
        album = info.get("album", [""])[0]
        genre = info.get("genre", [""])[0]
        if offset or (duration and duration < round(info.info.length * 1000) - 100):
            title += " (excerpt)"
        relative = f"iPod_Control/Music/F00/T{index:04}.wav"
        audio = root / relative
        audio.parent.mkdir(parents=True, exist_ok=True)
        command = [
            ffmpeg,
            "-nostdin",
            "-v",
            "error",
            "-ss",
            str(offset),
            "-i",
            str(source),
        ]
        if duration:
            command += ["-t", str(duration / 1000)]
        command += ["-vn", "-ar", "44100", "-ac", "2", "-c:a", "pcm_s16le", str(audio)]
        subprocess.run(command, check=True, timeout=120)
        mono, _ = librosa.load(audio, sr=22050, mono=True)
        length = round(len(mono) * 1000 / 22050)
        parts = ()
        if args.cached_parts:
            if "sha256" not in case or not case.get("provider"):
                raise ValueError(
                    "Cached parts require source hash and provider provenance"
                )
            name = case["name"]
            if Path(name).name != name or ":" in name:
                raise ValueError("Invalid cached part name")
            with np.load(args.cached_parts / f"{name}.npz", allow_pickle=False) as data:
                # Manifest durations are integer milliseconds; decoding a cut can
                # differ from the original sample-boundary excerpt by <1 ms.
                if abs(len(data["mono"]) - len(mono)) > 23:
                    raise ValueError("Cached parts excerpt length mismatch")
                parts = tuple(
                    PhasePart(
                        kind,
                        *(
                            data[f"{kind.value}_{field}"]
                            for field in ("bands", "energy", "chroma")
                        ),
                    )
                    for kind in PhasePartKind
                )
        print(f"Analyzing {title}: {length / 1000:.2f}s", flush=True)
        chart = analyze_phase_pcm(
            mono.astype("<f4").tobytes(), length_ms=length, parts=parts
        )
        encoded = build_analysis(chart)
        pid = (
            int.from_bytes(
                hashlib.sha256(f"{digest}:{offset}:{length}".encode()).digest()[:8],
                "little",
            )
            or 1
        )
        record = PhaseTrack(pid, 0, len(encoded))
        chart_path = game / analysis_relative_path(record)
        chart_path.parent.mkdir(parents=True, exist_ok=True)
        chart_path.write_bytes(encoded)
        analysis_tracks.append(record)
        tracks.append(
            chunk(
                MHIT,
                MhitHeader(
                    track_id=index,
                    db_track_id=pid,
                    db_track_id_2=pid,
                    length=length,
                    size=audio.stat().st_size,
                    media_type=1,
                ),
                children=tuple(
                    new_string_mhod(kind, value)
                    for kind, value in (
                        (1, title),
                        (2, ":" + relative.replace("/", ":")),
                        (3, album),
                        (4, artist),
                        (5, genre),
                        (6, "WAV audio file"),
                    )
                ),
            )
        )
        receipts.append(
            {
                "title": title,
                "artist": artist,
                "pid": f"{pid:016X}",
                "length_ms": length,
                "source_sha256": digest,
                "audio_sha256": hashlib.sha256(audio.read_bytes()).hexdigest(),
                "chart_sha256": hashlib.sha256(encoded).hexdigest(),
                "transients": len(chart.transients),
                "beats": len(chart.beats),
                "offset_s": offset,
                "parts_provider": case.get("provider") if parts else None,
            }
        )
    entries = tuple(
        chunk(MHIP, MhipHeader(track_id=i)) for i in range(1, len(tracks) + 1)
    )
    master = chunk(
        MHYP,
        MhypHeader(playlist_id=1, master_flag=1),
        children=(new_string_mhod(1, "Phase Windows Test Library"), *entries),
    )
    playlist = chunk(
        MHYP,
        MhypHeader(playlist_id=playlist_pid, phase_game_flag=25),
        children=(new_string_mhod(1, "Phase"), *entries),
    )
    database = write_iTunesDB(
        new_iTunesDB(
            MhbdHeader(),
            datasets=(
                chunk(
                    MHSD,
                    MhsdHeader(dataset_type=1),
                    children=(chunk(MHLT, EmptyChunkHeader(), children=tuple(tracks)),),
                ),
                chunk(
                    MHSD,
                    MhsdHeader(dataset_type=3),
                    children=(
                        chunk(MHLP, EmptyChunkHeader(), children=(master, playlist)),
                    ),
                ),
            ),
        )
    )
    verified = IPodLibrary(database).snapshot
    if len(verified.tracks) != len(tracks) or not any(
        p.name == "Phase" for p in verified.playlists
    ):
        raise ValueError("iOpenPod could not round-trip the test library")
    target = root / "iPod_Control/iTunes/iTunesDB"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(database)
    (game / "Phase Data/Phase Library").write_bytes(
        build_library(PhaseLibrary(playlist_pid, tuple(analysis_tracks)))
    )
    (root / "preparation.json").write_text(
        json.dumps(
            {
                "playlist_pid": f"{playlist_pid:016X}",
                "algorithm": PHASE_ANALYSIS_VERSION,
                "tracks": receipts,
            },
            indent=2,
        )
        + "\n"
    )
    print(f"Prepared {len(tracks)} songs: {root}", flush=True)


if __name__ == "__main__":
    main()
