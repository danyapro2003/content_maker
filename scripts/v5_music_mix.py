"""Choose V5 background cues once per story and mix a separate language-specific preview."""
import argparse
import json
import math
import os
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

from v5_music_library import ROOT, connect, run, select


def seconds(stamp):
    h,m,s=stamp.split(":")
    return int(h)*3600+int(m)*60+float(s)


def mix(cfg, db, choices, source, subtitles, output):
    ff=cfg.get("ffmpegPath") or "ffmpeg"
    probe=cfg.get("ffprobePath") or "ffprobe"
    def streams(f):
        return json.loads(run([probe,"-v","error","-show_streams","-of","json",str(f)]))["streams"]
    duration=float(next(s for s in streams(source) if s["codec_type"]=="video")["duration"])
    rate=44100
    bus=np.zeros((round(duration*rate),2),dtype="float32")
    grid=np.arange(0,duration+.1,.02)
    duck=np.ones(len(grid))
    for line in subtitles.read_text(encoding="utf-8-sig").splitlines():
        if not line.startswith("Dialogue:"):continue
        fields=line.split(",",4)
        if fields[3]!="Word":continue
        a,b=seconds(fields[1]),seconds(fields[2])
        lo,hi=grid.searchsorted([a-.15,b+.28])
        t=grid[lo:hi]
        gain=1-.65*np.minimum(np.clip((t-a+.15)/.15,0,1),np.clip((b+.28-t)/.28,0,1))
        duck[lo:hi]=np.minimum(duck[lo:hi],gain)
    for cue in choices:
        track=db.execute("SELECT t.file FROM tracks t JOIN parts p ON p.track=t.id WHERE p.id=?",(cue["id"],)).fetchone()
        span=cue["out"]-cue["in"]
        data=run([ff,"-v","error","-ss",str(cue["track_start"]),"-t",str(span),"-i",str(ROOT/track["file"]),
                  "-ac","2","-ar",str(rate),"-f","f32le","-"])
        pcm=np.frombuffer(data,dtype="<f4").reshape(-1,2)
        begin=round(cue["in"]*rate); n=min(len(pcm),len(bus)-begin,round(span*rate))
        if n<=0 or n<round(span*rate)-rate*.1:raise ValueError("Выбранный фрагмент вышел за границу файла")
        t=np.arange(n)/rate
        fade=np.minimum(np.clip(t/.45,0,1),np.clip((span-t)/.65,0,1))
        gain=min(2.,.065/max(.001,float(np.sqrt(np.mean(pcm[:n]**2)))))
        bus[begin:begin+n]+=pcm[:n]*(fade*np.interp(cue["in"]+t,grid,duck)*gain)[:,None]
    if output.exists():raise FileExistsError(output)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        bed=Path(tmp)/"background.wav";rendered=Path(tmp)/"mixed.mp4"
        sf.write(bed,bus,rate,subtype="FLOAT")
        run([ff,"-v","error","-i",str(source),"-i",str(bed),"-filter_complex",
             "[0:a:0][1:a:0]amix=inputs=2:duration=first:normalize=0,alimiter=limit=0.98:level=false:latency=true[a]",
             "-map","0:v:0","-map","[a]","-map_chapters","-1","-c:v","copy","-c:a","aac","-b:a","192k",
             "-movflags","+faststart",str(rendered)])
        info=streams(rendered)
        for kind in ("video","audio"):
            d=float(next(s for s in info if s["codec_type"]==kind)["duration"])
            if not math.isfinite(d) or abs(d-duration)>.20:raise ValueError("Длительность версии с музыкой изменилась")
        os.replace(rendered,output)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for k in ("settings","edl","audiomap","plan","video","ass","output"):
        parser.add_argument(k,type=Path)
    parser.add_argument("story",type=int)
    args=parser.parse_args()
    def read(path):return json.loads(path.read_text(encoding="utf-8-sig"))
    cfg=read(args.settings);db=connect()
    if args.plan.exists():decision=read(args.plan)
    else:
        story=read(args.edl)["stories"][args.story]
        native=[s for s in read(args.audiomap)["segments"] if s["label"]=="music"] if args.audiomap.exists() else []
        try:
            decision={"placements":select(db,cfg,story,native)}
        except Exception as e:
            decision={"placements":[],"selection_error":str(e)[:500]}
        pending=args.plan.with_suffix(".tmp");pending.write_text(json.dumps(decision,ensure_ascii=False,indent=2),encoding="utf8")
        pending.replace(args.plan)
    if decision.get("selection_error"):
        raise ValueError("Подбор музыки для истории не выполнен: "+decision["selection_error"])
    if decision["placements"]:
        mix(cfg,db,decision["placements"],args.video,args.ass,args.output)
        for c in decision["placements"]:print(f"{c['in']:.1f}–{c['out']:.1f} с · {c['title']} · {c['reason']}")
    else:print("Оригинальный звук: подбор не дал музыкальных включений.")


if __name__=="__main__":
    main()
