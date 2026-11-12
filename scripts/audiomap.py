"""Карта аудио: где речь, где музыка, где тишина.

Движки:
  ced/zipformer — sherpa-onnx AudioTagging (AudioSet-теггер, ONNX, CPU)
  ina           — inaSpeechSegmenter (CNN, TensorFlow) — точнее, но тяжёлый

Выход audiomap.json:
{
  "engine": "...", "audio_sec": 7763.1, "step": 2.0, "window": 10.0,
  "segments": [{"label":"speech|music|noise|silence","start":..,"end":..}, ...],
  "speech_sec": 4200.1, "music_sec": 2100.5
}

Запуск:
  python audiomap.py --wav a.wav --out audiomap.json --engine zipformer
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpu_env  # noqa: E402,F401

import numpy as np  # noqa: E402

SR = 16000

# метки AudioSet, которые считаем музыкой / речью
MUSIC_LABELS = {
    "music", "musical instrument", "singing", "vocal music", "a capella",
    "choir", "piano", "guitar", "drum", "percussion", "synthesizer", "keyboard (musical)",
    "string instrument", "bowed string instrument", "plucked string instrument",
    "brass instrument", "woodwind instrument", "electronic music", "ambient music",
    "hip hop music", "pop music", "rock music", "jazz", "classical music",
    "heavy metal", "country", "reggae", "blues", "folk music", "background music",
    "soundtrack music", "theme music", "jingle (music)", "lullaby", "beatboxing",
    "rapping", "humming", "whistling", "child singing", "female singing", "male singing",
}
SPEECH_LABELS = {
    "speech", "conversation", "narration, monologue", "male speech, man speaking",
    "female speech, woman speaking", "child speech, kid speaking", "speech synthesizer",
    "shout", "whispering", "babbling", "speech bubble",
}
NOISE_LABELS = {
    "silence", "noise", "environmental noise", "white noise", "pink noise",
    "static", "hum", "hiss", "crackle",
}


def read_wav(path):
    import soundfile as sf

    data, sr = sf.read(path, dtype="float32", always_2d=True)
    audio = data[:, 0]
    if sr != SR:
        n = int(round(len(audio) * SR / sr))
        audio = np.interp(np.linspace(0, len(audio) - 1, n),
                          np.arange(len(audio)), audio).astype(np.float32)
    return np.ascontiguousarray(audio)


# ---------------------------------------------------------------- zipformer/ced

def map_zipformer(audio, model_dir, window=10.0, step=2.0, threshold=0.3):
    import sherpa_onnx

    model = os.path.join(model_dir, "model.int8.onnx")
    labels = os.path.join(model_dir, "class_labels_indices.csv")
    if not os.path.exists(model):
        model = os.path.join(model_dir, "model.onnx")

    cfg = sherpa_onnx.AudioTaggingConfig()
    cfg.model.zipformer.model = model
    cfg.model.num_threads = 2
    cfg.model.provider = "cpu"
    cfg.labels = labels
    cfg.top_k = 8
    if not cfg.validate():
        raise RuntimeError("невалидный конфиг AudioTagging")
    tagger = sherpa_onnx.AudioTagging(cfg)

    win = int(window * SR)
    hop = int(step * SR)
    total = len(audio)
    votes = []  # (start, label, score)
    pos = 0
    n = 0
    while pos + win <= total:
        chunk = audio[pos: pos + win]
        stream = tagger.create_stream()
        stream.accept_waveform(sample_rate=SR, waveform=chunk)
        events = tagger.compute(stream, top_k=8)
        label, score = _classify(events, threshold)
        votes.append((pos / SR, label, round(float(score), 3)))
        pos += hop
        n += 1
        if n % 50 == 0:
            print(f"  ... {pos/SR:.0f}с", file=sys.stderr)
    # хвост
    if pos < total - int(1.0 * SR):
        chunk = audio[max(0, total - win):]
        stream = tagger.create_stream()
        stream.accept_waveform(sample_rate=SR, waveform=chunk)
        events = tagger.compute(stream, top_k=8)
        label, score = _classify(events, threshold)
        votes.append((pos / SR, label, round(float(score), 3)))
    return votes, window, step


def _classify(events, threshold):
    """Суммируем вероятности по группам: музыка / речь / шум."""
    music = speech = noise = 0.0
    for e in events:
        name = (e.name or "").strip().lower()
        p = float(e.prob)
        if name in MUSIC_LABELS or any(k in name for k in ("music", "singing", "instrument")):
            music = max(music, p)
        elif name in SPEECH_LABELS or "speech" in name or "conversation" in name:
            speech = max(speech, p)
        elif name in NOISE_LABELS or "noise" in name or "silence" in name:
            noise = max(noise, p)
    best = max((music, "music"), (speech, "speech"), (noise, "noise"))
    if best[0] < threshold:
        # ни то ни другое уверенно: если музыка и речь близки — это речь под музыку,
        # для ASR опаснее, поэтому помечаем music
        if music > 0.15 and abs(music - speech) < 0.2:
            return "music", music
        return ("speech" if speech >= music and speech >= noise else
                "music" if music >= noise else "noise"), best[0]
    return best[1], best[0]


# ---------------------------------------------------------------- ina

def map_ina(audio, tmp_dir, detect_gender=False):
    from inaSpeechSegmenter import Segmenter

    import soundfile as sf

    os.makedirs(tmp_dir, exist_ok=True)
    p = os.path.join(tmp_dir, "_ina_input.wav")
    sf.write(p, audio, SR, subtype="PCM_16")
    seg = Segmenter(vad_engine="smn", detect_gender=detect_gender)
    raw = seg(p)
    out = []
    for label, start, end in raw:
        if label in ("male", "female", "speech"):
            lab = "speech"
        elif label == "music":
            lab = "music"
        elif label in ("noise",):
            lab = "noise"
        else:
            lab = "silence"
        out.append((round(start, 2), lab, 1.0))
        out[-1] = out[-1] + (round(end, 2),)
    return [(s, l, sc, e) for (s, l, sc, e) in out], 10.0, 10.0


# ---------------------------------------------------------------- слияние

def smooth_and_merge(votes, window, step, total_sec, min_len=3.0):
    """Голоса -> сегменты. Медианное сглаживание по 5 окнам, потом слияние."""
    if not votes:
        return []
    # votes: (start, label, score) или (start, label, score, end)
    is_range = len(votes[0]) == 4
    if is_range:
        segs = [{"start": v[0], "end": v[3], "label": v[1], "score": v[2]} for v in votes]
    else:
        labs = [v[1] for v in votes]
        sm = []
        for i in range(len(labs)):
            lo = max(0, i - 2)
            hi = min(len(labs), i + 3)
            w = labs[lo:hi]
            sm.append(max(set(w), key=w.count))
        segs = []
        for i, v in enumerate(votes):
            start = v[0]
            end = votes[i + 1][0] if i + 1 < len(votes) else min(total_sec, start + window)
            segs.append({"start": round(start, 2), "end": round(end, 2),
                         "label": sm[i], "score": v[2]})

    merged = []
    for s in segs:
        if merged and merged[-1]["label"] == s["label"]:
            merged[-1]["end"] = s["end"]
        else:
            merged.append(dict(s))
    # короткие вставки -> в соседа
    changed = True
    while changed:
        changed = False
        for i, s in enumerate(merged):
            if s["end"] - s["start"] < min_len and len(merged) > 1:
                if i == 0:
                    merged[1]["start"] = s["start"]
                elif i == len(merged) - 1:
                    merged[i - 1]["end"] = s["end"]
                else:
                    prev, nxt = merged[i - 1], merged[i + 1]
                    if prev["label"] == nxt["label"]:
                        prev["end"] = nxt["end"]
                        merged.pop(i + 1)
                    else:
                        # отдаём тому, кто длиннее рядом
                        longer = prev if (prev["end"] - prev["start"]) >= (nxt["end"] - nxt["start"]) else nxt
                        longer["end" if longer is prev else "start"] = (
                            s["end"] if longer is prev else s["start"])
                merged.pop(i)
                changed = True
                break
    return merged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--engine", default="zipformer", choices=["zipformer", "ina"])
    ap.add_argument("--model-dir",
                    default=r"D:\ai_models\audio\sherpa-onnx-zipformer-small-audio-tagging-2024-04-15")
    ap.add_argument("--window", type=float, default=10.0)
    ap.add_argument("--step", type=float, default=2.0)
    ap.add_argument("--threshold", type=float, default=0.3)
    args = ap.parse_args()

    t0 = time.time()
    audio = read_wav(args.wav)
    total = len(audio) / SR
    print(f"[{args.engine}] {total:.1f}с аудио", file=sys.stderr)

    t1 = time.time()
    if args.engine == "ina":
        votes, window, step = map_ina(audio, os.path.dirname(args.out))
    else:
        votes, window, step = map_zipformer(audio, args.model_dir, args.window, args.step,
                                            args.threshold)
    infer_s = time.time() - t1

    segs = smooth_and_merge(votes, window, step, total)
    speech = sum(s["end"] - s["start"] for s in segs if s["label"] == "speech")
    music = sum(s["end"] - s["start"] for s in segs if s["label"] == "music")

    out = {
        "engine": args.engine,
        "audio_sec": round(total, 2),
        "window": window,
        "step": step,
        "infer_sec": round(infer_s, 2),
        "x_realtime": round(total / infer_s, 1) if infer_s > 0 else None,
        "speech_sec": round(speech, 1),
        "music_sec": round(music, 1),
        "segments": segs,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print(f"[{args.engine}] {infer_s:.1f}с = x{out['x_realtime']}; "
          f"речь {speech:.0f}с ({100*speech/total:.0f}%), музыка {music:.0f}с "
          f"({100*music/total:.0f}%), сегментов {len(segs)}", file=sys.stderr)


if __name__ == "__main__":
    main()
