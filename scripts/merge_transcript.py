"""Сшивка транскрипта для Пересказа 3.0: слова + карта аудио + диаризация -> SRT + packed.md.

Логика фраз — та же, что в TranscriptPacker (C++), чтобы LLM видела привычный формат:
  * разрыв по паузе >= pause,
  * разрыв по смене спикера,
  * жёсткий лимит длины фразы <= max_phrase,
  * мягкий разрыв на границе предложения при наборе 40% длины.

Дополнительно к v2:
  * строки-маркеры для интервалов без речи: [120.5-167.2] (музыка, 46.7 с),
  * пометка '?' на неуверенных фразах (низкий logprob),
  * флаг suspicious из ревизора (если передан --review).

Запуск:
  python merge_transcript.py --words words.json --out speech.srt \
      [--audiomap audiomap.json] [--diar diar.json] [--review review.json] \
      [--packed packed.md] [--pause 0.5] [--max-phrase 12]
"""
from __future__ import annotations

import argparse
import json
import os
import sys

SENT_END = ".!?…:"


def load(path):
    if not path or not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def srt_time(t):
    if t < 0:
        t = 0.0
    ms = int(round(t * 1000))
    h, ms = divmod(ms, 3600000)
    m, ms = divmod(ms, 60000)
    s, ms = divmod(ms, 1000)
    return "%02d:%02d:%02d,%03d" % (h, m, s, ms)


def speaker_at(segments, start, end):
    """Доминирующий спикер по перекрытию с интервалом."""
    best, best_ov = None, 0.0
    for s in segments:
        ov = min(end, s["end"]) - max(start, s["start"])
        if ov > best_ov:
            best_ov, best = ov, s["speaker"]
    return best


def in_zone(zones, t):
    for z in zones:
        if z[0] <= t < z[1]:
            return z
    return None


def build_phrases(words, diar_segs, pause, max_phrase):
    phrases = []
    spk_map = {}

    for w in words:
        text = (w.get("w") or "").strip()
        if not text:
            continue
        st, en = float(w["t"]), float(w["e"])
        if en <= st:
            en = st + 0.08

        spk_raw = speaker_at(diar_segs, st, en) if diar_segs else None
        if spk_raw:
            if spk_raw not in spk_map:
                spk_map[spk_raw] = "S%d" % len(spk_map)
            spk = spk_map[spk_raw]
        else:
            spk = ""

        new = True
        if phrases:
            last = phrases[-1]
            dur = last["end"] - last["start"]
            gap = st - last["end"]
            if gap >= pause:
                new = True
            elif spk and last["speaker"] and spk != last["speaker"]:
                new = True
            elif en - last["start"] > max_phrase:
                new = True
            elif dur >= 0.4 * max_phrase and last["text"][-1:] in SENT_END:
                new = True
            else:
                new = False

        if new:
            phrases.append({"start": st, "end": en, "speaker": spk,
                            "text": text, "lps": [w["lp"]] if w.get("lp") is not None else []})
        else:
            p = phrases[-1]
            p["end"] = max(p["end"], en)
            p["text"] += " " + text
            if w.get("lp") is not None:
                p["lps"].append(w["lp"])
    return phrases


def add_gap_markers(phrases, zones, min_gap=8.0):
    """Между фразами вставляем маркеры «что там было» — музыка/тишина."""
    out = []
    prev_end = None
    for p in phrases:
        if prev_end is not None and zones and p["start"] - prev_end >= min_gap:
            gap_start, gap_end = prev_end, p["start"]
            label = None
            z = in_zone(zones, (gap_start + gap_end) / 2.0)
            if z:
                label = z[2]
            if label and label != "speech":
                out.append({"start": gap_start, "end": gap_end,
                            "speaker": "", "text": "(%s, %.1f с)"
                            % ("музыка" if label == "music" else "без речи",
                               gap_end - gap_start),
                            "lps": [], "marker": True})
        out.append(p)
        prev_end = p["end"]
    return out


def extract_zones(am):
    if not am:
        return []
    return [(s["start"], s["end"], s["label"]) for s in am.get("segments", [])]


def suspicious_ranges(review):
    if not review:
        return []
    out = []
    for c in review.get("chunks", []):
        if c.get("suspicious"):
            out.append((c["start"], c["end"]))
    return out


def is_suspicious(ranges, start, end):
    for a, b in ranges:
        if min(end, b) - max(start, a) > 0.5 * (end - start):
            return True
    return False


def write_srt(phrases, path):
    with open(path, "w", encoding="utf-8") as f:
        for i, p in enumerate(phrases, 1):
            f.write("%d\n%s --> %s\n%s\n\n"
                    % (i, srt_time(p["start"]), srt_time(p["end"]), p["text"]))


