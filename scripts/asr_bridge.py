"""Единый мост ASR для Пересказа 3.0: любой движок -> words.json.

Все движки имеют разные ограничения и форматы, поэтому здесь:
  * нарезка на чанки по тихим местам (GigaAM не умеет >25с, остальным тоже
    полезно: изоляция ошибок + стабильность против зацикливания),
  * прогон каждого чанка отдельно,
  * сборка слов с абсолютными таймкодами,
  * метрики качества на чанк (logprob / no_speech / повтор предыдущего).

Движки:
  whispercpp      — внешний whisper-cli.exe (путь через --whisper-cli), word-level SRT
  faster-whisper  — CTranslate2, CUDA/CPU, антифантомные фильтры из коробки
  gigaam-v3-ctc   — GigaAM v3 CTC через onnx-asr (лучший русский)
  gigaam-v3-e2e   — GigaAM v3 e2e RNNT (с пунктуацией)
  parakeet-v3     — NVIDIA Parakeet TDT 0.6B v3 (устойчив к музыке, ревизор)

Выход words.json:
{
  "engine": "...", "audio_sec": 7763.1, "chunk_sec": 20,
  "words": [{"w": "Привет", "t": 12.34, "e": 12.71, "chunk": 3, "lp": -0.21}, ...],
  "chunks": [{"i":0,"start":0.0,"end":19.8,"n_words":12,"text":"...","lp":-0.3}, ...]
}

Запуск:
  python asr_bridge.py --engine gigaam-v3-ctc --wav a.wav --out words.json
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpu_env  # noqa: E402,F401  — DLL cuDNN из pip до onnxruntime/ctranslate2

import numpy as np  # noqa: E402

SR = 16000


# ---------------------------------------------------------------- нарезка

def read_wav(path: str) -> np.ndarray:
    import soundfile as sf

    data, sr = sf.read(path, dtype="float32", always_2d=True)
    audio = data[:, 0]
    if sr != SR:
        # линейный ресемплинг: точности хватает, ffmpeg уже отдаёт 16 кГц
        n = int(round(len(audio) * SR / sr))
        idx = np.linspace(0, len(audio) - 1, n)
        audio = np.interp(idx, np.arange(len(audio)), audio).astype(np.float32)
    return np.ascontiguousarray(audio)


def _quiet_cut(audio: np.ndarray, target: int, search: int) -> int:
    """Ближайшая к target точка с минимальной энергией — чтобы не резать слово."""
    lo = max(0, target - search)
    hi = min(len(audio), target + search)
    if hi - lo < 400:
        return target
    seg = audio[lo:hi]
    win = 320  # 20 мс
    n = len(seg) // win
    if n < 2:
        return target
    energy = np.sqrt(np.mean(seg[: n * win].reshape(n, win) ** 2, axis=1) + 1e-12)
    # ищем тишину в средней части окна поиска, чтобы не уехать в край
    third = max(1, n // 3)
    mid = energy[third: n - third] if n - 2 * third > 2 else energy
    k = int(np.argmin(mid)) + (third if mid is not energy else 0)
    return lo + k * win


def plan_chunks(audio: np.ndarray, chunk_sec: float, max_sec: float = 25.0):
    """Границы чанков: ~chunk_sec, но не больше max_sec (лимит GigaAM)."""
    if chunk_sec <= 0 or max_sec <= 0:
        raise ValueError("chunk length must be positive")
    target = max(1, int(min(chunk_sec, max_sec) * SR))
    maximum = max(1, int(max_sec * SR))
    search = int(1.5 * SR)
    out = []
    start = 0
    while start < len(audio):
        desired = min(len(audio), start + target)
        cut = desired if desired == len(audio) else _quiet_cut(audio, desired, search)
        cut = min(len(audio), start + maximum, max(start + 1, cut))
        if cut <= start + min(SR, target // 2) and desired > cut:
            cut = desired
        out.append((start / SR, cut / SR))
        start = cut
    return out


# ---------------------------------------------------------------- движки

def _words_from_tokens(tokens, timestamps, logprobs=None, offset=0.0, chunk=0):
    """Токены (с '▁'/пробелами) -> слова с реалистичными концами.

    onnx-asr отдаёт только НАЧАЛО каждого токена. Конец слова = начало его
    последнего токена + медианная длительность токена в чанке (иначе, если
    брать начало следующего слова, пауза в 3 секунды превращается в «слово
    длиной 3 секунды» и снаппинг уезжает в тишину).
    """
    tokens = tokens or []
    tss = timestamps or []

    # медианная длительность токена по чанку
    deltas = [tss[i + 1] - tss[i] for i in range(len(tss) - 1) if tss[i + 1] > tss[i]]
    med = float(np.median(deltas)) if deltas else 0.06

    words = []
    cur, start, last_ts, lps = "", None, None, []

    def flush():
        nonlocal cur, start, last_ts, lps
        if cur:
            end = (last_ts + med) if last_ts is not None else (start + med)
            lp = float(np.mean(lps)) if lps else None
            words.append({"w": cur, "t": round(start + offset, 2),
                          "e": round(end + offset, 2), "chunk": chunk, "lp": lp})
        cur, start, last_ts, lps = "", None, None, []

    for i, tok in enumerate(tokens):
        ts = tss[i] if i < len(tss) else None
        lp = logprobs[i] if logprobs and i < len(logprobs) else None
        is_break = tok.startswith(" ") or tok.startswith("▁")
        piece = tok.strip().lstrip("▁")
        if is_break and cur:
            flush()
        if piece:
            if start is None:
                start = ts if ts is not None else 0.0
            if ts is not None:
                last_ts = ts
            cur += piece
            if lp is not None:
                lps.append(lp)
    flush()
    return words


def run_onnx_asr(audio: np.ndarray, chunks, model_name: str):
    import onnx_asr

    model = onnx_asr.load_model(model_name,
                                providers=["CUDAExecutionProvider", "CPUExecutionProvider"])
    model = model.with_timestamps()

    words, chunk_meta = [], []
    for i, (a, b) in enumerate(chunks):
        seg = audio[int(a * SR): int(b * SR)]
        res = model.recognize(seg, sample_rate=SR)
        ws = _words_from_tokens(res.tokens, res.timestamps,
                                getattr(res, "logprobs", None), offset=a, chunk=i)
        # не выпускаем слово за пределы чанка
        for w in ws:
            w["e"] = round(min(w["e"], b), 2)
        words.extend(ws)
        chunk_meta.append({"i": i, "start": round(a, 2), "end": round(b, 2),
                           "n_words": len(ws), "text": (res.text or "").strip()})
    return words, chunk_meta


def run_faster_whisper(audio: np.ndarray, chunks, device="cuda", compute_type="float16",
                       model_size="large-v3", language="ru"):
    from faster_whisper import WhisperModel

    try:
        model = WhisperModel(model_size, device=device, compute_type=compute_type)
        used = f"{device}/{compute_type}"
    except Exception as e:
        print(f"[faster-whisper] {device}/{compute_type} не завелась: {e}", file=sys.stderr)
        model = WhisperModel(model_size, device="cpu", compute_type="int8")
        used = "cpu/int8"
    print(f"[faster-whisper] устройство: {used}", file=sys.stderr)

    words, chunk_meta = [], []
    for i, (a, b) in enumerate(chunks):
        seg = audio[int(a * SR): int(b * SR)]
        segments, _ = model.transcribe(
            seg,
            language=language,
            word_timestamps=True,
            condition_on_previous_text=False,
            vad_filter=True,
            compression_ratio_threshold=2.4,
            log_prob_threshold=-1.0,
            no_speech_threshold=0.6,
            hallucination_silence_threshold=2.0,
        )
        texts, lps = [], []
        for s in segments:
            texts.append(s.text.strip())
            lps.append(s.avg_logprob)
            for w in (s.words or []):
                words.append({"w": w.word.strip(), "t": round(w.start + a, 2),
                              "e": round(w.end + a, 2), "chunk": i, "lp": round(s.avg_logprob, 3)})
        chunk_meta.append({"i": i, "start": round(a, 2), "end": round(b, 2),
                           "n_words": sum(1 for w in words if w["chunk"] == i),
                           "text": " ".join(texts),
                           "lp": round(float(np.mean(lps)), 3) if lps else None})
    return words, chunk_meta


_SRT_TIME = re.compile(r"(\d+):(\d+):(\d+)[,.](\d+)\s*-->\s*(\d+):(\d+):(\d+)[,.](\d+)")


def run_whispercpp(wav_path, chunks, cli, model, vad_model=None, extra=None, language="ru"):
    """Каждый чанк — отдельный процесс whisper-cli (изоляция ошибок)."""
    import subprocess
    import tempfile

    words, chunk_meta = [], []
    tmp = tempfile.mkdtemp(prefix="asrchunk_")
    try:
        import soundfile as sf
        full = read_wav(wav_path)
        for i, (a, b) in enumerate(chunks):
            seg_path = os.path.join(tmp, f"c{i:04d}.wav")
            sf.write(seg_path, full[int(a * SR): int(b * SR)], SR, subtype="PCM_16")
            base = os.path.join(tmp, f"c{i:04d}")
            args = [cli, "-m", model, "-f", seg_path, "-osrt", "-of", base,
                    "-l", language, "-ml", "1", "-sow", "-mc", "0", "-sns", "-nf"]
            if vad_model:
                args += ["--vad", "-vm", vad_model]
            if extra:
                args += extra
            r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                               errors="replace")
            srt = base + ".srt"
            if r.returncode != 0 or not os.path.exists(srt):
                raise RuntimeError(f"whisper chunk {i} [{a:.2f}-{b:.2f}]: "
                                   f"exit {r.returncode}; {r.stderr[-600:]}")
            texts = []
            if os.path.exists(srt):
                for blk in open(srt, encoding="utf-8", errors="replace").read().split("\n\n"):
                    lines = [l for l in blk.splitlines() if l.strip()]
                    tc = next((l for l in lines if "-->" in l), None)
                    if not tc:
                        continue
                    m = _SRT_TIME.search(tc)
                    if not m:
                        continue
                    g = [int(x) for x in m.groups()]
                    st = g[0] * 3600 + g[1] * 60 + g[2] + g[3] / 1000.0
                    en = g[4] * 3600 + g[5] * 60 + g[6] + g[7] / 1000.0
                    txt = " ".join(lines[lines.index(tc) + 1:]).strip()
                    if not txt:
                        continue
                    texts.append(txt)
                    words.append({"w": txt, "t": round(st + a, 2), "e": round(en + a, 2),
                                  "chunk": i, "lp": None})
            chunk_meta.append({"i": i, "start": round(a, 2), "end": round(b, 2),
                               "n_words": len(texts), "text": " ".join(texts),
                               "rc": r.returncode})
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    return words, chunk_meta


# ---------------------------------------------------------------- качество

def quality_report(words, chunks, audio_sec):
    texts = [w["w"] for w in words]
    n = len(texts)
    uniq = len(set(texts)) / n if n else 1.0
    lps = [w["lp"] for w in words if w.get("lp") is not None]
    stretched = sum(1 for w in words if w["e"] - w["t"] > 2.0)
    # повтор целого текста чанка предыдущего (признак петли)
    dup = 0
    prev = None
    for c in chunks:
        t = (c.get("text") or "").strip()
        if t and t == prev and len(t) > 20:
            dup += 1
        prev = t or prev
    return {
        "n_words": n,
        "unique_ratio": round(uniq, 3),
        "avg_logprob": round(float(np.mean(lps)), 3) if lps else None,
        "stretched_words": stretched,
        "duplicate_chunks": dup,
        "n_chunks": len(chunks),
        "words_per_min": round(n / (audio_sec / 60.0), 1) if audio_sec else None,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True,
                    choices=["whispercpp", "faster-whisper", "gigaam-v3-ctc",
                             "gigaam-v3-e2e", "parakeet-v3"])
    ap.add_argument("--wav", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--chunk", type=float, default=20.0)
    ap.add_argument("--language", default="ru")
    ap.add_argument("--whisper-cli", default=r"D:\whisper.cpp\build\bin\Release\whisper-cli.exe")
    ap.add_argument("--whisper-model", default=r"D:\whisper_models\ggml-large-v3.bin")
    ap.add_argument("--vad-model", default=r"D:\whisper_models\ggml-silero-v6.2.0.bin")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--compute-type", default="float16")
    args = ap.parse_args()

    t0 = time.time()
    audio = read_wav(args.wav)
    audio_sec = len(audio) / SR
    chunks = plan_chunks(audio, args.chunk)
    print(f"[{args.engine}] {audio_sec:.1f}с аудио -> {len(chunks)} чанков "
          f"(~{args.chunk}с), нарезка {time.time()-t0:.1f}с", file=sys.stderr)

    t0 = time.time()
    if args.engine == "whispercpp":
        words, cmeta = run_whispercpp(args.wav, chunks, args.whisper_cli,
                                      args.whisper_model, args.vad_model, language=args.language)
    elif args.engine == "faster-whisper":
        words, cmeta = run_faster_whisper(audio, chunks, args.device, args.compute_type,
                                         language=args.language)
    elif args.engine == "parakeet-v3":
        words, cmeta = run_onnx_asr(audio, chunks, "nemo-parakeet-tdt-0.6b-v3")
    elif args.engine == "gigaam-v3-e2e":
        words, cmeta = run_onnx_asr(audio, chunks, "gigaam-v3-e2e-rnnt")
    else:
        words, cmeta = run_onnx_asr(audio, chunks, "gigaam-v3-ctc")
    infer_s = time.time() - t0

    report = quality_report(words, cmeta, audio_sec)
    out = {
        "engine": args.engine,
        "language": args.language,
        "audio_sec": round(audio_sec, 2),
        "chunk_sec": args.chunk,
        "infer_sec": round(infer_s, 2),
        "rtf": round(infer_s / audio_sec, 3) if audio_sec else None,
        "x_realtime": round(audio_sec / infer_s, 1) if infer_s > 0 else None,
        "quality": report,
        "chunks": cmeta,
        "words": words,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print(f"[{args.engine}] инференс {infer_s:.1f}с = x{out['x_realtime']} реалтайма; "
          f"слов {report['n_words']}, уникальных {report['unique_ratio']}", file=sys.stderr)
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
