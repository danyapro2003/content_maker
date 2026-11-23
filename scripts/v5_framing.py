"""Prepare run-local FFmpeg framing filters for a V5 story, in EDL order."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path

from v5_edit_prototype import FPS, crop_path, detect_shots


def segment_filter(index, analysis, folder, prefix):
    width, _, crop, boxes, cuts = analysis
    wide_ranges, commands = [], []
    last = None
    for first, end in zip(cuts, cuts[1:] + [len(boxes)]):
        positions = crop_path(boxes[first:end], width, crop)
        if positions is None:
            wide_ranges.append(f"between(n,{first},{end-1})")
            continue
        for frame, x in enumerate(positions, first):
            fraction = round(float(x) / (width - crop), 6)
            if fraction != last:
                commands.append(f"{frame/FPS:.3f} crop@face{index} x (iw-ow)*{fraction};")
                last = fraction
    base = f"[{index}:v:0]setpts=PTS-STARTPTS,fps={FPS}"
    wide = "scale=1080:1920:force_original_aspect_ratio=decrease,pad=1080:1920:(ow-iw)/2:(oh-ih)/2,setsar=1"
    dest = f"[vp{index}];"
    if not commands:
        return base + "," + wide + dest
    name = f"{prefix}_seg{index}.cmd"
    (folder / name).write_text("\n".join(commands), encoding="ascii")
    # Render from full-resolution source. The 640px proxy only supplies coordinates.
    face = (f"sendcmd=f={name},crop@face{index}=w='min(iw,trunc(ih*9/16/2)*2)':"
            "h=ih:x=(iw-ow)/2:y=0,scale=1080:1920,setsar=1")
    if not wide_ranges:
        return base + "," + face + dest
    return (base + f",split[face{index}][wide{index}];[face{index}]" + face + f"[fc{index}];"
            + f"[wide{index}]" + wide + f"[wc{index}];[fc{index}][wc{index}]overlay=enable='"
            + "+".join(wide_ranges) + "'" + dest)


def musical_ending(track, ranges, budget, ffmpeg, temp):
    import librosa
    import numpy as np
    import soundfile as sf

    # Select continuous music and cut pictures on its transients.
    wav = temp / "track.wav"
    subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(track), "-t", "60",
                    "-ac", "1", "-ar", "22050", str(wav)], check=True)
    audio, sr = sf.read(wav)
    accents = librosa.onset.onset_strength(y=audio, sr=sr)
    peaks = librosa.onset.onset_detect(onset_envelope=accents, sr=sr)
    clips = [(a,b) for a,b in ranges[-3:] if b-a >= 1.0]
    span = min(5., budget, sum(min(1.8,b-a) for a,b in clips))
    length = int(span * FPS)
    if length < FPS or not clips:
        return [], 0
    times = librosa.frames_to_time(peaks, sr=sr)
    eligible = [i for i,t in enumerate(times) if t+length/FPS <= len(audio)/sr]
    if not eligible:
        return [], 0
    hook = times[max(eligible, key=lambda i: accents[peaks[i]])]
    marks = sorted({round((t-hook)*FPS) for t in times if t>hook})
    cuts = [0]
    for i in range(1,len(clips)):
        possible = [m for m in marks if m-cuts[-1]>=10 and length-m >= 10*(len(clips)-i)]
        if not possible:
            return [], 0
        cuts.append(min(possible,key=lambda m: abs(m-length*i/len(clips))))
    shots = []
    for (a,b), left,right in zip(clips,cuts,cuts[1:]+[length]):
        duration = (right-left)/FPS
        if duration > b-a:
            return [], 0
        shots.append((b-duration,b))
    return shots, float(hook)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("source", "plan", "model"):
        parser.add_argument(arg, type=Path)
    parser.add_argument("ffmpeg")
    parser.add_argument("times", nargs="+", type=float)
    parser.add_argument("--track", type=Path)
    parser.add_argument("--limit", type=float, default=190)
    parser.add_argument("--reaction-room", type=float, default=0)
    args = parser.parse_args()
    filters = []
    with tempfile.TemporaryDirectory() as directory:
        proxy = Path(directory) / "proxy.mp4"
        ranges = list(zip(args.times[::2], args.times[1::2], strict=True))
        budget = max(0., args.limit - sum(b-a for a,b in ranges))
        original_end = ranges[-1][1]
        room = min(args.reaction_room, budget, 1.2)
        held = 0.
        if room >= .28:
            # Only keep a visible face on an uninterrupted shot after the last line.
            pre = min(.32, original_end-ranges[-1][0])
            subprocess.run([args.ffmpeg, "-v", "error", "-y", "-ss", str(original_end-pre),
                "-t", str(pre+room), "-i", str(args.source), "-an", "-vf", f"fps={FPS},scale=640:-2",
                str(proxy)], check=True)
            _, _, _, faces, cuts = detect_shots(proxy, args.model)
            boundary = round(pre*FPS)
            end_frame = min((c for c in cuts if c >= boundary), default=len(faces))
            if faces[boundary:end_frame] and all(faces[boundary:end_frame]):
                held = max(0., min(room,(end_frame-boundary)/FPS))
                if held < .28:
                    held = 0.
            ranges[-1] = (ranges[-1][0], original_end+held)
        shots, hook = musical_ending(args.track, ranges, budget-held, args.ffmpeg, Path(directory)) if args.track else ([],0.)
        for index, (start, end) in enumerate(ranges+shots):
            subprocess.run([args.ffmpeg, "-loglevel", "error", "-y", "-ss", str(start), "-t", str(end-start),
                "-i", str(args.source), "-an", "-vf", f"fps={FPS},scale=640:-2,setsar=1",
                "-c:v", "libx264", str(proxy)], check=True)
            filters.append(segment_filter(index, detect_shots(proxy, args.model), args.plan.parent, args.plan.stem))
    pending = args.plan.with_suffix(".tmp")
    pending.write_text(json.dumps(dict(filters=filters, reaction=held, shots=shots, hook=hook)), encoding="utf8")
    pending.replace(args.plan)


if __name__ == "__main__":
    main()
