#!/usr/bin/env python3
from __future__ import annotations
import argparse,json,os,re,sys,tempfile
from pathlib import Path

STOCK_SONGS={"tutorial","bopeebo","fresh","dad-battle","spookeez","south","monster","pico","philly-nice","blammed","satin-panties","high","milf","cocoa","eggnog","winter-horrorland","senpai","roses","thorns","ugh","guns","stress","darnell","lit-up","2hot","blazin"}
STOCK_CHARS={"dad","mom","mom-car","parents-christmas","spooky","pico","monster","monster-christmas","senpai","senpai-angry","spirit","tankman"}
STOCK_STAGES={"stage","spooky","philly","limo","mall","mallevil","school","schoolevil","tank","phillystreets","phillyblazin"}
BUILTIN_NOTES={"","alt animation","gf sing","hey!","hurt note","no animation"}
BUILTIN_EVENTS={"","add camera zoom","alt idle animation","camera follow pos","change character","change scroll speed","hey!","kill henchmen","play animation","screen shake","set gf speed","set property"}
AUDIO=(".ogg",".wav",".mp3",".flac"); IMAGES=(".png",".jpg",".jpeg",".webp"); VIDEOS=(".mp4",".webm",".mkv",".avi",".mov"); ATLASES=(".xml",".txt",".json")

def n(s): return str(s).replace("\\","/").strip().strip("/").lower()
def song_id(s): return re.sub(r"[^\w\x80-\uffff]+","-",str(s).strip().lower()).strip("-")
def lsize(p):
    try:z=p.stat().st_size
    except OSError:return 0
    if z<=1024:
        try:t=p.read_text(encoding="utf-8")
        except Exception:return z
        if t.startswith("version https://git-lfs.github.com/spec/v1\n"):
            m=re.search(r"^size (\d+)$",t,re.M)
            if m:return int(m.group(1))
    return z
def idx(root,files):
    out=set()
    for p in files:
        try:out.add(n(p.relative_to(root).as_posix()))
        except ValueError:pass
    return out
def has(index,cands):
    c=[n(x) for x in cands]; return any(any(x.endswith(y) for x in index) for y in c)
def findp(root,files,cands):
    c=[n(x) for x in cands]
    for p in files:
        try:r=n(p.relative_to(root).as_posix())
        except ValueError:continue
        if any(r.endswith(x) for x in c):return p
    return None
def achild(p,lim):
    try:r=[x for x in p.iterdir() if x.is_dir()]
    except OSError:return []
    return sorted(r,key=lambda x:x.as_posix().lower())[:lim]
def stock_provider(project):
    mods=project/"mods"; ranked=[]
    if not mods.is_dir():return None
    c=[]
    for pack in achild(mods,256):
        for x in (pack/"assets",pack/"bin"/"assets"):
            if x.is_dir():c.append(x)
        for ch in achild(pack,64):
            for x in (ch/"assets",ch/"bin"/"assets"):
                if x.is_dir():c.append(x)
    for a in sorted(set(map(Path.resolve,c)),key=lambda x:x.as_posix().lower())[:1024]:
        songs=sum((a/"songs"/s).is_dir() for s in STOCK_SONGS)
        if songs<4:continue
        topo=sum((a/d).is_dir() for d in ("images","songs","sounds","music","shared","characters","data")); ranked.append((-(songs*100+topo),a.as_posix().lower(),a,songs))
    if not ranked:return None
    _,_,a,s=sorted(ranked)[0]; return {"assetsRoot":str(a),"stockSongs":s}
def audio(song,stem):
    return [f"{p}/{stem}{e}" for e in AUDIO for p in (f"songs/{song}",f"assets/songs/{song}",f"assets/preload/songs/{song}",f"assets/shared/songs/{song}")]
