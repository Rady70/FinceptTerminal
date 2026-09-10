"""Offline pre-flight checks for the Flathub submission.

This is the authoritative semantic implementation used by both the full
pre-flight and the CI version gate. It checks the parts of the packaging
contract that can be verified without a Linux desktop or flatpak-builder:

* every git source is pinned to a full commit;
* the Flatpak application source ref and commit resolve on the remote;
* the pinned source's CMake version and executable are known;
* version fields cannot be satisfied by comments or unrelated numbers;
* the Flatpak launch declarations name the executable the pinned source builds;
* screenshots are pinned and the desktop entry is internally consistent.

Run ``python preflight.py`` for the complete check, or
``python preflight.py --version-only --repo-root <repo>`` for the CI gate.
The latter deliberately omits only the existing screenshot and local-image
checks, which are unrelated to version or executable consistency.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
from xml.dom import Node, minidom

try:
    import yaml
except ImportError as exc:  # pragma: no cover - exercised by the CI environment
    print(f"::error::PyYAML is required for packaging validation: {exc}")
    raise SystemExit(1)


D = Path(__file__).resolve().parent
APPID = "in.fincept.FinceptTerminal"
SEMVER_RE = re.compile(r"\d+\.\d+\.\d+\Z")
FULL_SHA_RE = re.compile(r"[0-9a-fA-F]{40}\Z")
PROJECT_VERSION_RE = re.compile(
    r"^[ \t]*project\(FinceptTerminal[ \t]+VERSION[ \t]+"
    r"(\d+\.\d+\.\d+)(?:[ \t]|$)",
    re.MULTILINE,
)
OUTPUT_NAME_RE = re.compile(
    r"^[ \t]*set_target_properties\([ \t]*FinceptTerminal[ \t]+"
    r"PROPERTIES[ \t]+OUTPUT_NAME[ \t]+"
    r'"([A-Za-z0-9][A-Za-z0-9._-]*)"[ \t]*\)',
    re.MULTILINE,
)
EXECUTABLE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\Z")
APPIMAGE_ENV_RE = re.compile(r"\$\{FINCEPT_VERSION:-(\d+\.\d+\.\d+)\}\Z")
APPIMAGE_FILE_RE = re.compile(
    r"[A-Za-z0-9][A-Za-z0-9._-]*-!ENV "
    r"\$\{FINCEPT_VERSION:-(\d+\.\d+\.\d+)\}-x86_64\.AppImage\Z"
)
PACKAGING_SUFFIXES = {
    ".xml", ".yml", ".yaml", ".json", ".desktop", ".spec", ".plist"
}
MAIN_CATEGORIES = {
    "AudioVideo", "Audio", "Video", "Development", "Education", "Game",
    "Graphics", "Network", "Office", "Science", "Settings", "System",
    "Utility",
}

fails: list[str] = []
warns: list[str] = []


def ck(ok: bool, label: str, hard: bool = True) -> None:
    print(f"  [{'PASS' if ok else ('FAIL' if hard else 'WARN')}] {label}")
    if not ok:
        (fails if hard else warns).append(label)


class StrictLoader(yaml.SafeLoader):
    """SafeLoader variant that rejects duplicate YAML mapping keys."""


def strict_mapping(loader, node, deep=False):
    mapping = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise yaml.constructor.ConstructorError(
                "while constructing a mapping", node.start_mark,
                f"found duplicate key: {key!r}", key_node.start_mark,
            )
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


def env_scalar(loader, node):
    # AppImageBuilder uses !ENV on a scalar. The validator needs the literal
    # fallback expression, not environment expansion.
    return loader.construct_scalar(node)


StrictLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, strict_mapping
)
StrictLoader.add_constructor("!ENV", env_scalar)


def load_yaml(path: Path, label: str):
    try:
        with path.open(encoding="utf-8") as fh:
            return yaml.load(fh, Loader=StrictLoader)
    except Exception as exc:
        ck(False, f"{label} parses ({exc})")
        return None


def read_text(path: Path, label: str):
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        ck(False, f"{label} is readable ({exc})")
        return None


def normalize_git_url(url: str | None) -> str:
    value = (url or "").rstrip("/")
    if value.endswith(".git"):
        value = value[:-4]
    if value.startswith("git@github.com:"):
        value = "https://github.com/" + value.split(":", 1)[1]
    elif value.startswith("ssh://git@github.com/"):
        value = "https://github.com/" + value.split(
            "ssh://git@github.com/", 1
        )[1]
    return value.lower()


def run_git(repo: Path, *args: str):
    try:
        return subprocess.run(
            ["git", *args], cwd=repo, capture_output=True, text=True,
            timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None


def remote_ref_values(remote_text: str, wanted: str) -> list[str]:
    return [
        parts[0]
        for line in remote_text.splitlines()
        if len(parts := line.split()) >= 2 and parts[1] == wanted
    ]


def remote_commit_refs(remote_text: str, wanted: str) -> list[str]:
    return [
        parts[1]
        for line in remote_text.splitlines()
        if len(parts := line.split()) >= 2 and parts[0] == wanted
    ]


def remote_contains_commit(repo: Path, remote_text: str, commit: str) -> bool:
    if remote_commit_refs(remote_text, commit):
        return True
    # A commit-only Flatpak source need not be the tip of a named ref. It is
    # still fail-closed: the object must be locally inspectable and reachable
    # from at least one ref advertised by the source remote.
    for line in remote_text.splitlines():
        parts = line.split()
        if len(parts) < 2 or not FULL_SHA_RE.fullmatch(parts[0]):
            continue
        ancestor = run_git(repo, "merge-base", "--is-ancestor", commit, parts[0])
        if ancestor is not None and ancestor.returncode == 0:
            return True
    return False


def project_values(cmake_text: str | None) -> list[str]:
    return PROJECT_VERSION_RE.findall(cmake_text or "")


def output_names(cmake_text: str | None) -> list[str]:
    return OUTPUT_NAME_RE.findall(cmake_text or "")


def pinned_source_metadata(repo: Path, commit: str | None) -> tuple[str | None, str | None]:
    if not isinstance(commit, str) or not FULL_SHA_RE.fullmatch(commit):
        return None, None
    exists = run_git(repo, "cat-file", "-e", f"{commit}^{{commit}}")
    if exists is None or exists.returncode != 0:
        return None, None
    shown = run_git(repo, "show", f"{commit}:fincept-qt/CMakeLists.txt")
    if shown is None or shown.returncode != 0:
        return None, None
    versions = project_values(shown.stdout)
    names = output_names(shown.stdout)
    version = versions[0] if len(versions) == 1 else None
    executable = names[0] if len(names) == 1 and EXECUTABLE_RE.fullmatch(names[0]) else None
    return version, executable


def strip_xml_comments(text: str) -> str:
    # Remove comments one at a time so an unterminated comment cannot expose a
    # dotted number to the coverage scan.
    while "<!--" in text:
        before, remainder = text.split("<!--", 1)
        if "-->" not in remainder:
            text = before
            break
        _, after = remainder.split("-->", 1)
        text = before + after
    return text


def strip_packaging_comments(path: Path, text: str) -> str:
    if path.suffix.lower() in {".xml", ".plist"}:
        return strip_xml_comments(text)
    if path.suffix.lower() in {".yml", ".yaml"}:
        lines = []
        for line in text.splitlines():
            if re.match(r"^[ \t]*#", line):
                continue
            lines.append(re.sub(r"[ \t]+#.*$", "", line))
        return "\n".join(lines)
    if path.suffix.lower() in {".desktop", ".spec"}:
        return "\n".join(
            line for line in text.splitlines()
            if not re.match(r"^[ \t]*#", line)
        )
    return text


def check_packaging_path_coverage(pkg_dir: Path, version: str | None) -> None:
    paths = sorted(
        path for path in pkg_dir.rglob("*")
        if path.is_file() and path.suffix.lower() in PACKAGING_SUFFIXES
    )
    ck(bool(paths), f"found {len(paths)} supported packaging metadata paths")
    if version is None:
        return
    for path in paths:
        text = read_text(path, f"packaging path {path.relative_to(pkg_dir.parent.parent)}")
        if text is None:
            continue
        visible = strip_packaging_comments(path, text)
        versions = sorted(set(re.findall(r"\d+\.\d+\.\d+", visible)))
        if versions and version not in versions:
            rel = path.relative_to(pkg_dir.parent.parent).as_posix()
            ck(False, f"{rel} mentions {', '.join(versions)} but not {version}")


def first_text(dom, tag: str) -> str | None:
    nodes = dom.getElementsByTagName(tag)
    for node in nodes:
        if node.firstChild and node.firstChild.nodeValue:
            return node.firstChild.nodeValue.strip()
    return None


def release_versions(dom) -> list[str]:
    containers = dom.getElementsByTagName("releases")
    if len(containers) != 1:
        return []
    return [
        node.getAttribute("version")
        for node in containers[0].childNodes
        if node.nodeType == Node.ELEMENT_NODE and node.tagName == "release"
    ]


def parse_xml_text(text: str, label: str):
    try:
        dom = minidom.parseString(text)
        ck(True, "well-formed XML")
        return dom
    except Exception as exc:
        ck(False, f"{label} is well-formed XML ({exc})")
        return None


def parse_xml(path: Path, label: str):
    text = read_text(path, label)
    return parse_xml_text(text, label) if text is not None else None


def parse_desktop_text(text: str):
    active = False
    values: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip("\r")
        if line.startswith("[") and line.endswith("]"):
            active = line == "[Desktop Entry]"
            continue
        if active and line and not line.lstrip().startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


def parse_desktop(path: Path):
    text = read_text(path, "desktop entry")
    return parse_desktop_text(text) if text is not None else {}


def pinned_source_file(repo: Path, commit: str | None, relative_path: str, label: str):
    if not isinstance(commit, str) or not FULL_SHA_RE.fullmatch(commit):
        ck(False, f"{label} commit is valid")
        return None
    shown = run_git(repo, "show", f"{commit}:{relative_path}")
    if shown is None or shown.returncode != 0:
        ck(False, f"{label} exists in pinned source")
        return None
    return shown.stdout


def check_application_source(repo: Path, manifest: dict, origin_url: str):
    print("[2] git sources pinned (Flathub: tag without commit = ERROR for new submissions)")
    modules = manifest.get("modules") if isinstance(manifest, dict) else None
    if not isinstance(modules, list):
        ck(False, "manifest modules list declared")
        modules = []
    sources = [
        (module.get("name", "?"), source)
        for module in modules
        if isinstance(module, dict)
        for source in module.get("sources", [])
        if isinstance(source, dict) and source.get("type") == "git"
    ]
    ck(len(sources) > 0, f"found {len(sources)} git sources")
    for name, source in sources:
        raw_url = source.get("url", "")
        url = raw_url.rsplit("/", 1)[-1] if raw_url else "missing-url"
        ck(bool(raw_url), f"{name}:{url} url declared")
        if "tag" in source:
            ck("commit" in source, f"{name}:{url} has tag -> must also have commit")
        else:
            ck("commit" in source, f"{name}:{url} pinned by commit")
        if "commit" in source:
            commit = source.get("commit")
            ck(
                isinstance(commit, str) and bool(FULL_SHA_RE.fullmatch(commit)),
                f"{name}:{url} commit is full 40-char SHA",
            )

    app_sources = [
        (name, source) for name, source in sources
        if isinstance(source.get("url"), str)
        and source["url"].rstrip("/").endswith("/FinceptTerminal.git")
    ]
    ck(len(app_sources) == 1, f"exactly one application source ({len(app_sources)} found)")
    if len(app_sources) != 1:
        return None, None, None, None
    module_name, source = app_sources[0]
    ck(module_name == "FinceptTerminal", "application source belongs to FinceptTerminal module")

    print("[3] built ref actually exists on the remote")
    source_url = source.get("url", "")
    ck(bool(origin_url), "repository origin is available for application source")
    if origin_url:
        ck(
            normalize_git_url(source_url) == normalize_git_url(origin_url),
            "application source URL matches repository origin",
        )

    remote_result = run_git(repo, "ls-remote", source_url) if source_url else None
    remote_text = remote_result.stdout if remote_result and remote_result.returncode == 0 else ""
    ck(bool(remote_text.strip()), "application source remote resolves")

    commit = source.get("commit")
    commit_ok = isinstance(commit, str) and bool(FULL_SHA_RE.fullmatch(commit))
    tag_present = "tag" in source
    branch_present = "branch" in source
    ck(not (tag_present and branch_present), "application source has at most one named ref")
    if tag_present:
        tag = source.get("tag")
        tag_ok = isinstance(tag, str) and bool(
            SEMVER_RE.fullmatch(tag[1:] if tag.startswith("v") else tag)
        )
        ck(tag_ok, f"application source tag is semantic ({tag})")
        if tag_ok and commit_ok:
            tag_ref = f"refs/tags/{tag}"
            tag_shas = remote_ref_values(remote_text, tag_ref + "^{}")
            if not tag_shas:
                tag_shas = remote_ref_values(remote_text, tag_ref)
            ck(len(tag_shas) == 1, f"tag {tag} exists exactly once on application source remote")
            if len(tag_shas) == 1:
                ck(tag_shas[0].lower() == commit.lower(), f"commit matches {tag} ({tag_shas[0][:8]})")
    elif branch_present:
        branch = source.get("branch")
        branch_ok = isinstance(branch, str) and bool(re.fullmatch(r"[A-Za-z0-9._/-]+", branch))
        ck(branch_ok, f"application source branch is valid ({branch})")
        if branch_ok and commit_ok:
            branch_shas = remote_ref_values(remote_text, f"refs/heads/{branch}")
            ck(
                len(branch_shas) == 1,
                f"branch {branch} exists exactly once on application source remote",
            )
            if len(branch_shas) == 1:
                ck(
                    branch_shas[0].lower() == commit.lower(),
                    f"commit matches {branch} ({branch_shas[0][:8]})",
                )
    elif commit_ok:
        ck(
            remote_contains_commit(repo, remote_text, commit),
            "application source commit is advertised or reachable from its remote",
        )
    else:
        ck(False, "application source has a resolvable commit")

    source_version, source_executable = pinned_source_metadata(repo, commit)
    ck(source_version is not None, "pinned application source CMake version resolves")
    ck(source_executable is not None, "pinned application source executable resolves")
    return source, source_version, source_executable, sources


def metainfo_binaries(metainfo) -> list[str]:
    return [
        node.firstChild.nodeValue.strip()
        for node in metainfo.getElementsByTagName("binary")
        if node.firstChild and node.firstChild.nodeValue
    ] if metainfo is not None else []


def desktop_exec_binary(desktop: dict) -> str:
    try:
        return shlex.split(desktop.get("Exec", ""), posix=True)[0]
    except (ValueError, IndexError):
        return ""


def check_launch_contract(
    manifest: dict,
    desktop: dict,
    metainfo,
    pinned_desktop: dict,
    pinned_metainfo,
    executable: str | None,
) -> None:
    """Check the packaging declarations that cross the pinned source boundary."""
    command = manifest.get("command") if isinstance(manifest, dict) else None
    exec_binary = desktop_exec_binary(pinned_desktop)
    provided = metainfo_binaries(pinned_metainfo)
    expected = executable or "<unresolved>"
    ck(command == executable, f"manifest command {command!r} == pinned executable {expected}")
    ck(exec_binary == executable, f"pinned desktop Exec {exec_binary!r} == pinned executable {expected}")
    ck(
        len(provided) == 1 and provided[0] == executable,
        f"pinned metainfo provides exactly the pinned executable ({expected})",
    )
    # main.cpp sets QApplication's application name to the CMake output name;
    # the X11/desktop startup class must therefore use that same identity.
    ck(
        pinned_desktop.get("StartupWMClass") == executable,
        f"pinned desktop StartupWMClass == pinned executable {expected}",
    )
    # The submitted checkout should carry the same files that the manifest's
    # pinned source will install. Keep this local comparison separate from the
    # source-boundary checks above so either side cannot hide drift.
    ck(
        desktop_exec_binary(desktop) == command,
        "working-tree desktop Exec == manifest command",
    )
    ck(
        metainfo_binaries(metainfo) == provided,
        "working-tree metainfo binaries == pinned source metainfo binaries",
    )


def check_version_field(value: str | None, version: str | None, label: str) -> None:
    if value is None:
        ck(False, f"could not resolve exactly one {label}")
    elif version is None or value != version:
        ck(False, f"version drift: {label} is {value}, expected {version}")
    else:
        ck(True, f"{label}={version}")


def appimage_fallbacks(recipe: dict):
    appdir = recipe.get("AppDir") if isinstance(recipe, dict) else None
    app_info = appdir.get("app_info") if isinstance(appdir, dict) else None
    app_info = app_info if isinstance(app_info, dict) else {}
    appimage = recipe.get("AppImage") if isinstance(recipe, dict) else None
    appimage = appimage if isinstance(appimage, dict) else {}
    app_version = app_info.get("version")
    file_name = appimage.get("file_name")
    app_match = APPIMAGE_ENV_RE.fullmatch(app_version) if isinstance(app_version, str) else None
    file_match = APPIMAGE_FILE_RE.fullmatch(file_name) if isinstance(file_name, str) else None
    return (
        app_match.group(1) if app_match else None,
        file_match.group(1) if file_match else None,
    )


def check_desktop_basics(desktop: dict, manifest: dict) -> None:
    ck(desktop.get("Type") == "Application", "Type=Application")
    ck(bool(desktop.get("Name")), "Name present")
    ck(desktop.get("Icon") == APPID, f"Icon == app id ({APPID})")
    exec_value = desktop.get("Exec", "")
    try:
        exec_binary = shlex.split(exec_value, posix=True)[0]
    except (ValueError, IndexError):
        exec_binary = ""
    ck(exec_binary == manifest.get("command"), "Exec binary == manifest command")
    categories = [category for category in desktop.get("Categories", "").split(";") if category]
    ck(bool(set(categories) & MAIN_CATEGORIES), f"has a main category ({sorted(set(categories) & MAIN_CATEGORIES)})")
    ck("MimeType" not in desktop or bool(desktop.get("MimeType")), "no empty MimeType")
    if "MimeType" in desktop:
        ck(False, "declares MimeType with no shared-mime-info XML installed", hard=False)
    ck("%" not in exec_value, "Exec has no field code the app cannot honour")


def parse_release_file(path: Path, label: str):
    dom = parse_xml(path, label)
    return dom, release_versions(dom) if dom is not None else []


def main(argv: list[str] | None = None) -> int:
    global fails, warns
    fails, warns = [], []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version-only", action="store_true")
    parser.add_argument("--repo-root", type=Path, default=None)
    args = parser.parse_args(argv)

    repo = (args.repo_root or D.parents[2]).resolve()
    cmake_path = repo / "fincept-qt" / "CMakeLists.txt"
    cmake_text = read_text(cmake_path, "working-tree CMakeLists.txt")
    current_versions = project_values(cmake_text)
    current_version = current_versions[0] if len(current_versions) == 1 else None
    ck(len(current_versions) == 1, "working-tree CMake project version resolves")
    if current_version:
        print(f"CMake project version: {current_version}")

    pkg_dir = repo / "fincept-qt" / "packaging"
    ck(pkg_dir.is_dir(), f"packaging directory exists ({pkg_dir})")
    if not pkg_dir.is_dir():
        return 1
    check_packaging_path_coverage(pkg_dir, current_version)

    flatpak_dir = repo / "fincept-qt" / "packaging" / "flatpak"
    manifest_path = flatpak_dir / f"{APPID}.yml"
    print("[1] manifest YAML")
    manifest = load_yaml(manifest_path, "manifest YAML")
    if not isinstance(manifest, dict):
        ck(False, "manifest is a YAML mapping")
        manifest = {}
    ck(manifest.get("id") == APPID, f"id matches filename ({APPID})")
    ck(bool(manifest.get("command")), "command declared")
    ck(bool(manifest.get("runtime")) and bool(manifest.get("sdk")), "runtime + sdk declared")

    origin_result = run_git(repo, "config", "--get", "remote.origin.url")
    origin_url = origin_result.stdout.strip() if origin_result and origin_result.returncode == 0 else ""
    source, source_version, source_executable, _ = check_application_source(
        repo, manifest, origin_url
    )

    metainfo_path = flatpak_dir / f"{APPID}.metainfo.xml"
    print("[4] metainfo XML")
    metainfo, metainfo_releases = parse_release_file(metainfo_path, "metainfo")
    ck(first_text(metainfo, "id") == APPID if metainfo is not None else False, "metainfo id matches app id")
    for tag in ("name", "summary", "metadata_license", "project_license"):
        ck(bool(first_text(metainfo, tag)) if metainfo is not None else False, f"<{tag}> present")
    if metainfo is not None:
        ck(bool(metainfo.getElementsByTagName("content_rating")), "has content_rating (OARS)")
        launch = metainfo.getElementsByTagName("launchable")
        ck(
            bool(launch) and bool(launch[0].firstChild)
            and launch[0].firstChild.nodeValue.strip() == f"{APPID}.desktop",
            "launchable points at the desktop id",
        )
    else:
        ck(False, "metainfo content can be inspected")

    print("[5] screenshots not on a mutable branch")
    images = []
    if metainfo is not None:
        images = [
            node.firstChild.nodeValue.strip()
            for node in metainfo.getElementsByTagName("image")
            if node.firstChild and node.firstChild.nodeValue
        ]
    if args.version_only:
        print("  [SKIP] screenshot and local-image checks in version-only mode")
    else:
        ck(bool(metainfo is not None and metainfo.getElementsByTagName("screenshot")), "has screenshots")
        ck(len(images) > 0, f"{len(images)} screenshot URLs")
        for url in images:
            ck("/main/" not in url and "/master/" not in url, f"pinned (not a branch): .../{url.rsplit('/', 1)[-1]}")

    print("[6] newest listed release matches what the manifest builds")
    ck(bool(metainfo_releases), f"releases listed: {metainfo_releases[:3]}")
    ck(
        bool(metainfo_releases) and all(bool(SEMVER_RE.fullmatch(value)) for value in metainfo_releases),
        "all listed release versions are semantic",
    )
    ck(source_version is not None, f"built version resolves from pinned source ({source_version})")
    if source_version is not None and current_version is not None:
        ck(
            source_version == current_version,
            f"built version {source_version} == working-tree CMake version {current_version}",
        )
    ck(
        bool(metainfo_releases) and source_version is not None and metainfo_releases[0] == source_version,
        f"newest release {metainfo_releases[0] if metainfo_releases else None} == built version {source_version}",
    )
    if metainfo is not None:
        for release in metainfo.getElementsByTagName("release"):
            ck(
                bool(re.fullmatch(r"\d{4}-\d{2}-\d{2}", release.getAttribute("date"))),
                f"release {release.getAttribute('version')} date is ISO-8601",
            )

    print("[7] desktop entry and Flatpak launch contract")
    desktop = parse_desktop(flatpak_dir / f"{APPID}.desktop")
    check_desktop_basics(desktop, manifest)
    source_commit = source.get("commit") if isinstance(source, dict) else None
    pinned_desktop_text = pinned_source_file(
        repo, source_commit, "fincept-qt/packaging/flatpak/in.fincept.FinceptTerminal.desktop",
        "pinned Flatpak desktop entry",
    )
    pinned_metainfo_text = pinned_source_file(
        repo, source_commit, "fincept-qt/packaging/flatpak/in.fincept.FinceptTerminal.metainfo.xml",
        "pinned Flatpak metainfo",
    )
    pinned_desktop = parse_desktop_text(pinned_desktop_text) if pinned_desktop_text is not None else {}
    pinned_metainfo = parse_xml_text(pinned_metainfo_text, "pinned Flatpak metainfo") if pinned_metainfo_text is not None else None
    check_launch_contract(
        manifest, desktop, metainfo, pinned_desktop, pinned_metainfo, source_executable
    )

    print("[8] other authoritative packaging version fields")
    appimage_path = repo / "fincept-qt" / "packaging" / "linux" / "AppImageBuilder.yml"
    appimage_recipe = load_yaml(appimage_path, "AppImageBuilder YAML")
    appimage_version, filename_version = appimage_fallbacks(appimage_recipe or {})
    check_version_field(appimage_version, current_version, "AppImage app_info.version fallback")
    check_version_field(filename_version, current_version, "AppImage file_name version fallback")
    appstream_path = repo / "fincept-qt" / "packaging" / "linux" / "fincept-terminal.appdata.xml"
    _, appstream_releases = parse_release_file(appstream_path, "Linux AppStream metadata")
    check_version_field(
        appstream_releases[0] if appstream_releases else None,
        current_version,
        "newest Linux AppStream release version",
    )

    if not args.version_only:
        print("[9] installed assets exist")
        icon = repo / "fincept-qt" / "resources" / f"{APPID}.png"
        ck(icon.is_file(), "256x256 icon present")
        for url in images:
            relative = url.split("/images/")[-1]
            ck((repo / "images" / relative).is_file(), f"screenshot source images/{relative}")

    if os.environ.get("FINCEPT_VERSION") and os.environ["FINCEPT_VERSION"] != current_version:
        ck(
            False,
            f"FINCEPT_VERSION={os.environ['FINCEPT_VERSION']} does not match CMake project version {current_version}",
        )

    print(f"\n{len(fails)} failure(s), {len(warns)} warning(s)")
    for failure in fails:
        print("  FAIL:", failure)
    for warning in warns:
        print("  WARN:", warning)
    if fails:
        return 1
    print(f"Packaging metadata is consistent with {current_version}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
