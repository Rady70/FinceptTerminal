"""Offline pre-flight for the Flathub submission.

Checks the subset of flatpak-builder-lint / AppStream rules that can be
verified WITHOUT a Linux box, so the real lint run passes first time:

  * every git source is pinned (Flathub: a tag without a commit is a hard
    error for new submissions -- tags are mutable)
  * the application source URL/ref/commit the manifest builds actually EXISTS
    on the remote, and the pinned commit's CMake version is resolvable
  * screenshots are pinned to a tag/commit, not a mutable branch
  * the newest <release> matches the version the manifest builds, so Flathub
    cannot advertise a version whose binary it does not have
  * the desktop entry is internally consistent with the manifest and declares
    no MIME type or field code the app cannot honour

Run:  python preflight.py       (needs pyyaml; exits non-zero on any failure)

It does NOT replace flatpak-builder-lint -- run that on Linux too. See
FLATHUB.md.
"""
import io, os, re, sys, subprocess, xml.dom.minidom
import yaml

D = os.path.dirname(os.path.abspath(__file__))
# <repo>/fincept-qt/packaging/flatpak -> <repo>
REPO = os.path.abspath(os.path.join(D, "..", "..", ".."))
APPID = "in.fincept.FinceptTerminal"
fails, warns = [], []
SEMVER_RE = re.compile(r"\d+\.\d+\.\d+\Z")
PROJECT_VERSION_RE = re.compile(
    r"^[ \t]*project\(FinceptTerminal[ \t]+VERSION[ \t]+"
    r"(\d+\.\d+\.\d+)(?:[ \t]|$)",
    re.MULTILINE,
)

def ck(ok, label, hard=True):
    print(f"  [{'PASS' if ok else ('FAIL' if hard else 'WARN')}] {label}")
    if not ok:
        (fails if hard else warns).append(label)

def normalize_git_url(url):
    value = (url or "").rstrip("/")
    if value.endswith(".git"):
        value = value[:-4]
    if value.startswith("git@github.com:"):
        value = "https://github.com/" + value.split(":", 1)[1]
    elif value.startswith("ssh://git@github.com/"):
        value = "https://github.com/" + value.split("ssh://git@github.com/", 1)[1]
    return value.lower()

def remote_ref_values(remote_text, wanted):
    return [
        parts[0]
        for line in remote_text.splitlines()
        if len(parts := line.split()) >= 2 and parts[1] == wanted
    ]

def remote_commit_refs(remote_text, wanted):
    return [
        parts[1]
        for line in remote_text.splitlines()
        if len(parts := line.split()) >= 2 and parts[0] == wanted
    ]

def pinned_source_version(commit):
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", commit):
        return None
    exists = subprocess.run(
        ["git", "cat-file", "-e", f"{commit}^{{commit}}"],
        cwd=REPO, capture_output=True, text=True,
    )
    if exists.returncode != 0:
        return None
    shown = subprocess.run(
        ["git", "show", f"{commit}:fincept-qt/CMakeLists.txt"],
        cwd=REPO, capture_output=True, text=True,
    )
    if shown.returncode != 0:
        return None
    versions = PROJECT_VERSION_RE.findall(shown.stdout)
    return versions[0] if len(versions) == 1 else None

print("[1] manifest YAML")
man = yaml.safe_load(io.open(os.path.join(D, f"{APPID}.yml"), encoding="utf-8"))
ck(man["id"] == APPID, f"id matches filename ({APPID})")
ck(bool(man.get("command")), "command declared")
ck(man.get("runtime") and man.get("sdk"), "runtime + sdk declared")

print("[2] git sources pinned (Flathub: tag without commit = ERROR for new submissions)")
srcs = [(m.get("name", "?"), s) for m in man["modules"] if isinstance(m, dict)
        for s in m.get("sources", []) if isinstance(s, dict) and s.get("type") == "git"]