def voices(song):return sum((audio(song,s) for s in ("voices","voices-player","voices-opponent","voices-bf","voices-dad","voices-player1","voices-player2")),[])
def chars(x):return [f"{p}/{n(x)}.json" for p in ("characters","data/characters","assets/characters","assets/data/characters","assets/preload/characters","assets/shared/characters")]
def stages(x):return [f"{p}/{n(x)}{e}" for e in (".json",".lua") for p in ("stages","data/stages","assets/stages","assets/data/stages","assets/preload/stages","assets/shared/stages")]
def scripts(folder,x):return [f"{p}/{n(x)}.lua" for p in (folder,f"data/{folder}",f"assets/{folder}",f"assets/data/{folder}",f"assets/preload/{folder}",f"assets/shared/{folder}")]
def img(r):
    r=n(r); roots=("images","assets/images","shared/images","assets/shared/images","preload/images","assets/preload/images"); _,e=os.path.splitext(r); names=[r] if e in IMAGES else [r+x for x in IMAGES]
    return names if r.startswith(tuple(x+"/" for x in roots)) else [f"{p}/{x}" for p in roots for x in names]
def atlas(r):
    out=[]
    for x in img(r):
        stem=os.path.splitext(x)[0]; out += [stem+e for e in ATLASES]
    return out
def gaudio(r,k):
    r=n(r); _,e=os.path.splitext(r); names=[r] if e in AUDIO else [r+x for x in AUDIO]
    return [f"{p}/{x}" for p in (k,f"assets/{k}",f"shared/{k}",f"assets/shared/{k}",f"preload/{k}",f"assets/preload/{k}") for x in names]
def video(r):
    r=n(r); _,e=os.path.splitext(r); names=[r] if e in VIDEOS else [r+x for x in VIDEOS]
    return [f"{p}/{x}" for p in ("videos","assets/videos","shared/videos","assets/shared/videos","preload/videos","assets/preload/videos") for x in names]
def shader(r):
    r=n(r); _,e=os.path.splitext(r); names=[r] if e else [r+".frag",r+".vert"]
    return [f"{p}/{x}" for p in ("shaders","data/shaders","shared/shaders","assets/shaders","assets/data/shaders","assets/shared/shaders") for x in names]
def childscript(r):
    r=n(r) if n(r).endswith(".lua") else n(r)+".lua"; return [r,f"scripts/{r}",f"assets/scripts/{r}",f"assets/preload/scripts/{r}",f"assets/shared/scripts/{r}"]
def stock_char(x):
    x=n(x); return x in STOCK_CHARS or x.startswith("bf") or x.startswith("gf")