def write_packed(phrases, path):
    with open(path, "w", encoding="utf-8") as f:
        for p in phrases:
            f.write("[%.2f-%.2f] " % (p["start"], p["end"]))
            if p["speaker"]:
                f.write(p["speaker"] + " ")
            f.write(p["text"])
            if p.get("flag"):
                f.write("  " + p["flag"])
            f.write("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--words", required=True)
    ap.add_argument("--out", required=True, help="SRT для прожига")
    ap.add_argument("--packed", default=None, help="markdown для LLM")
    ap.add_argument("--clean-words", default=None, help="единые проверенные слова для монтажа и сабов")
    ap.add_argument("--audiomap", default=None)
    ap.add_argument("--diar", default=None)
    ap.add_argument("--review", default=None)
    ap.add_argument("--pause", type=float, default=0.5)
    ap.add_argument("--max-phrase", type=float, default=12.0)
    ap.add_argument("--min-gap", type=float, default=8.0)
    ap.add_argument("--drop-music", action="store_true",
                    help="выбросить слова, попавшие в помеченную музыку (осторожно!)")
    ap.add_argument("--drop-music-lowconf", action="store_true",
                    help="выбросить только неуверенные слова в музыке (lp < --min-lp); "
                         "диалог под музыкой остаётся")
    ap.add_argument("--min-lp", type=float, default=-0.55)
    args = ap.parse_args()

    data = load(args.words)
    if not data:
        sys.exit("нет words.json")
    words = data.get("words", [])

    zones = extract_zones(load(args.audiomap))
    diar = load(args.diar) or []
    review = load(args.review)
    sus = suspicious_ranges(review)
    # Repairs are applied once here, before both phrase packing and subtitle export.
    for correction in (review or {}).get("corrections", []):
        a, b = correction["start"], correction["end"]
        words = [w for w in words if not a <= (w["t"] + w["e"]) / 2 < b]
        words.extend(correction["words"])
    words = sorted(words, key=lambda w: (w["t"], w["e"]))

    dropped = 0
    if args.drop_music and zones:
        music = [(a, b) for a, b, lab in zones if lab == "music"]
        kept = []
        for w in words:
            mid = (w["t"] + w["e"]) / 2.0
            if any(a <= mid < b for a, b in music):
                dropped += 1
                continue
            kept.append(w)
        words = kept
    elif args.drop_music_lowconf and zones:
        # Правило, подтверждённое замерами: в музыке выкидываем только слова
        # с плохим logprob (галлюцинация), а нормальные оставляем —
        # под музыкой часто идёт настоящий диалог.
        music = [(a, b) for a, b, lab in zones if lab == "music"]
        kept = []
        for w in words:
            mid = (w["t"] + w["e"]) / 2.0
            lp = w.get("lp")
            if any(a <= mid < b for a, b in music):
                if lp is not None and lp < args.min_lp:
                    dropped += 1
                    continue
            kept.append(w)
        words = kept

    phrases = build_phrases(words, diar, args.pause, args.max_phrase)
    phrases = add_gap_markers(phrases, zones, args.min_gap)

    uncertain = 0
    suspect = 0
    for p in phrases:
        if p.get("marker"):
            continue
        flags = []
        if p["lps"]:
            lp = sum(p["lps"]) / len(p["lps"])
            if lp < args.min_lp:
                flags.append("?")
                uncertain += 1
        if sus and is_suspicious(sus, p["start"], p["end"]):
            flags.append("[проверить]")
            suspect += 1
        p["flag"] = " ".join(flags)

    # Markers describe gaps to the LLM; they must never become visible subtitles.
    write_srt([p for p in phrases if not p.get("marker")], args.out)
    if args.clean_words:
        clean = {"engine": data.get("engine"), "language": data.get("language", "ru"),
                 "audio_sec": data.get("audio_sec"), "words": words,
                 "corrections": len((review or {}).get("corrections", [])),
                 "dropped_music_words": dropped}
        with open(args.clean_words, "w", encoding="utf-8") as f:
            json.dump(clean, f, ensure_ascii=False, indent=1)
    if args.packed:
        write_packed(phrases, args.packed)

    spk = len({p["speaker"] for p in phrases if p["speaker"]})
    total = phrases[-1]["end"] - phrases[0]["start"] if phrases else 0
    msg = ("[merge] фраз %d, спикеров %d, неуверенных %d, подозрительных %d"
           % (len(phrases), spk, uncertain, suspect))
    if dropped:
        msg += ", выброшено в музыке %d слов" % dropped
    if args.packed:
        msg += ", packed %d символов" % os.path.getsize(args.packed)
    print(msg, file=sys.stderr)
    print("[merge] покрытие %.0fс (%.0f%%) от %.0fс аудио"
          % (total, 100 * total / data["audio_sec"] if data.get("audio_sec") else 0,
             data.get("audio_sec", 0)), file=sys.stderr)


if __name__ == "__main__":
    main()