ck(len(srcs) > 0, f"found {len(srcs)} git sources")
for name, s in srcs:
    raw_url = s.get("url", "")
    url = raw_url.rsplit("/", 1)[-1] if raw_url else "missing-url"
    ck(bool(raw_url), f"{name}:{url} url declared")
    if "tag" in s:
        ck("commit" in s, f"{name}:{url} has tag -> must also have commit")
    else:
        ck("commit" in s, f"{name}:{url} pinned by commit")
    if "commit" in s:
        commit = s.get("commit")
        ck(isinstance(commit, str) and bool(re.fullmatch(r"[0-9a-f]{40}", commit)),
           f"{name}:{url} commit is full 40-char SHA")

app_sources = [
    (name, s) for name, s in srcs
    if isinstance(s.get("url"), str)
    and s["url"].rstrip("/").endswith("/FinceptTerminal.git")
]
ck(len(app_sources) == 1, f"exactly one application source ({len(app_sources)} found)")
if len(app_sources) == 1:
    ck(app_sources[0][0] == "FinceptTerminal", "application source belongs to FinceptTerminal module")

print("[3] built ref actually exists on the remote")
app_source = app_sources[0][1] if len(app_sources) == 1 else None
remote_text = ""
built = None
if app_source is not None:
    source_url = app_source.get("url", "")
    origin_result = subprocess.run(
        ["git", "config", "--get", "remote.origin.url"],
        cwd=REPO, capture_output=True, text=True,
    )
    origin_url = origin_result.stdout.strip() if origin_result.returncode == 0 else ""
    ck(bool(origin_url), "repository origin is available for application source")
    if origin_url:
        ck(normalize_git_url(source_url) == normalize_git_url(origin_url),
           "application source URL matches repository origin")

    remote = subprocess.run(
        ["git", "ls-remote", source_url], cwd=REPO,
        capture_output=True, text=True,
    )
    remote_text = remote.stdout if remote.returncode == 0 else ""
    ck(bool(remote_text.strip()), "application source remote resolves")

    commit = app_source.get("commit")
    commit_ok = isinstance(commit, str) and bool(re.fullmatch(r"[0-9a-f]{40}", commit))
    tag_present = "tag" in app_source
    branch_present = "branch" in app_source
    ck(not (tag_present and branch_present), "application source has at most one named ref")

    if tag_present:
        tag = app_source.get("tag")
        tag_ok = isinstance(tag, str) and bool(SEMVER_RE.fullmatch(tag[1:] if tag.startswith("v") else tag))
        ck(tag_ok, f"application source tag is semantic ({tag})")
        if tag_ok and commit_ok:
            tag_ref = f"refs/tags/{tag}"
            tag_shas = remote_ref_values(remote_text, tag_ref + "^{}")
            if not tag_shas:
                tag_shas = remote_ref_values(remote_text, tag_ref)
            ck(len(tag_shas) == 1, f"tag {tag} exists exactly once on application source remote")
            if len(tag_shas) == 1:
                ck(tag_shas[0].lower() == commit.lower(),
                   f"commit matches {tag} ({tag_shas[0][:8]})")
    elif branch_present:
        branch = app_source.get("branch")
        branch_ok = isinstance(branch, str) and bool(re.fullmatch(r"[A-Za-z0-9._/-]+", branch))
        ck(branch_ok, f"application source branch is valid ({branch})")
        if branch_ok and commit_ok:
            branch_shas = remote_ref_values(remote_text, f"refs/heads/{branch}")
            ck(len(branch_shas) == 1, f"branch {branch} exists exactly once on application source remote")
            if len(branch_shas) == 1:
                ck(branch_shas[0].lower() == commit.lower(),
                   f"commit matches {branch} ({branch_shas[0][:8]})")
    elif commit_ok:
        ck(bool(remote_commit_refs(remote_text, commit)),
           "application source commit is advertised by its remote")
    else:
        ck(False, "application source has a resolvable commit")

    built = pinned_source_version(commit)
    ck(built is not None, "pinned application source CMake version resolves")

print("[4] metainfo XML")
mp = os.path.join(D, f"{APPID}.metainfo.xml")
dom = xml.dom.minidom.parse(mp)
xt = io.open(mp, encoding="utf-8").read()
ck(True, "well-formed XML")
def one(tag):
    n = dom.getElementsByTagName(tag)
    return n[0].firstChild.nodeValue.strip() if n and n[0].firstChild else None
