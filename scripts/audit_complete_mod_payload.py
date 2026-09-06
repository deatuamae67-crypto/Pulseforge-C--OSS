#!/usr/bin/env python3
from __future__ import annotations
import argparse,json,os,re,sys,tempfile
from pathlib import Path

STOCK_SONGS={"tutorial","bopeebo","fresh","dad-battle","spookeez","south","monster","pico","philly-nice","blammed","satin-panties","high","milf","cocoa","eggnog","winter-horrorland","senpai","roses","thorns","ugh","guns","stress","darnell","lit-up","2hot","blazin"}
STOCK_CHARS={"dad","mom","mom-car","parents-christmas","spooky","pico","monster","monster-christmas","senpai","senpai-angry","spirit","tankman"}
STOCK_STAGES={"stage","spooky","philly","limo","mall","mallevil","school","schoolevil","tank","phillystreets","phillyblazin"}
BUILTIN_NOTES={"","alt animation","gf sing","hey!","hurt note","no animation"}
BUILTIN_EVENTS={"","add camera zoom","alt idle animation","camera follow pos","change character","change scroll speed","hey!","kill henchmen","play animation","screen shake","set gf speed","set property"}
AUDIO=(".ogg",".wav",".mp3",".flac")

def n(s): return str(s).replace("\\","/").strip().strip("/").lower()
def song_id(s): return re.sub(r"[^\w\x80-\uffff]+","-",str(s).strip().lower()).strip("-")
def lsize(p):
    try: z=p.stat().st_size
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
    c=[n(x) for x in cands]
    return any(any(x.endswith(y) for x in index) for y in c)
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
        topo=sum((a/d).is_dir() for d in ("images","songs","sounds","music","shared","characters","data"))
        ranked.append((-(songs*100+topo),a.as_posix().lower(),a,songs))
    if not ranked:return None
    _,_,a,s=sorted(ranked)[0];return {"assetsRoot":str(a),"stockSongs":s}
def audio(song,stem):
    return [f"{p}/{stem}{e}" for e in AUDIO for p in (f"songs/{song}",f"assets/songs/{song}",f"assets/preload/songs/{song}",f"assets/shared/songs/{song}")]
def voices(song):return sum((audio(song,s) for s in ("voices","voices-player","voices-opponent","voices-bf","voices-dad")),[])
def chars(x):return [f"{p}/{n(x)}.json" for p in ("characters","data/characters","assets/characters","assets/data/characters","assets/preload/characters","assets/shared/characters")]
def stages(x):return [f"{p}/{n(x)}{e}" for e in (".json",".lua") for p in ("stages","data/stages","assets/stages","assets/data/stages","assets/preload/stages","assets/shared/stages")]
def scripts(folder,x):return [f"{p}/{n(x)}.lua" for p in (folder,f"data/{folder}",f"assets/{folder}",f"assets/data/{folder}",f"assets/preload/{folder}",f"assets/shared/{folder}")]
def img(r):
    r=n(r); root=("images","assets/images","shared/images","assets/shared/images","preload/images","assets/preload/images")
    _,e=os.path.splitext(r); names=[r] if e else [r+x for x in (".png",".jpg",".jpeg",".webp")]
    return names if r.startswith(tuple(x+"/" for x in root)) else [f"{p}/{x}" for p in root for x in names]
def gaudio(r,k):
    r=n(r);_,e=os.path.splitext(r); names=[r] if e in AUDIO else [r+x for x in AUDIO]
    return [f"{p}/{x}" for p in (k,f"assets/{k}",f"shared/{k}",f"assets/shared/{k}",f"preload/{k}",f"assets/preload/{k}") for x in names]
def shader(r):
    r=n(r);_,e=os.path.splitext(r);names=[r] if e else [r+".frag",r+".vert"]
    return [f"{p}/{x}" for p in ("shaders","data/shaders","shared/shaders","assets/shaders","assets/data/shaders","assets/shared/shaders") for x in names]
def childscript(r):
    r=n(r)+("" ) if n(r).endswith(".lua") else n(r)+".lua";return [r,f"scripts/{r}",f"assets/scripts/{r}",f"assets/preload/scripts/{r}"]