LUA=[
("image",re.compile(r"\bmake(?:Animated)?LuaSprite\s*\(\s*[^,]+,\s*['\"]([^'\"]+)['\"]",re.I)),
("image",re.compile(r"\bprecacheImage\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),
("image",re.compile(r"\bloadGraphic\s*\(\s*[^,]+,\s*['\"]([^'\"]+)['\"]",re.I)),
("sound",re.compile(r"\b(?:playSound|precacheSound)\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),
("music",re.compile(r"\bplayMusic\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),
("video",re.compile(r"\b(?:startVideo|playVideo)\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),
("shader",re.compile(r"\binitLuaShader\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),
("script",re.compile(r"\baddLuaScript\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),
("character",re.compile(r"\b(?:addCharacterToList|precacheCharacter)\s*\(\s*['\"]([^'\"]+)['\"]",re.I))]

def readj(p):
    try:
        if p.stat().st_size>16*1024*1024:return None
        return json.loads(p.read_text(encoding="utf-8",errors="replace"))
    except Exception:return None
def meta(path):
    r=readj(path)
    if isinstance(r,dict):
        s=r.get("song",r)
        if isinstance(s,dict):
            d={k:s.get(k) for k in ("song","needsVoices","player1","player2","gfVersion","stage")}; d["notes"]=set(); d["events"]=set(); d["dynamicChars"]=set()
            for sec in s.get("notes",[]):
                if isinstance(sec,dict):
                    for q in sec.get("sectionNotes",[]):
                        if isinstance(q,list) and len(q)>3 and isinstance(q[3],str):d["notes"].add(q[3])
            for ev in s.get("events",[]):
                if isinstance(ev,list) and len(ev)>1 and isinstance(ev[1],list):
                    for q in ev[1]:
                        if isinstance(q,list) and q and isinstance(q[0],str):
                            d["events"].add(q[0])
                            if n(q[0])=="change character" and len(q)>2 and isinstance(q[2],str) and q[2].strip():d["dynamicChars"].add(q[2].strip())
            return d,True
    try:t=path.open("rb").read(2*1024*1024).decode("utf-8",errors="replace")
    except OSError:return {},False
    d={"notes":set(),"events":set(),"dynamicChars":set()}
    for k in ("song","player1","player2","gfVersion","stage"):
        m=re.search(rf'"{k}"\s*:\s*"([^"]+)"',t,re.I)
        if m:d[k]=m.group(1)
    m=re.search(r'"needsVoices"\s*:\s*(true|false)',t,re.I)
    if m:d["needsVoices"]=m.group(1).lower()=="true"
    return d,False

def event_rows(raw):
    """Yield Psych event triplets from chart or standalone events JSON shapes."""
    if isinstance(raw,dict):
        song=raw.get("song",raw)
        if isinstance(song,dict): raw=song.get("events",[])
        else: raw=[]
    if not isinstance(raw,list): return
    for ev in raw:
        if not (isinstance(ev,list) and len(ev)>1 and isinstance(ev[1],list)): continue
        for q in ev[1]:
            if isinstance(q,list) and q and isinstance(q[0],str): yield q

def audit_character(cid,context,mod,files,local,stock,err,warn,checked):
    key=n(cid)
    if not key or stock_char(key) or key in checked:return
    checked.add(key); p=findp(mod,files,chars(key))
    if p is None:err.append(f"{context}: custom character '{cid}' definition missing"); return
    r=readj(p)
    if not isinstance(r,dict):err.append(f"{p}: custom character JSON unreadable"); return
    image=r.get("image")
    if not isinstance(image,str) or not image.strip():err.append(f"{p}: custom character '{cid}' has no image reference"); return
    if not (has(local,img(image)) or (stock and has(stock,img(image)))):err.append(f"{p}: custom character '{cid}' image '{image}' is missing"); return
    if isinstance(r.get("animations"),list) and r["animations"] and not (has(local,atlas(image)) or (stock and has(stock,atlas(image)))):err.append(f"{p}: custom character '{cid}' image '{image}' has animations but no XML/TXT/JSON atlas metadata")

def audit(project,mod,desc):
    project=project.resolve(); mod=mod.resolve(); err=[]; warn=[]
    if not mod.is_dir():return {},[f"missing mod root: {mod}"]
    files=[p for p in mod.rglob("*") if p.is_file()]
    if len(files)>600000:return {},["payload exceeds 600000-file audit bound"]
    total=sum(lsize(p) for p in files)
    if len(files)<int(desc.get("min_files",0)):err.append(f"{len(files)} files < descriptor minimum {desc.get('min_files')}")
    if total<int(desc.get("min_bytes",0)):err.append(f"{total} bytes < descriptor minimum {desc.get('min_bytes')}")
    local=idx(mod,files); sp=stock_provider(project); stock=set()
    if sp:
        a=Path(sp["assetsRoot"])
        try:stock=idx(a.parent,[p for p in a.rglob("*") if p.is_file()])
        except OSError:pass
    def ok(c,allow=True):return has(local,c) or bool(allow and stock and has(stock,c))
    charts=[]; notes=set(); events=set(); refs=[]
    for p in files:
        if p.suffix.lower()!=".json":continue
        rel=p.relative_to(mod); parts=list(rel.parts); low=[x.lower() for x in parts]
        if "data" not in low[:-1]:continue
        i=max(i for i,x in enumerate(low[:-1]) if x=="data")
        if i+1>=len(parts)-1:continue
        folder=song_id(parts[i+1]); stem=song_id(p.stem)
        if not folder or stem=="events" or folder not in stem:continue
        d,full=meta(p); sid=song_id(d.get("song") or folder); entry={"chart":str(p.relative_to(project)),"song":sid,"full":full}
        if not ok(audio(sid,"inst"),sid in STOCK_SONGS):err.append(f"{entry['chart']}: missing Inst for '{sid}'")
        if d.get("needsVoices") is True and not ok(voices(sid),sid in STOCK_SONGS):err.append(f"{entry['chart']}: needsVoices=true but Voices missing for '{sid}'")
        if d.get("needsVoices") is None:warn.append(f"{entry['chart']}: needsVoices not inspectable within bound")
        for k in ("player1","player2","gfVersion"):
            v=d.get(k)
            if isinstance(v,str) and v:refs.append((v,entry["chart"]))
        for v in d.get("dynamicChars",set()):refs.append((v,f"{entry['chart']} Change Character event"))
        v=d.get("stage")
        if isinstance(v,str) and v and n(v) not in STOCK_STAGES and not ok(stages(v),False):err.append(f"{entry['chart']}: custom stage '{v}' missing")
        notes.update(d.get("notes",set())); events.update(d.get("events",set())); charts.append(entry)
    # Standalone Psych data/<song>/events.json files carry the same runtime
    # dependencies as inline chart events and must not be ignored.
    for p in files:
        if p.suffix.lower()!=".json" or p.stem.lower()!="events": continue
        rel=p.relative_to(mod); low=[x.lower() for x in rel.parts]
        if "data" not in low[:-1]: continue
        raw=readj(p)
        if raw is None:
            warn.append(f"{p.relative_to(project)}: standalone events JSON unreadable within audit bound")
            continue
        for q in event_rows(raw):
            events.add(q[0])
            if n(q[0])=="change character" and len(q)>2 and isinstance(q[2],str) and q[2].strip():
                refs.append((q[2].strip(),f"{p.relative_to(project)} Change Character event"))

    checked=set()
    for cid,ctx in refs:audit_character(cid,ctx,mod,files,local,stock,err,warn,checked)
    for v in notes:
        if n(v) not in BUILTIN_NOTES and not ok(scripts("custom_notetypes",v),False):err.append(f"custom note type '{v}' script missing")
    for v in events:
        if n(v) not in BUILTIN_EVENTS and not ok(scripts("custom_events",v),False):err.append(f"custom event '{v}' script missing")
    lua_refs=0
    for p in files:
        if p.suffix.lower()!=".lua":continue
        try:
            if p.stat().st_size>4*1024*1024:warn.append(f"{p.relative_to(project)}: Lua too large for static ref audit"); continue
            t=p.read_text(encoding="utf-8",errors="replace")
        except OSError:continue
        for kind,pat in LUA:
            for m in pat.finditer(t):
                lua_refs+=1; r=m.group(1); src=f"{p.relative_to(project)}:{t.count(chr(10),0,m.start())+1}"
                if kind=="character":audit_character(r,f"{src} Lua character preload",mod,files,local,stock,err,warn,checked); continue
                c={"image":img(r),"sound":gaudio(r,"sounds"),"music":gaudio(r,"music"),"video":video(r),"shader":shader(r),"script":childscript(r)}[kind]; allow=kind!="script"
                if not ok(c,allow):err.append(f"{src}: unresolved {kind} '{r}'"+(" (no stock provider)" if allow and not sp else ""))
    if charts and len(files)<=2:err.append("chart-bearing payload is metadata/charts only; functional assets are missing")
    rep={"format":"pulseforge-complete-mod-audit-v3","slug":desc.get("slug"),"files":len(files),"bytes":total,"stockProvider":sp,"charts":charts,"charactersChecked":sorted(checked),"luaReferencesChecked":lua_refs,"warnings":warn,"errors":err,"ok":not err}
    return rep,err

def selftest():
    with tempfile.TemporaryDirectory() as td:
        r=Path(td); m=r/"mods"/"x"
        for d in ("data/custom","songs/custom","characters","stages","custom_notetypes","custom_events","images/characters","images/bg","videos","scripts"):(m/d).mkdir(parents=True,exist_ok=True)
        j={"song":{"song":"custom","needsVoices":True,"player1":"bf","player2":"xchar","gfVersion":"gf","stage":"xstage","notes":[{"sectionNotes":[[0,0,0,"X Note"]]}],"events":[[0,[["X Event","",""]]]]}}
        (m/"data/custom/custom.json").write_text(json.dumps(j)); (m/"data/custom/events.json").write_text(json.dumps({"song":{"events":[[0,[["Change Character","dad","xchar2"]]]]}})); (m/"songs/custom/Inst.ogg").write_bytes(b"x"); (m/"songs/custom/Voices.ogg").write_bytes(b"x")
        for c in ("xchar","xchar2"):
            (m/f"characters/{c}.json").write_text(json.dumps({"image":f"characters/{c}","animations":[{"anim":"idle","name":"idle"}]})); (m/f"images/characters/{c}.png").write_bytes(b"x"); (m/f"images/characters/{c}.xml").write_text("<TextureAtlas/>")
        (m/"stages/xstage.lua").write_text("makeLuaSprite('x','bg/foo',0,0)\nstartVideo('intro')"); (m/"images/bg/foo.png").write_bytes(b"x"); (m/"videos/intro.mp4").write_bytes(b"x"); (m/"custom_notetypes/X Note.lua").write_text(""); (m/"custom_events/X Event.lua").write_text(""); (m/"scripts/a.lua").write_text("loadGraphic('x','bg/foo')\naddCharacterToList('xchar2','dad')")
        d={"slug":"x","min_files":1,"min_bytes":1}; _,e=audit(r,m,d)
        if e:print(e,file=sys.stderr); return 1
        (m/"images/characters/xchar.xml").unlink(); _,e=audit(r,m,d)
        if not any("atlas metadata" in x for x in e):return 1
        (m/"images/characters/xchar.xml").write_text("<TextureAtlas/>"); (m/"songs/custom/Inst.ogg").unlink(); _,e=audit(r,m,d)
        return 0 if any("missing Inst" in x for x in e) else 1

def main():
    a=argparse.ArgumentParser(); a.add_argument("root",nargs="?",default="."); a.add_argument("--mod-root"); a.add_argument("--descriptor"); a.add_argument("--output"); a.add_argument("--self-test",action="store_true"); x=a.parse_args()
    if x.self_test:
        rc=selftest(); print("PulseForge Complete mod auditor self-test:","PASS" if rc==0 else "FAIL"); return rc
    if not x.mod_root or not x.descriptor:a.error("--mod-root and --descriptor required")
    root=Path(x.root).resolve(); mod=Path(x.mod_root); mod=mod if mod.is_absolute() else root/mod; dp=Path(x.descriptor); dp=dp if dp.is_absolute() else root/dp; d=json.loads(dp.read_text(encoding="utf-8")); rep,err=audit(root,mod,d)
    if x.output:
        out=Path(x.output); out.parent.mkdir(parents=True,exist_ok=True); out.write_text(json.dumps(rep,indent=2,ensure_ascii=False),encoding="utf-8")
    print(f"PulseForge Complete mod audit: {rep.get('files',0)} files; {len(rep.get('charts',[]))} charts; {len(rep.get('charactersChecked',[]))} custom characters; {len(rep.get('errors',[]))} errors; {len(rep.get('warnings',[]))} warnings")
    for w in rep.get("warnings",[]):print("WARNING:",w)
    for e in rep.get("errors",[]):print("ERROR:",e,file=sys.stderr)
    return 1 if err else 0
if __name__=="__main__":raise SystemExit(main())