ck(one("id") == APPID, "metainfo id matches app id")
for t in ("name", "summary", "metadata_license", "project_license"):
    ck(bool(one(t)), f"<{t}> present")
ck(len(dom.getElementsByTagName("screenshot")) > 0, "has screenshots")
ck(bool(dom.getElementsByTagName("content_rating")), "has content_rating (OARS)")
launch = dom.getElementsByTagName("launchable")
ck(bool(launch) and launch[0].firstChild.nodeValue.strip() == f"{APPID}.desktop",
   "launchable points at the desktop id")

print("[5] screenshots not on a mutable branch")
imgs = [n.firstChild.nodeValue.strip() for n in dom.getElementsByTagName("image") if n.firstChild]
ck(len(imgs) > 0, f"{len(imgs)} screenshot URLs")
for u in imgs:
    ck("/main/" not in u and "/master/" not in u, f"pinned (not a branch): .../{u.rsplit('/',1)[-1]}")

print("[6] newest listed release matches what the manifest builds")
rels = [r.getAttribute("version") for r in dom.getElementsByTagName("release")]
ck(bool(rels), f"releases listed: {rels[:3]}")
ck(bool(rels) and all(bool(SEMVER_RE.fullmatch(v)) for v in rels),
   "all listed release versions are semantic")
current_cmake = io.open(os.path.join(REPO, "fincept-qt", "CMakeLists.txt"), encoding="utf-8").read()
current_versions = PROJECT_VERSION_RE.findall(current_cmake)
ck(len(current_versions) == 1, "working-tree CMake project version resolves")
ck(built is not None, f"built version resolves from pinned source ({built})")
if built is not None and len(current_versions) == 1:
    ck(built == current_versions[0],
       f"built version {built} == working-tree CMake version {current_versions[0]}")
ck(bool(rels) and built is not None and rels[0] == built,
   f"newest release {rels[0] if rels else None} == built version {built}")
for r in dom.getElementsByTagName("release"):
    ck(bool(re.fullmatch(r"\d{4}-\d{2}-\d{2}", r.getAttribute("date"))),
       f"release {r.getAttribute('version')} date is ISO-8601")

print("[7] desktop entry")
dp = os.path.join(D, f"{APPID}.desktop")
kv = dict(l.split("=", 1) for l in io.open(dp, encoding="utf-8").read().splitlines()
          if "=" in l and not l.startswith("["))
ck(kv.get("Type") == "Application", "Type=Application")
ck(bool(kv.get("Name")), "Name present")
ck(kv.get("Icon") == APPID, f"Icon == app id ({APPID})")
ck(kv.get("Exec", "").split()[0] == man["command"], "Exec binary == manifest command")
cats = [c for c in kv.get("Categories", "").split(";") if c]
MAIN = {"AudioVideo","Audio","Video","Development","Education","Game","Graphics",
        "Network","Office","Science","Settings","System","Utility"}
ck(bool(set(cats) & MAIN), f"has a main category ({sorted(set(cats) & MAIN)})")
ck("MimeType" not in kv or bool(kv.get("MimeType")), "no empty MimeType")
if "MimeType" in kv:
    ck(False, "declares MimeType with no shared-mime-info XML installed", hard=False)
ck("%" not in kv.get("Exec", ""), "Exec has no field code the app cannot honour")

print("[8] installed assets exist")
ck(os.path.isfile(os.path.join(REPO, "fincept-qt", "resources", f"{APPID}.png")), "256x256 icon present")
for u in imgs:
    rel = u.split("/images/")[-1]
    ck(os.path.isfile(os.path.join(REPO, "images", rel)), f"screenshot source images/{rel}")

print(f"\n{len(fails)} failure(s), {len(warns)} warning(s)")
if fails:
    for f in fails: print("  FAIL:", f)
if warns:
    for w in warns: print("  WARN:", w)
sys.exit(1 if fails else 0)
