"""Диаризация (кто говорил когда) — sherpa-onnx, БЕЗ HuggingFace токена.

Модели (публичные ссылки GitHub Releases, никакой регистрации):
  pyannote/segmentation-3.0 (ONNX)  — нарезка на локальные «кто говорит»
  3D-Speaker eres2net (ONNX)        — эмбеддинги голоса

Выход — ТОТ ЖЕ формат, что у scripts/diarize.py (pyannote), чтобы
DiarizeRunner::parseJson в C++ съел без изменений:
  [{"start": 1.23, "end": 4.56, "speaker": "SPEAKER_00"}, ...]

Запуск:
  python diarize_sherpa.py --wav a.wav --out diar.json [--num-speakers 5]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpu_env  # noqa: E402,F401

SR = 16000
DEFAULT_SEG = r"D:\ai_models\audio\sherpa-onnx-pyannote-segmentation-3-0\model.onnx"
DEFAULT_EMB = (r"D:\ai_models\audio"
               r"\3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx")


def read_wav(path):
    import numpy as np
    import soundfile as sf

    data, sr = sf.read(path, dtype="float32", always_2d=True)
    audio = data.mean(axis=1) if data.shape[1] > 1 else data[:, 0]
    if sr != SR:
        n = int(round(len(audio) * SR / sr))
        audio = np.interp(np.linspace(0, len(audio) - 1, n),
                          np.arange(len(audio)), audio).astype(np.float32)
    return np.ascontiguousarray(audio.astype("float32"))


def build(seg_model, emb_model, num_speakers=-1, threshold=0.9, threads=2, provider="cpu"):
    import sherpa_onnx as so

    cfg = so.OfflineSpeakerDiarizationConfig(
        segmentation=so.OfflineSpeakerSegmentationModelConfig(
            pyannote=so.OfflineSpeakerSegmentationPyannoteModelConfig(model=seg_model),
            num_threads=threads,
            provider=provider,
        ),
        embedding=so.SpeakerEmbeddingExtractorConfig(
            model=emb_model, num_threads=threads, provider=provider),
        clustering=so.FastClusteringConfig(num_clusters=num_speakers, threshold=threshold),
        min_duration_on=0.3,
        min_duration_off=0.5,
    )
    if not cfg.validate():
        raise RuntimeError("невалидный конфиг диаризации — проверь пути к моделям")
    return so.OfflineSpeakerDiarization(cfg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seg-model", default=DEFAULT_SEG)
    ap.add_argument("--emb-model", default=DEFAULT_EMB)
    ap.add_argument("--num-speakers", type=int, default=-1,
                    help="-1 = определить автоматически")
    ap.add_argument("--threshold", type=float, default=0.9,
                    help="меньше = больше спикеров (0.9 откалибровано на 4-спикерном семпле)")
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--device", default="cpu", choices=["cpu", "cuda"])
    args = ap.parse_args()

    t0 = time.time()
    audio = read_wav(args.wav)
    total = len(audio) / SR
    print(f"[diarize_sherpa] {total:.1f}с аудио, {args.device}", file=sys.stderr)

    sd = build(args.seg_model, args.emb_model, args.num_speakers,
               args.threshold, args.threads, args.device)

    t1 = time.time()
    result = sd.process(audio)
    infer_s = time.time() - t1

    segs = []
    for s in result.sort_by_start_time():
        segs.append({"start": round(float(s.start), 3),
                     "end": round(float(s.end), 3),
                     "speaker": f"SPEAKER_{int(s.speaker):02d}"})

    speakers = sorted({s["speaker"] for s in segs})
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(segs, f, ensure_ascii=False, indent=1)

    print(f"[diarize_sherpa] {infer_s:.1f}с = x{total/infer_s:.1f}; "
          f"{len(segs)} сегментов, {len(speakers)} спикеров "
          f"({', '.join(speakers) if speakers else '—'})", file=sys.stderr)


if __name__ == "__main__":
    main()