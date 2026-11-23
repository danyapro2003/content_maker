# diarize.py — диаризация спикеров через pyannote.audio
# Вход: wav-файл. Выход: JSON [{start, end, speaker}, ...]
# Использование:
#   python diarize.py audio.wav --out diar.json --model pyannote/speaker-diarization-3.1 --hf-token TOKEN
import argparse
import json
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", default="pyannote/speaker-diarization-3.1")
    ap.add_argument("--hf-token", default=None)
    args = ap.parse_args()

    try:
        from pyannote.audio import Pipeline
    except ImportError:
        print("pyannote.audio не установлен в этом окружении", file=sys.stderr)
        sys.exit(2)

    pipeline = Pipeline.from_pretrained(args.model, token=args.hf_token)
    if pipeline is None:
        print("Не удалось загрузить модель (проверь HF токен и принятие лицензии)", file=sys.stderr)
        sys.exit(3)

    # GPU, если доступен
    import torch
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    pipeline.to(device)
    print(f"[pyannote] device={pipeline.device}", flush=True)

    # грузим wav через scipy — избегаем torchcodec/torchaudio
    # (torchcodec DLL на Windows часто падает с нативным крашем)
    import numpy as np
    from scipy.io import wavfile

    sample_rate, data = wavfile.read(args.audio)
    if data.dtype == np.int16:
        data = data.astype(np.float32) / 32768.0
    elif data.dtype == np.int32:
        data = data.astype(np.float32) / 2147483648.0
    else:
        data = data.astype(np.float32)
    if data.ndim > 1:  # стерео -> моно
        data = data.mean(axis=1)
    waveform = torch.from_numpy(data).unsqueeze(0)  # (1, time)

    diarization = pipeline({"waveform": waveform, "sample_rate": sample_rate})

    # pyannote 3.x: DiarizeOutput; 4.x: .speaker_diarization
    annotation = getattr(diarization, "speaker_diarization", diarization)

    segments = []
    for turn, _, speaker in annotation.itertracks(yield_label=True):
        segments.append({
            "start": round(turn.start, 3),
            "end": round(turn.end, 3),
            "speaker": speaker,  # SPEAKER_00, SPEAKER_01, ...
        })

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(segments, f, ensure_ascii=False, indent=1)

    print(f"OK: {len(segments)} сегментов, "
          f"{len(set(s['speaker'] for s in segments))} спикеров")


if __name__ == "__main__":
    main()
