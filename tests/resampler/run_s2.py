import json, subprocess, pathlib, re
import numpy as np, soundfile as sf, jiwer
from scipy.signal import resample_poly

ROOT = pathlib.Path.home() / "s2-resampler"
WH = ROOT / "whisper.cpp"
MODEL = WH / "models" / "ggml-base.en.bin"
CLI = WH / "build" / "bin" / "whisper-cli"
DRV = ROOT / "resample-driver"
manifest = json.load(open(ROOT / "manifest.json"))
work = ROOT / "work"; work.mkdir(exist_ok=True)

def norm(s):
    s = s.lower()
    s = re.sub(r"[^a-z' ]", " ", s)
    return " ".join(s.split())

def transcribe(wav, prefix):
    subprocess.run([str(CLI), "-m", str(MODEL), "-f", str(wav), "-l", "en", "-nt", "-np", "-otxt", "-of", str(prefix)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return norm(open(str(prefix) + ".txt").read())

conds = {"native16k": None, "hann8": "hann8", "kaiser16": "kaiser16", "kaiser32": "kaiser32"}
hyps = {c: {} for c in conds}
refs = {}
for i, item in enumerate(manifest):
    name = item["name"]; refs[name] = norm(item["text"])
    x16, sr = sf.read(item["file"], dtype="float32")
    assert sr == 16000 and x16.ndim == 1
    # Baseline: native 16 kHz clip straight to whisper.
    hyps["native16k"][name] = transcribe(item["file"], work / f"{name}-native16k")
    # Candidates: upsample to 48 kHz (exact 3x, adds no content), then resample 48k -> 16k with the candidate.
    x48 = resample_poly(x16, 3, 1).astype("<f4")
    raw48 = work / f"{name}.48k.raw"; x48.tofile(raw48)
    for c in ["hann8", "kaiser16", "kaiser32"]:
        raw16 = work / f"{name}-{c}.16k.raw"
        subprocess.run([str(DRV), c, str(raw48), str(raw16)], check=True, stdout=subprocess.DEVNULL)
        y = np.fromfile(raw16, dtype="<f4")
        wav = work / f"{name}-{c}.wav"; sf.write(wav, y, 16000, subtype="FLOAT")
        hyps[c][name] = transcribe(wav, work / f"{name}-{c}")
    print(i + 1, name, flush=True)

json.dump({"refs": refs, "hyps": hyps}, open(ROOT / "results.json", "w"), indent=1)
ref_words = sum(len(refs[n].split()) for n in refs)
for c in conds:
    names = list(refs)
    out = jiwer.process_words([refs[n] for n in names], [hyps[c][n] for n in names])
    errs = out.substitutions + out.deletions + out.insertions
    print(f"{c:10s} WER={errs / ref_words * 100:.2f}% errors={errs} ref_words={ref_words}")
