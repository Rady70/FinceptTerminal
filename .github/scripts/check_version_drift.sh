#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# check_version_drift.sh — assert the packaging metadata agrees with CMake.
#
# The single source of truth is fincept-qt/CMakeLists.txt:
#     project(FinceptTerminal VERSION X.Y.Z ...)
#
# Only these application-version fields are authoritative shipped metadata:
#   * Flatpak metainfo: the newest <release version="...">
#   * AppImageBuilder: app_info.version's FINCEPT_VERSION fallback
#   * AppImageBuilder: AppImage.file_name's FINCEPT_VERSION fallback
#   * Linux AppStream data: the newest <release version="...">
#
# Do not replace these checks with a repository-wide dotted-number search.
# Packaging files contain comments, desktop-entry format versions, dependency
# versions, and historical release entries. None of those proves that the
# current bundle's advertised application version matches the CMake binary.
# Historical <release> entries remain valid; only the newest entry is checked.
#
# Usage: check_version_drift.sh [repo-root]   (default: cwd)
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

ROOT="${1:-.}"
CMAKELISTS="${ROOT}/fincept-qt/CMakeLists.txt"
PKG_DIR="${ROOT}/fincept-qt/packaging"

if [ ! -f "$CMAKELISTS" ]; then
    echo "::error::${CMAKELISTS} not found"
    exit 1
fi

mapfile -t CMAKE_VERSIONS < <(
    sed -nE 's/^[[:space:]]*project\(FinceptTerminal[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+)([[:space:]]|$).*/\1/p' "$CMAKELISTS"
)
if [ "${#CMAKE_VERSIONS[@]}" -ne 1 ] || [ -z "${CMAKE_VERSIONS[0]}" ]; then
    echo "::error::expected exactly one active project(FinceptTerminal VERSION ...) in ${CMAKELISTS}"
    exit 1
fi
VERSION="${CMAKE_VERSIONS[0]}"
echo "CMake project version: ${VERSION}"

if [ ! -d "$PKG_DIR" ]; then
    echo "::error::${PKG_DIR} not found"
    exit 1
fi

# Print the first release version inside a real <releases> block. XML comments
# are removed before matching so a commented-out <release> cannot satisfy the
# check. Later entries may describe historical releases.
extract_newest_release_version() {
    awk '
    {
        line = $0
        while (1) {
            if (in_comment) {
                end_pos = index(line, "-->")
                if (end_pos == 0) {
                    line = ""
                    break
                }
                line = substr(line, end_pos + 3)
                in_comment = 0
            }

            open_pos = index(line, "<!--")
            if (open_pos == 0) break
            end_pos = index(substr(line, open_pos + 4), "-->")
            if (end_pos == 0) {
                line = substr(line, 1, open_pos - 1)
                in_comment = 1
                break
            }
            line = substr(line, 1, open_pos - 1) \
                   substr(line, open_pos + 4 + end_pos + 2)
        }

        if (line ~ /^[[:space:]]*<releases([[:space:]]|>)/) {
            in_releases = 1
            next
        }
        if (in_releases && line ~ /^[[:space:]]*<\/releases>[[:space:]]*$/) exit
        if (in_releases && line ~ /^[[:space:]]*<release[[:space:]]+version="[0-9]+\.[0-9]+\.[0-9]+"/) {
            value = line
            sub(/^[[:space:]]*<release[[:space:]]+version="/, "", value)
            sub(/".*$/, "", value)
            print value
            exit
        }
    }' "$1"
}

# AppImageBuilder uses a supported !ENV expression. Extract only the value
# under AppDir.app_info, not a comment or an unrelated YAML version field.
extract_appimage_app_info_version() {
    awk '
        /^  app_info:[[:space:]]*$/ { in_app_info = 1; next }
        in_app_info && /^[^[:space:]]/ { in_app_info = 0 }
        in_app_info && /^    version:[[:space:]]*!ENV[[:space:]]+\$\{FINCEPT_VERSION:-[0-9]+\.[0-9]+\.[0-9]+\}[[:space:]]*$/ { print }
    ' "$1" |
        sed -nE 's/^[[:space:]]*version:[[:space:]]*!ENV[[:space:]]+\$\{FINCEPT_VERSION:-([0-9]+\.[0-9]+\.[0-9]+)\}[[:space:]]*$/\1/p'
}

# Extract only AppImage.file_name's version-bearing !ENV expression.
extract_appimage_file_name_version() {
    awk '
        /^AppImage:[[:space:]]*$/ { in_appimage = 1; next }
        in_appimage && /^[^[:space:]]/ { in_appimage = 0 }
        in_appimage && /^  file_name:[[:space:]]*FinceptTerminal-!ENV[[:space:]]+\$\{FINCEPT_VERSION:-[0-9]+\.[0-9]+\.[0-9]+\}-x86_64\.AppImage[[:space:]]*$/ { print }
    ' "$1" |
        sed -nE 's/^[[:space:]]*file_name:[[:space:]]*FinceptTerminal-!ENV[[:space:]]+\$\{FINCEPT_VERSION:-([0-9]+\.[0-9]+\.[0-9]+)\}-x86_64\.AppImage[[:space:]]*$/\1/p'
}

DRIFT=0
check_version_field() {
    local file="$1"
    local label="$2"
    local extractor="$3"
    local display_file="${file#"$ROOT"/}"
    local -a values=()

    if [ ! -f "$file" ]; then
        echo "::error::missing ${label}: ${display_file}"
        DRIFT=1
        return
    fi

    mapfile -t values < <("$extractor" "$file")
    if [ "${#values[@]}" -ne 1 ] || [ -z "${values[0]}" ]; then
        echo "::error::could not resolve exactly one ${label} in ${display_file}"
        DRIFT=1
    elif [ "${values[0]}" != "$VERSION" ]; then
        echo "::error::version drift in ${display_file}: ${label} is ${values[0]}, expected ${VERSION}"
        DRIFT=1
    else
        echo "OK: ${display_file} ${label}=${VERSION}"
    fi
}

check_version_field \
    "$PKG_DIR/flatpak/in.fincept.FinceptTerminal.metainfo.xml" \
    "newest Flatpak release version" extract_newest_release_version
check_version_field \
    "$PKG_DIR/linux/AppImageBuilder.yml" \
    "AppImage app_info.version fallback" extract_appimage_app_info_version
check_version_field \
    "$PKG_DIR/linux/AppImageBuilder.yml" \
    "AppImage file_name version fallback" extract_appimage_file_name_version
check_version_field \
    "$PKG_DIR/linux/fincept-terminal.appdata.xml" \
    "newest Linux AppStream release version" extract_newest_release_version

# The recipe permits an explicit override at packaging time. If supplied, it
# must still name the same release; an unset or empty variable uses the checked
# recipe fallback.
if [ -n "${FINCEPT_VERSION:-}" ] && [ "${FINCEPT_VERSION}" != "$VERSION" ]; then
    echo "::error::FINCEPT_VERSION=${FINCEPT_VERSION} does not match CMake project version ${VERSION}"
    DRIFT=1
fi

if [ "$DRIFT" -ne 0 ]; then
    echo "::error::packaging metadata is out of sync with CMakeLists.txt (${VERSION}). Update the files listed above."
    exit 1
fi
echo "Packaging metadata is consistent with ${VERSION}."
