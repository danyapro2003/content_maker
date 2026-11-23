"""V5 offline editing trials. Run reaction or music --help for inputs (seconds)."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path

import cv2
import numpy as np

FPS = 25


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *map(str, args)], check=True)


def crop_path(boxes, width, crop):
    """Find a still composition, or the slowest feasible pan within this shot."""
    from scipy.optimize import linprog
    from scipy.sparse import diags, hstack, vstack, csr_matrix

    known = [i for i, b in enumerate(boxes) if b is not None]
    if not known or crop >= width:
        return None
    left, right = np.array([boxes[i] for i in known]).T
    frames = np.arange(len(boxes))
    low = np.maximum(0, np.interp(frames, known, right) - crop)
    high = np.minimum(width - crop, np.interp(frames, known, left))
    # Padding may extend past the image boundary.
    low = np.minimum(low, width - crop)
    high = np.maximum(high, 0)
    if np.any(low > high):
        return None
    if low.max() <= high.min():
        center = np.median((low + high) / 2)
        return np.full(len(boxes), np.clip(center, low.max(), high.min()))

    # Minimize peak pan speed. Bound acceleration so the window eases in and out.
    n = len(boxes)
    velocity = diags([-1, 1], [0, 1], shape=(n - 1, n))
    speed = hstack([velocity, -np.ones((n - 1, 1))])
    reverse = hstack([-velocity, -np.ones((n - 1, 1))])
    constraints = [speed, reverse]
    limits = [np.zeros(2 * (n - 1))]
    if n > 2:
        acceleration = diags([1, -2, 1], [0, 1, 2], shape=(n - 2, n))
        accel = hstack([acceleration, csr_matrix((n - 2, 1))])
        constraints.extend([accel, -accel])
        limits.append(np.full(2 * (n - 2), crop * 1.5 / FPS**2))
    result = linprog(np.r_[np.zeros(n), 1], A_ub=vstack(constraints),
                     b_ub=np.concatenate(limits),
                     bounds=[*zip(low, high), (0, crop * .65 / FPS)], method="highs")
    return result.x[:-1] if result.success else None


def detect_shots(proxy, model):
    reader = cv2.VideoCapture(str(proxy))
    width = int(reader.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(reader.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if min(width, height) <= 0:
        raise ValueError("No video frames")
    detector = cv2.FaceDetectorYN.create(str(model), "", (width, height), 0.75)
    crop = min(width, round(height * 9 / 16))
    centers = []
    cuts = []
    previous = None
    bounds = None
    frame_index = 0
    while True:
        ok, frame = reader.read()
        if not ok:
            break
        thumb = cv2.resize(frame, (64, 36)).astype(np.float32)
        cut = previous is None or np.mean(np.abs(thumb - previous)) > 26
        if cut:
            cuts.append(frame_index)
            bounds = None
        previous = thumb
        if cut or frame_index % 4 == 0:
            _, faces = detector.detect(frame)
            if faces is not None and len(faces):
                # Ignore tiny background faces. Keep space on both sides of the group.
                prominent = faces[faces[:, 2] >= max(faces[:, 2]) * .48]
                left = min(prominent[:, 0] - prominent[:, 2] * .16)
                right = max(prominent[:, 0] + prominent[:, 2] * 1.16)
                bounds = (float(left), float(right))
        centers.append(bounds)
        frame_index += 1
    reader.release()
    if not centers:
        raise ValueError("Empty reaction interval")
    return width, height, crop, centers, cuts


def reaction(args, temp):
    # Analyze and render identical frames, including the reaction tail after dialogue.
    proxy = temp / "scene.mp4"
    ffmpeg("-ss", args.start, "-i", args.source, "-t", args.end - args.start,
           "-vf", "fps=25,scale=640:-2,setsar=1", "-an", "-c:v", "libx264", proxy)
    width, height, crop, centers, cuts = detect_shots(proxy, args.model)
    reader = cv2.VideoCapture(str(proxy))
    video = temp / "framing.mp4"
    writer = cv2.VideoWriter(str(video), cv2.VideoWriter_fourcc(*"mp4v"), FPS, (540, 960))
    if not writer.isOpened():
        raise RuntimeError("Cannot encode crop preview")
    shots = []
    for start, end in zip(cuts, cuts[1:] + [len(centers)]):
        path = crop_path(centers[start:end], width, crop)
        wide = path is None
        for i in range(start, end):
            ok, frame = reader.read()
            if not ok:
                raise RuntimeError("Proxy changed between analysis and rendering")
            if wide:
                # Fit all of the original picture, including when no face is visible.
                scale = min(540 / width, 960 / height)
                small = cv2.resize(frame, (round(width * scale), round(height * scale)))
                out = np.zeros((960, 540, 3), dtype=np.uint8)
                h, w = small.shape[:2]
                y, dx = (960 - h) // 2, (540 - w) // 2
                out[y:y+h, dx:dx+w] = small
            else:
                center = (float(path[i-start]) + (crop-1)/2, (height-1)/2)
                window = cv2.getRectSubPix(frame, (crop, height), center)
                out = cv2.resize(window, (540, 960))
            writer.write(out)
        shots.append(dict(start=start/FPS, end=end/FPS, framing="wide" if wide else "faces",
                          pan_range_px=None if wide else round(float(np.ptp(path)), 2)))
    writer.release()
    reader.release()
    ffmpeg("-i", video, "-ss", args.start, "-i", args.source, "-map", "0:v", "-map", "1:a:0",
           "-t", len(centers)/FPS, "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", args.output)
    return dict(source_start=args.start, source_end=args.end, shots=shots)


def music(args, temp):
    import librosa
    import soundfile as sf

    wav = temp / "music.wav"
    ffmpeg("-ss", args.hook, "-i", args.track, "-t", args.tail + .6,
           "-vn", "-ac", 1, "-ar", 22050, wav)
    audio, sr = sf.read(wav)
    onset = librosa.onset.onset_strength(y=audio, sr=sr)
    accents = librosa.onset.onset_detect(onset_envelope=onset, sr=sr, units="time")
    near = accents[accents <= .5]
    hook = float(near[0]) if len(near) else 0.
    tail_frames = round(args.tail * FPS)
    if len(audio) / sr + .025 < hook + tail_frames / FPS:
        raise ValueError("Music ends before the last shot")
    # Snap the desired shot lengths to audible transients, on the output frame grid.
    marks = sorted(set(round((t - hook) * FPS) for t in accents if t > hook))
    boundaries = [0]
    for n in range(1, len(args.shots)):
        ideal = n * tail_frames / len(args.shots)
        choices = [m for m in marks if boundaries[-1] + 5 <= m <= tail_frames - 5*(len(args.shots)-n)]
        if not choices:
            raise ValueError("Too few musical accents for the requested number of cuts")
        boundaries.append(min(choices, key=lambda m: abs(m - ideal)))
    boundaries.append(tail_frames)
    intro = round((args.end - args.start) * FPS)
    parts = [(args.start, intro)] + list(zip(args.shots, np.diff(boundaries)))
    for i, (start, frames) in enumerate(parts):
        part = temp / f"part{i}.mp4"
        ffmpeg("-ss", start, "-i", args.source, "-vf", "fps=25,scale=540:-2,setsar=1",
               "-frames:v", frames, "-an", "-c:v", "libx264", "-pix_fmt", "yuv420p", part)
        actual = subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0",
            "-show_entries", "stream=nb_frames", "-of", "csv=p=0", str(part)], text=True)
        if int(actual.strip()) != frames:
            raise ValueError(f"Not enough source frames at {start}s")
    manifest = temp / "parts.txt"
    manifest.write_text("".join(f"file 'part{i}.mp4'\n" for i in range(len(parts))), encoding="ascii")
    ffmpeg("-f", "concat", "-safe", 0, "-i", manifest, "-ss", args.start, "-t", intro/FPS,
           "-i", args.source, "-ss", args.hook + hook, "-t", tail_frames/FPS, "-i", args.track,
           "-filter_complex", "[1:a:0][2:a:0]concat=n=2:v=0:a=1[a]", "-map", "0:v", "-map", "[a]",
           "-c:v", "copy", "-c:a", "aac", args.output)
    return dict(dialogue_source_start=args.start, hook_in_output=intro/FPS,
                hook_in_track=args.hook+hook, shots=[dict(source_start=s, output_start=(intro+b)/FPS)
                    for s, b in zip(args.shots, boundaries)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    face = sub.add_parser("reaction")
    face.add_argument("--model", type=Path, required=True, help="OpenCV YuNet ONNX")
    beat = sub.add_parser("music")
    beat.add_argument("--track", type=Path, required=True)
    beat.add_argument("--hook", type=float, required=True, help="Approximate start of the musical hook")
    beat.add_argument("--tail", type=float, required=True, help="Duration of the ending montage")
    beat.add_argument("--shots", type=float, nargs="+", required=True, help="Start times in source video")
    for cmd in (face, beat):
        cmd.add_argument("source", type=Path)
        cmd.add_argument("output", type=Path)
        cmd.add_argument("--start", type=float, required=True)
        cmd.add_argument("--end", type=float, required=True, help="End of reaction or end of final dialogue")
    args = parser.parse_args()
    inputs = [args.source, args.model if args.mode == "reaction" else args.track]
    if args.start < 0 or args.end <= args.start or args.output.resolve() in [p.resolve() for p in inputs]:
        parser.error("Invalid source interval or output path")
    if args.mode == "music" and (args.hook < 0 or min(args.shots) < 0 or args.tail * FPS < 5*len(args.shots)):
        parser.error("Invalid music or shot timing")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        result = (reaction if args.mode == "reaction" else music)(args, Path(work))
    args.output.with_suffix(".json").write_text(json.dumps(result, indent=2), encoding="utf8")


if __name__ == "__main__":
    main()
