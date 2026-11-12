"""Ревизор транскрипта: второй движок проверяет ВЕСЬ фильм чанками.

Пользователь: «проверку можно сделать дороже (весь фильм сравнивать чанками по 30с)».
Так и делаем: основной транскрипт есть, второй движок прогоняет каждый чанк
независимо, потом пословное сравнение.

Расхождение двух движков само по себе помечает чанк suspicious.
Остальные признаки учитываются в сочетании (>=2):
  * agreement < min_agree (два движка сильно расходятся)
  * попадает в 'music' по карте аудио
  * avg_logprob основного движка ниже порога
  * плотность слов < 5/мин (в зоне почти нет речи, а слова есть — подозрительно)

Спорные окна повторно распознаются короткими чанками. Исправление допускается,
если независимый ревизор и повторный проход согласны; прочие расхождения
остаются помеченными. Результат corrections применяется при общей сшивке.

Запуск:
  python review.py --wav film.wav --words words.json --engine parakeet-v3 \
      --out review.json [--chunk 30] [--audiomap audiomap.json]
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpu_env  # noqa: E402,F401
import asr_bridge  # noqa: E402

import numpy as np  # noqa: E402

SR = 16000


def norm(w):
    return re.sub(r"[^\wа-яё]", "", (w or "").lower(), flags=re.I)


def words_between(words, a, b):
    return [w for w in words if a <= (w["t"] + w.get("e", w["t"])) / 2.0 < b]


def zone_label(zones, a, b):
    """Метка зоны, занимающей большую часть интервала."""
    acc = {}
    for name, s, e in zones:
        ov = min(b, e) - max(a, s)
        if ov > 0:
            acc[name] = acc.get(name, 0.0) + ov
    if not acc:
        return None
    return max(acc, key=acc.get)


def run_engine(wav, audio, chunks, engine, language="ru", whisper_cli=None,
               whisper_model=None, vad_model=None):
    """Прогоняет второй движок по готовым границам (те же, что у основного)."""
    if engine == "parakeet-v3":
        return asr_bridge.run_onnx_asr(audio, chunks, "nemo-parakeet-tdt-0.6b-v3")
    if engine == "gigaam-v3-ctc":
        return asr_bridge.run_onnx_asr(audio, chunks, "gigaam-v3-ctc")
    if engine == "gigaam-v3-e2e":
        return asr_bridge.run_onnx_asr(audio, chunks, "gigaam-v3-e2e-rnnt")
    if engine == "faster-whisper":
        return asr_bridge.run_faster_whisper(audio, chunks,
                                             device="cuda", compute_type="float16", language=language)
    if engine == "whispercpp":
        return asr_bridge.run_whispercpp(
            wav, chunks,
            whisper_cli, whisper_model, vad_model, language=language)
    raise ValueError("неизвестный движок: " + engine)


def agreement(left, right):
    a = [norm(w["w"]) for w in left if norm(w["w"])]
    b = [norm(w["w"]) for w in right if norm(w["w"])]
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio() if a or b else 1.0


def consensus_corrections(chunks, main_words, review_words, retry_words, min_agree):
    """Change disputed text only when the independent review and local retry agree.

    Disagreement alone does not identify which recognizer is right. Unresolved
    chunks remain flagged; music by itself never authorizes deleting dialogue.
    """
    corrections = []
    for c in chunks:
        if not c["suspicious"]:
            continue
        a, b = c["start"], c["end"]
        main = words_between(main_words, a, b)
        review = words_between(review_words, a, b)
        retry = words_between(retry_words, a, b)
        supported = bool(review and retry) and agreement(review, retry) >= 0.85
        empty_supported = (not review and not retry and bool(main)
                           and (c["zone"] in ("music", "noise", "silence")
                                or "низкий logprob" in c["reasons"]))
        if ((supported and agreement(main, retry) < min_agree) or empty_supported):
            corrected = [dict(w, t=max(a, w["t"]), e=min(b, w["e"])) for w in retry]
            corrected = [w for w in corrected if w["e"] > w["t"]]
            corrections.append({"start": a, "end": b, "words": corrected,
                                "reason": "review and local retry agree"})
            c["suspicious"] = False
            c["repaired"] = True
    return corrections


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", required=True)
    ap.add_argument("--words", required=True, help="words.json основного движка")
    ap.add_argument("--engine", default="parakeet-v3")
    ap.add_argument("--out", required=True)
    ap.add_argument("--chunk", type=float, default=30.0)
    ap.add_argument("--audiomap", default=None)
    ap.add_argument("--min-agree", type=float, default=0.60)
    ap.add_argument("--min-lp", type=float, default=-0.55)
    ap.add_argument("--language", default="ru")
    ap.add_argument("--whisper-cli", default=r"D:\whisper.cpp\build\bin\Release\whisper-cli.exe")
    ap.add_argument("--whisper-model", default=r"D:\whisper_models\ggml-large-v3.bin")
    ap.add_argument("--vad-model", default=r"D:\whisper_models\ggml-silero-v6.2.0.bin")
    args = ap.parse_args()

    main_data = json.load(open(args.words, encoding="utf-8"))
    audio = asr_bridge.read_wav(args.wav)
    total = len(audio) / SR

    engine = args.engine
    if args.language != "ru" and engine.startswith("gigaam"):
        engine = "parakeet-v3"
    if engine == main_data.get("engine"):
        engine = "faster-whisper" if engine != "faster-whisper" else "parakeet-v3"
        print(f"[review] для независимой проверки выбран {engine}", file=sys.stderr)
    chunk_sec = min(args.chunk, 25.0) if engine.startswith("gigaam") else args.chunk
    if chunk_sec <= 0:
        ap.error("--chunk must be positive")
    # Compare the same absolute windows, respecting the review engine's limit.
    chunks = []
    pos = 0.0
    while pos < total:
        chunks.append((pos, min(total, pos + chunk_sec)))
        pos += chunk_sec

    zones = []
    if args.audiomap and os.path.exists(args.audiomap):
        am = json.load(open(args.audiomap, encoding="utf-8"))
        zones = [(s["label"], s["start"], s["end"]) for s in am.get("segments", [])]

    print(f"[review] второй движок {args.engine} по {len(chunks)} чанкам "
          f"({total:.0f}с)...", file=sys.stderr)
    def recognize(intervals, selected_engine):
        return run_engine(args.wav, audio, intervals, selected_engine, args.language,
                          args.whisper_cli, args.whisper_model, args.vad_model)[0]

    rev_words = recognize(chunks, engine)
    print(f"[review] второй движок дал {len(rev_words)} слов", file=sys.stderr)

    out_chunks = []
    for i, (a, b) in enumerate(chunks):
        w1 = [norm(w["w"]) for w in words_between(main_data["words"], a, b)]
        w2 = [norm(w["w"]) for w in rev_words if a <= (w["t"] + w.get("e", w["t"])) / 2.0 < b]
        w1 = [x for x in w1 if x]
        w2 = [x for x in w2 if x]

        agree = difflib.SequenceMatcher(None, w1, w2, autojunk=False).ratio() if (w1 or w2) else 1.0
        lps = [w["lp"] for w in words_between(main_data["words"], a, b)
               if w.get("lp") is not None]
        lp = float(np.mean(lps)) if lps else None
        density = len(w1) / ((b - a) / 60.0) if b > a else 0
        label = zone_label(zones, a, b)

        reasons = []
        if agree < args.min_agree:
            reasons.append("расхождение %.0f%%" % (100 * (1 - agree)))
        if label == "music":
            reasons.append("музыка")
        if lp is not None and lp < args.min_lp:
            reasons.append("низкий logprob")
        if density < 5.0 and w1:
            reasons.append("почти нет речи, а слова есть")

        out_chunks.append({
            "i": i,
            "start": round(a, 2),
            "end": round(b, 2),
            "n_words_main": len(w1),
            "n_words_review": len(w2),
            "agreement": round(agree, 3),
            "avg_logprob": round(lp, 3) if lp is not None else None,
            "words_per_min": round(density, 1),
            "zone": label,
            "reasons": reasons,
            "suspicious": agree < args.min_agree or len(reasons) >= 2,
            # текст обоих — чтобы можно было глазами посмотреть
            "text_main": " ".join(norm(w["w"]) for w in words_between(main_data["words"], a, b)),
            "text_review": " ".join(norm(w["w"]) for w in rev_words
                                    if a <= (w["t"] + w.get("e", w["t"])) / 2.0 < b),
        })

    disputed = [c for c in out_chunks if c["suspicious"]]
    corrections = []
    if disputed:
        retry_chunks = []
        for c in disputed:
            a, b = c["start"], c["end"]
            part = audio[int(a * SR):int(b * SR)]
            retry_chunks.extend((a + x, a + y) for x, y in asr_bridge.plan_chunks(part, 10.0))
        print(f"[review] повторно распознаю спорные окна: {len(retry_chunks)} коротких чанков",
              file=sys.stderr)
        retry_words = recognize(retry_chunks, main_data.get("engine", "faster-whisper"))
        corrections = consensus_corrections(out_chunks, main_data["words"], rev_words,
                                             retry_words, args.min_agree)
    susp = [c for c in out_chunks if c["suspicious"]]
    agree_avg = float(np.mean([c["agreement"] for c in out_chunks])) if out_chunks else 1.0

    out = {
        "engine_main": main_data.get("engine"),
        "engine_review": engine,
        "chunk_sec": chunk_sec,
        "language": args.language,
        "corrections": corrections,
        "n_repaired": len(corrections),
        "audio_sec": round(total, 2),
        "n_chunks": len(out_chunks),
        "agreement_avg": round(agree_avg, 3),
        "n_suspicious": len(susp),
        "suspect_sec": round(sum(c["end"] - c["start"] for c in susp), 1),
        "chunks": out_chunks,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)

    print(f"[review] среднее согласие {agree_avg*100:.1f}%, "
          f"подозрительных чанков {len(susp)} из {len(out_chunks)} "
          f"({out['suspect_sec']:.0f}с из {total:.0f}с)", file=sys.stderr)
    for c in susp[:15]:
        print(f"  [{c['start']:.0f}-{c['end']:.0f}] {c['agreement']*100:.0f}% "
              f"{'/'.join(c['reasons'])}", file=sys.stderr)


if __name__ == "__main__":
    main()