LUA=[("image",re.compile(r"\bmake(?:Animated)?LuaSprite\s*\(\s*[^,]+,\s*['\"]([^'\"]+)['\"]",re.I)),("image",re.compile(r"\b(?:precacheImage)\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),("sound",re.compile(r"\b(?:playSound|precacheSound)\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),("music",re.compile(r"\bplayMusic\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),("shader",re.compile(r"\binitLuaShader\s*\(\s*['\"]([^'\"]+)['\"]",re.I)),("script",re.compile(r"\baddLuaScript\s*\(\s*['\"]([^'\"]+)['\"]",re.I))]
def meta(path):
    try:z=path.stat().st_size
    except OSError:return {},False
    if z<=16*1024*1024:
        try:
            r=json.loads(path.read_text(encoding="utf-8",errors="replace"));s=r.get("song",r) if isinstance(r,dict) else {};d={k:s.get(k) for k in ("song","needsVoices","player1","player2","gfVersion","stage")};d["notes"]=set();d["events"]=set()
            for sec in s.get("notes",[]) if isinstance(s,dict) else []:
                for q in sec.get("sectionNotes",[]) if isinstance(sec,dict) else []:
                    if isinstance(q,list) and len(q)>3 and isinstance(q[3],str):d["notes"].add(q[3])
            for ev in s.get("events",[]) if isinstance(s,dict) else []:
                if isinstance(ev,list) and len(ev)>1 and isinstance(ev[1],list):
                    for q in ev[1]:
                        if isinstance(q,list) and q and isinstance(q[0],str):d["events"].add(q[0])
            return d,True
        except Exception:pass
    try:t=path.open("rb").read(2*1024*1024).decode("utf-8",errors="replace")
    except OSError:return {},False
    d={"notes":set(),"events":set()}
    for k in ("song","player1","player2","gfVersion","stage"):
        m=re.search(rf'"{k}"\s*:\s*"([^"]+)"',t,re.I)
        if m:d[k]=m.group(1)
    m=re.search(r'"needsVoices"\s*:\s*(true|false)',t,re.I)
    if m:d["needsVoices"]=m.group(1).lower()=="true"
    return d,False
def audit(project,mod,desc):
    project=project.resolve();mod=mod.resolve();err=[];warn=[]
    if not mod.is_dir():return {},[f"missing mod root: {mod}"]
    files=[p for p in mod.rglob("*") if p.is_file()]
    if len(files)>600000:return {},["payload exceeds 600000-file audit bound"]
    total=sum(lsize(p) for p in files)
    if len(files)<int(desc.get("min_files",0)):err.append(f"{len(files)} files < descriptor minimum {desc.get('min_files')}")
    if total<int(desc.get("min_bytes",0)):err.append(f"{total} bytes < descriptor minimum {desc.get('min_bytes')}")
    local=idx(mod,files);sp=stock_provider(project);stock=set()
    if sp:
        a=Path(sp["assetsRoot"]);stock=idx(a.parent,[p for p in a.rglob("*") if p.is_file()])
    def ok(c,allow=True):return has(local,c) or bool(allow and stock and has(stock,c))
    charts=[];notes=set();events=set()
    for p in files:
        if p.suffix.lower()!=".json":continue
        rel=p.relative_to(mod);parts=list(rel.parts);low=[x.lower() for x in parts]
        if "data" not in low[:-1]:continue
        i=max(i for i,x in enumerate(low[:-1]) if x=="data")
        if i+1>=len(parts)-1:continue
        folder=song_id(parts[i+1]);stem=song_id(p.stem)
        if not folder or stem=="events" or folder not in stem:continue
        d,full=meta(p);sid=song_id(d.get("song") or folder);entry={"chart":str(p.relative_to(project)),"song":sid,"full":full}
        if not ok(audio(sid,"inst"),sid in STOCK_SONGS):err.append(f"{entry['chart']}: missing Inst for '{sid}'")
        if d.get("needsVoices") is True and not ok(voices(sid),sid in STOCK_SONGS):err.append(f"{entry['chart']}: needsVoices=true but Voices missing for '{sid}'")
        if d.get("needsVoices") is None:warn.append(f"{entry['chart']}: needsVoices not inspectable within bound")
        for k in ("player1","player2","gfVersion"):
            v=d.get(k)
            if isinstance(v,str) and v and not (n(v) in STOCK_CHARS or n(v).startswith("bf") or n(v).startswith("gf")) and not ok(chars(v),False):err.append(f"{entry['chart']}: custom character '{v}' missing")
        v=d.get("stage")
        if isinstance(v,str) and v and n(v) not in STOCK_STAGES and not ok(stages(v),False):err.append(f"{entry['chart']}: custom stage '{v}' missing")
        notes.update(d.get("notes",set()));events.update(d.get("events",set()));charts.append(entry)
    for v in notes:
        if n(v) not in BUILTIN_NOTES and not ok(scripts("custom_notetypes",v),False):err.append(f"custom note type '{v}' script missing")
    for v in events:
        if n(v) not in BUILTIN_EVENTS and not ok(scripts("custom_events",v),False):err.append(f"custom event '{v}' script missing")
    for p in files:
        if p.suffix.lower()!=".lua":continue
        try:
            if p.stat().st_size>4*1024*1024:warn.append(f"{p.relative_to(project)}: Lua too large for static ref audit");continue
            t=p.read_text(encoding="utf-8",errors="replace")
        except OSError:continue
        for kind,pat in LUA:
            for m in pat.finditer(t):
                r=m.group(1);c={"image":img(r),"sound":gaudio(r,"sounds"),"music":gaudio(r,"music"),"shader":shader(r),"script":childscript(r)}[kind];allow=kind!="script"
                if not ok(c,allow):err.append(f"{p.relative_to(project)}:{t.count(chr(10),0,m.start())+1}: unresolved {kind} '{r}'"+(" (no stock provider)" if allow and not sp else ""))
    if charts and len(files)<=2:err.append("chart-bearing payload is metadata/charts only; functional assets are missing")
    return {"format":"pulseforge-complete-mod-audit-v2","slug":desc.get("slug"),"files":len(files),"bytes":total,"stockProvider":sp,"charts":charts,"warnings":warn,"errors":err,"ok":not err},err
def selftest():
    with tempfile.TemporaryDirectory() as td:
        r=Path(td);m=r/"mods"/"x";(m/"data"/"custom").mkdir(parents=True);(m/"songs"/"custom").mkdir(parents=True);(m/"characters").mkdir();(m/"stages").mkdir();(m/"custom_notetypes").mkdir();(m/"custom_events").mkdir();(m/"images").mkdir();(m/"scripts").mkdir()
        j={"song":{"song":"custom","needsVoices":True,"player1":"bf","player2":"xchar","gfVersion":"gf","stage":"xstage","notes":[{"sectionNotes":[[0,0,0,"X Note"]]}],"events":[[0,[["X Event","",""]]]]}};(m/"data"/"custom"/"custom.json").write_text(json.dumps(j));(m/"songs"/"custom"/"Inst.ogg").write_bytes(b"x");(m/"songs"/"custom"/"Voices.ogg").write_bytes(b"x");(m/"characters"/"xchar.json").write_text("{}");(m/"stages"/"xstage.lua").write_text("");(m/"custom_notetypes"/"X Note.lua").write_text("");(m/"custom_events"/"X Event.lua").write_text("");(m/"images"/"bg.png").write_bytes(b"x");(m/"scripts"/"a.lua").write_text("makeLuaSprite('x','bg',0,0)")
        d={"slug":"x","min_files":1,"min_bytes":1};_,e=audit(r,m,d)
        if e:return 1
        (m/"songs"/"custom"/"Inst.ogg").unlink();_,e=audit(r,m,d)
        return 0 if any("missing Inst" in x for x in e) else 1
def main():
    a=argparse.ArgumentParser();a.add_argument("root",nargs="?",default=".");a.add_argument("--mod-root");a.add_argument("--descriptor");a.add_argument("--output");a.add_argument("--self-test",action="store_true");x=a.parse_args()
    if x.self_test:
        rc=selftest();print("PulseForge Complete mod auditor self-test:","PASS" if rc==0 else "FAIL");return rc
    if not x.mod_root or not x.descriptor:a.error("--mod-root and --descriptor required")
    root=Path(x.root).resolve();mod=Path(x.mod_root);mod=mod if mod.is_absolute() else root/mod;dp=Path(x.descriptor);dp=dp if dp.is_absolute() else root/dp;d=json.loads(dp.read_text(encoding="utf-8"));rep,err=audit(root,mod,d);out=Path(x.output) if x.output else root/"content-audit.json";out=out if out.is_absolute() else root/out;out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(rep,indent=2,ensure_ascii=False),encoding="utf-8");print(f"Complete mod audit {d.get('slug')}: {len(err)} error(s)")
    for e in err[:100]:print("ERROR:",e,file=sys.stderr)
    return 1 if err else 0
if __name__=="__main__":raise SystemExit(main())
