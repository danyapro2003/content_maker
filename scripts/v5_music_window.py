"""Window for importing, auditioning and editing V5 music cards."""
import json
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from v5_music_library import FORMATS, HIDDEN, ROOT, connect, describe, import_track, dynamics


def main():
    cfg = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8-sig"))
    try:
        from tkinterdnd2 import DND_FILES, TkinterDnD
        root = TkinterDnD.Tk()
    except (ImportError, RuntimeError, tk.TclError):
        root = tk.Tk()
    root.title("Библиотека музыки · Пересказ V5")
    root.geometry("900x650")
    db = connect()
    bar = ttk.Frame(root,padding=8); bar.pack(fill="x")
    body = ttk.Panedwindow(root,orient="horizontal"); body.pack(fill="both",expand=True)
    tracks = ttk.Treeview(body,show="tree",selectmode="browse"); body.add(tracks,weight=1)
    detail = ttk.Frame(body,padding=8); body.add(detail,weight=3)
    title = tk.StringVar(); ttk.Label(detail,textvariable=title,wraplength=520).pack(anchor="w")
    enabled = tk.BooleanVar(); part_enabled=tk.BooleanVar()
    ttk.Checkbutton(detail,text="Разрешить трек для подбора",variable=enabled).pack(anchor="w")
    def field(parent,label,var=None):
        ttk.Label(parent,text=label).pack(anchor="w")
        entry=ttk.Entry(parent,textvariable=var); entry.pack(fill="x")
        return entry
    track_note=tk.StringVar(); field(detail,"Моя заметка о треке и ограничения",track_note)
    parts=ttk.Treeview(detail,columns=("time","description"),show="headings",height=6,selectmode="browse")
    parts.heading("time",text="Секунды"); parts.column("time",width=100,stretch=False)
    parts.heading("description",text="Участок"); parts.pack(fill="both",expand=True,pady=6)
    bounds=ttk.Frame(detail);bounds.pack(fill="x")
    start,end=tk.StringVar(),tk.StringVar()
    for label,var in [("От, с",start),("До, с",end)]:
        ttk.Label(bounds,text=label).pack(side="left");ttk.Entry(bounds,textvariable=var,width=9).pack(side="left")
    auto_text=tk.StringVar();ttk.Label(detail,textvariable=auto_text,wraplength=500).pack(anchor="w",pady=5)
    note=tk.StringVar(); field(detail,"Моя заметка об участке",note)
    ttk.Checkbutton(detail,text="Разрешить этот участок",variable=part_enabled).pack(anchor="w")
    buttons=ttk.Frame(detail); buttons.pack(fill="x",pady=5)
    status=tk.StringVar(); ttk.Label(root,textvariable=status,wraplength=840,padding=6).pack(fill="x")
    state={"track":"","part":0,"player":None,"busy":False}
    def stop():
        if state["player"] and state["player"].poll() is None: state["player"].terminate()
    def select_part(*_):
        if not parts.selection(): return
        p=db.execute("SELECT * FROM parts WHERE id=?",(int(parts.selection()[0]),)).fetchone()
        state["part"]=p["id"];start.set(str(p["start"]));end.set(str(round(p["end"],4)))
        note.set(p["note"]);part_enabled.set(bool(p["enabled"]))
        levels=db.execute("SELECT levels FROM tracks WHERE id=?",(p["track"],)).fetchone()[0]
        energy=dynamics(json.loads(levels),p["start"],p["end"])
        details = f"Уровень {energy['median_dbfs']} dBFS, изменение {energy['change_db']:+} дБ." if energy else ""
        auto_text.set(details+"  "+(p["description"] or "Автоописание отсутствует. Для подбора можно добавить свою заметку."))
    def select_track(*_):
        if not tracks.selection():return
        key=tracks.selection()[0]
        t=db.execute("SELECT * FROM tracks WHERE id=?",(key,)).fetchone()
        state["track"]=key
        title.set(f"{t['title']}  ·  {t['artist']}  ·  {t['duration']:.1f} с")
        track_note.set(t["note"]);enabled.set(bool(t["enabled"]))
        parts.delete(*parts.get_children())
        for p in db.execute("SELECT * FROM parts WHERE track=? ORDER BY start",(key,)):
            parts.insert("","end",iid=str(p["id"]),values=(f"{p['start']:.1f} – {p['end']:.1f}",p["note"] or p["description"] or "Без описания"))
        if parts.get_children():parts.selection_set(parts.get_children()[0])
    def refresh(preferred=None):
        tracks.delete(*tracks.get_children())
        for t in db.execute("SELECT id,title FROM tracks ORDER BY title COLLATE NOCASE"):
            tracks.insert("","end",iid=t["id"],text=t["title"])
        ids=tracks.get_children()
        if ids: tracks.selection_set(preferred if preferred in ids else ids[0])
        else: status.set("Добавьте файлы или перетащите папку с музыкой.")
    tracks.bind("<<TreeviewSelect>>",select_track);parts.bind("<<TreeviewSelect>>",select_part)
    def limits():
        p=db.execute("SELECT * FROM tracks WHERE id=?",(state["track"],)).fetchone()
        if not p:raise ValueError("Выберите трек")
        a,b=float(start.get().replace(",",".")),float(end.get().replace(",","."))
        if not 0<=a<b<=p["duration"]+.001:raise ValueError("Проверьте границы участка в секундах")
        return p,a,min(b,p["duration"])
    def save():
        try:
            t,a,b=limits()
            with db:
                db.execute("UPDATE tracks SET note=?,enabled=? WHERE id=?",(track_note.get(),enabled.get(),t["id"]))
                db.execute("UPDATE parts SET start=?,end=?,note=?,enabled=? WHERE id=?",(a,b,note.get(),part_enabled.get(),state["part"]))
            parts.item(str(state["part"]),values=(f"{a:.1f} – {b:.1f}",note.get() or auto_text.get()))
            status.set("Карточка сохранена.")
        except Exception as e: messagebox.showerror("Карточка",str(e))
    def play():
        try:
            t,a,b=limits();stop()
            state["player"]=subprocess.Popen(["ffplay","-v","error","-autoexit","-nodisp","-ss",str(a),"-t",str(b-a),str(ROOT/t["file"])],creationflags=HIDDEN)
        except Exception as e: messagebox.showerror("Прослушивание",str(e))
    for name,cmd in [("▶ Участок",play),("Стоп",stop),("Сохранить карточку",save)]:
        ttk.Button(buttons,text=name,command=cmd).pack(side="left")
    auto=tk.BooleanVar(value=(db.execute("SELECT value FROM prefs WHERE key='auto_describe'").fetchone() or ["1"])[0]=="1")
    model=tk.StringVar(value=(db.execute("SELECT value FROM prefs WHERE key='audio_model'").fetchone() or [cfg.get("llmModel","")])[0])
    ttk.Checkbutton(detail,text="При импорте отправлять аудио модели для описания",variable=auto).pack(anchor="w")
    field(detail,"Модель с аудиовходом · "+cfg.get("llmBaseUrl",""),model)
    events=queue.Queue()
    def launch(paths=None):
        if state["busy"]:return
        if paths is None and not state["track"]:return
        model_name=model.get().strip()
        with db:
            db.execute("INSERT OR REPLACE INTO prefs VALUES('audio_model',?)",(model_name,))
            db.execute("INSERT OR REPLACE INTO prefs VALUES('auto_describe',?)",("1" if auto.get() else "0",))
        state["busy"]=True;current=state["track"];use_audio=auto.get()
        def work():
            conn=connect(); preferred=current;errors=[];added=0;duplicates=0;audio_available=use_audio
            try:
                if paths is None:
                    events.put(("msg","Модель слушает участки трека…"));describe(conn,current,cfg,model_name)
                else:
                    files=[]
                    for path in map(Path,paths):
                        files.extend(sorted(path.rglob("*")) if path.is_dir() else [path])
                    for f in files:
                        if not f.is_file() or f.suffix.lower() not in FORMATS:continue
                        events.put(("msg","Обработка · "+f.name))
                        try:
                            preferred,new=import_track(conn,f,cfg);added+=int(new);duplicates+=int(not new)
                            if new and audio_available:
                                try:
                                    describe(conn,preferred,cfg,model_name)
                                except Exception as e:
                                    errors.append(f"Трек сохранён. Автоописание приостановлено для этого импорта: {e}")
                                    audio_available=False
                        except Exception as e:errors.append(f"{f.name}: {e}")
            except Exception as e:errors.append(str(e))
            finally:
                conn.close();events.put(("done",preferred,("Описание завершено." if paths is None else f"Добавлено {added}, уже в библиотеке {duplicates}.")+" "+" | ".join(errors)))
        threading.Thread(target=work,daemon=True).start()
    def poll():
        while not events.empty():
            e=events.get()
            if e[0]=="msg":status.set(e[1])
            else:state["busy"]=False;refresh(e[1]);status.set(e[2])
        root.after(100,poll)
    ttk.Button(detail,text="Описать трек заново по аудио",command=launch).pack(anchor="w")
    ttk.Button(bar,text="Добавить файлы…",command=lambda:launch(filedialog.askopenfilenames(filetypes=[("Аудио","*.mp3 *.wav *.flac *.m4a *.ogg *.aac")]))).pack(side="left")
    def folder():
        path=filedialog.askdirectory()
        if path:launch([path])
    ttk.Button(bar,text="Добавить папку…",command=folder).pack(side="left")
    if hasattr(root,"drop_target_register"):
        root.drop_target_register(DND_FILES)
        root.dnd_bind("<<Drop>>",lambda e:launch(root.tk.splitlist(e.data)))
    def close():
        stop();root.destroy()
    root.protocol("WM_DELETE_WINDOW",close);refresh();poll();root.mainloop()


if __name__=="__main__":
    main()
