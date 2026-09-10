#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# check_version_drift.sh — assert the packaging metadata agrees with CMake.
#
# The single source of truth is fincept-qt/CMakeLists.txt:
#     project(FinceptTerminal VERSION X.Y.Z ...)
#
# Only these application-version fields are authoritative shipped metadata:
#   * Flatpak manifest: the application source commit's CMake version
#   * Flatpak metainfo: the newest <release version="...">
#   * AppImageBuilder: app_info.version's FINCEPT_VERSION fallback
#   * AppImageBuilder: AppImage.file_name's FINCEPT_VERSION fallback
#   * Linux AppStream data: the newest <release version="...">
#
# A comment-aware packaging-path scan is retained as a coverage net, but it is
# not a substitute for these semantic checks.
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
FLATPAK_MANIFEST="${PKG_DIR}/flatpak/in.fincept.FinceptTerminal.yml"
FLATPAK_METAINFO="${PKG_DIR}/flatpak/in.fincept.FinceptTerminal.metainfo.xml"

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

# Remove XML comments without letting a commented-out release or version
# satisfy a packaging-path check. This intentionally handles comments that
# span lines and multiple comments on one line.
strip_xml_comments() {
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
        print line
    }' "$1"
}

# Preserve coverage of every supported packaging metadata path while ignoring
# prose/comments. The field-specific checks below remain authoritative for
# files whose historical release entries or non-application numbers need
# semantic handling.
strip_packaging_comments() {
    case "$1" in
        *.xml|*.plist)
            strip_xml_comments "$1"
            ;;
        *.yml|*.yaml)
            sed -E '/^[[:space:]]*#/d; s/[[:space:]]+#.*$//' "$1"
            ;;
        *.desktop|*.spec)
            sed -E '/^[[:space:]]*#/d' "$1"
            ;;
        *)
            sed -n 'p' "$1"
            ;;
    esac
}

check_packaging_path_coverage() {
    local found=0
    local f content

    while IFS= read -r -d '' f; do
        found=1
        grep -Iq . "$f" 2>/dev/null || continue

        if ! content=$(strip_packaging_comments "$f"); then
            echo "::error::could not read packaging path: ${f#"$ROOT"/}"
            DRIFT=1
            continue
        fi

        local -a versions=()
        mapfile -t versions < <(
            printf '%s\n' "$content" |
                grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' |
                sort -u
        )
        [ "${#versions[@]}" -gt 0 ] || continue

        if ! printf '%s\n' "${versions[@]}" | grep -Fxq "$VERSION"; then
            echo "::error::version drift in ${f#"$ROOT"/}: mentions $(printf '%s ' "${versions[@]}")but never ${VERSION}"
            DRIFT=1
        fi
    done < <(find "$PKG_DIR" -type f \
        \( -name '*.xml' -o -name '*.yml' -o -name '*.yaml' -o -name '*.json' \
           -o -name '*.desktop' -o -name '*.spec' -o -name '*.plist' \) -print0)

    if [ "$found" -eq 0 ]; then
        echo "::error::no supported packaging metadata paths found under ${PKG_DIR}"
        DRIFT=1
    fi
}

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
        in_appimage && /^  file_name:[[:space:]]*[A-Za-z0-9][A-Za-z0-9._-]*!ENV[[:space:]]+\$\{FINCEPT_VERSION:-[0-9]+\.[0-9]+\.[0-9]+\}-x86_64\.AppImage[[:space:]]*$/ { print }
    ' "$1" |
        sed -nE 's/^[[:space:]]*file_name:[[:space:]]*[A-Za-z0-9][A-Za-z0-9._-]*!ENV[[:space:]]+\$\{FINCEPT_VERSION:-([0-9]+\.[0-9]+\.[0-9]+)\}-x86_64\.AppImage[[:space:]]*$/\1/p'
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

# Emit one pipe-separated record for each git source in the FinceptTerminal
# module. The module and source boundaries are structural, so a dependency
# source cannot accidentally stand in for the application source.
extract_flatpak_application_source_records() {
    awk '
        function reset_source() {
            in_git = 0
            url = tag = branch = commit = ""
            url_count = tag_count = branch_count = commit_count = 0
        }
        function emit_source() {
            if (in_git) {
                printf "%s|%s|%s|%s|%d|%d|%d|%d\n", \
                    url, tag, branch, commit, \
                    url_count, tag_count, branch_count, commit_count
            }
            reset_source()
        }
        /^  - name:[[:space:]]*/ {
            emit_source()
            name = $0
            sub(/^  - name:[[:space:]]*/, "", name)
            sub(/[[:space:]]*$/, "", name)
            in_app_module = (name == "FinceptTerminal")
            in_sources = 0
            next
        }
        in_app_module && /^    sources:[[:space:]]*$/ {
            emit_source()
            in_sources = 1
            next
        }
        in_app_module && in_sources && /^[ ]{4}[A-Za-z0-9_-]+:/ {
            emit_source()
            in_sources = 0
            next
        }
        in_app_module && in_sources && /^      - type:[[:space:]]*git[[:space:]]*$/ {
            emit_source()
            in_git = 1
            next
        }
        in_app_module && in_sources && /^      - type:/ {
            emit_source()
            next
        }
        in_app_module && in_sources && in_git && /^[ ]{8}[A-Za-z0-9_-]+:/ {
            field = substr($0, 9)
            key = field
            sub(/:.*/, "", key)
            value = field
            sub(/^[^:]*:[[:space:]]*/, "", value)
            if (key == "url") {
                url = value
                url_count++
            } else if (key == "tag") {
                tag = value
                tag_count++
            } else if (key == "branch") {
                branch = value
                branch_count++
            } else if (key == "commit") {
                commit = value
                commit_count++
            }
        }
        END { emit_source() }
    ' "$1"
}

normalize_git_url() {
    local url="$1"
    url="${url%/}"
    url="${url%.git}"
    case "$url" in
        git@github.com:*)
            url="https://github.com/${url#git@github.com:}"
            ;;
        ssh://git@github.com/*)
            url="https://github.com/${url#ssh://git@github.com/}"
            ;;
    esac
    printf '%s\n' "$url" | tr '[:upper:]' '[:lower:]'
}

remote_ref_values() {
    local remote_refs="$1"
    local wanted_ref="$2"
    printf '%s\n' "$remote_refs" | awk -v wanted="$wanted_ref" '$2 == wanted { print $1 }'
}

remote_commit_refs() {
    local remote_refs="$1"
    local wanted_commit="$2"
    printf '%s\n' "$remote_refs" | awk -v wanted="$wanted_commit" '$1 == wanted { print $2 }'
}

check_flatpak_application_source() {
    local module_count
    local source_records=()
    local source_record
    local source_matches=0
    local source_url=""
    local source_tag=""
    local source_branch=""
    local source_commit=""
    local source_url_count=0
    local source_tag_count=0
    local source_branch_count=0
    local source_commit_count=0
    local origin_url=""
    local remote_refs=""
    local remote_ok=0
    local source_cmake=""
    local pinned_source_version=""
    local newest_release_version=""
    local -a source_versions=()
    local -a release_versions=()
    local -a ref_shas=()
    local -a advertised_commits=()

    if [ ! -f "$FLATPAK_MANIFEST" ]; then
        echo "::error::missing Flatpak manifest: ${FLATPAK_MANIFEST#"$ROOT"/}"
        DRIFT=1
        return
    fi

    module_count=$(awk '/^  - name:[[:space:]]*FinceptTerminal[[:space:]]*$/ { count++ } END { print count + 0 }' "$FLATPAK_MANIFEST")
    if [ "$module_count" -ne 1 ]; then
        echo "::error::expected exactly one FinceptTerminal module in ${FLATPAK_MANIFEST#"$ROOT"/}"
        DRIFT=1
    fi

    mapfile -t source_records < <(extract_flatpak_application_source_records "$FLATPAK_MANIFEST")
    for source_record in "${source_records[@]}"; do
        IFS='|' read -r source_url source_tag source_branch source_commit \
            source_url_count source_tag_count source_branch_count source_commit_count <<< "$source_record"
        case "$source_url" in
            */FinceptTerminal.git)
                source_matches=$((source_matches + 1))
                ;;
        esac
    done

    if [ "$source_matches" -ne 1 ]; then
        echo "::error::expected exactly one FinceptTerminal.git application source in ${FLATPAK_MANIFEST#"$ROOT"/}"
        DRIFT=1
        return
    fi

    # Re-read the unique matching record so fields from a non-application git
    # dependency cannot survive the loop above.
    for source_record in "${source_records[@]}"; do
        IFS='|' read -r source_url source_tag source_branch source_commit \
            source_url_count source_tag_count source_branch_count source_commit_count <<< "$source_record"
        case "$source_url" in
            */FinceptTerminal.git) break ;;
        esac
    done

    if [ "$source_url_count" -ne 1 ] || [ "$source_commit_count" -ne 1 ] || \
       [ -z "$source_url" ] || [ -z "$source_commit" ]; then
        echo "::error::Flatpak application source must have exactly one non-empty url and commit"
        DRIFT=1
    fi
    if [ "$source_tag_count" -gt 1 ] || [ "$source_branch_count" -gt 1 ] || \
       { [ "$source_tag_count" -gt 0 ] && [ "$source_branch_count" -gt 0 ]; }; then
        echo "::error::Flatpak application source has ambiguous tag/branch ref"
        DRIFT=1
    fi
    if [ -n "$source_commit" ] && [[ ! "$source_commit" =~ ^[0-9a-fA-F]{40}$ ]]; then
        echo "::error::Flatpak application source commit is not a full 40-character SHA"
        DRIFT=1
    fi

    if [ -z "$source_url" ] || [ -z "$source_commit" ] || \
       [[ ! "$source_commit" =~ ^[0-9a-fA-F]{40}$ ]]; then
        return
    fi

    origin_url=$(git -C "$ROOT" config --get remote.origin.url 2>/dev/null) || origin_url=""
    if [ -z "$origin_url" ]; then
        echo "::error::could not resolve repository origin for Flatpak application source"
        DRIFT=1
    elif [ "$(normalize_git_url "$source_url")" != "$(normalize_git_url "$origin_url")" ]; then
        echo "::error::Flatpak application source URL does not match repository origin"
        DRIFT=1
    fi

    if ! remote_refs=$(git ls-remote "$source_url" 2>/dev/null); then
        echo "::error::could not resolve Flatpak application source remote"
        DRIFT=1
    elif [ -z "$remote_refs" ]; then
        echo "::error::Flatpak application source remote returned no refs"
        DRIFT=1
    else
        remote_ok=1
    fi

    if [ "$remote_ok" -eq 1 ]; then
        if [ "$source_tag_count" -eq 1 ]; then
            if [[ ! "$source_tag" =~ ^v?[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
                echo "::error::Flatpak application source tag is not a semantic version: ${source_tag}"
                DRIFT=1
            else
                mapfile -t ref_shas < <(remote_ref_values "$remote_refs" "refs/tags/${source_tag}^{}")
                if [ "${#ref_shas[@]}" -eq 0 ]; then
                    mapfile -t ref_shas < <(remote_ref_values "$remote_refs" "refs/tags/${source_tag}")
                fi
                if [ "${#ref_shas[@]}" -ne 1 ]; then
                    echo "::error::Flatpak application source tag is missing or ambiguous: ${source_tag}"
                    DRIFT=1
                elif [ "${ref_shas[0],,}" != "${source_commit,,}" ]; then
                    echo "::error::Flatpak application source commit does not match tag ${source_tag}"
                    DRIFT=1
                fi
            fi
        elif [ "$source_branch_count" -eq 1 ]; then
            if [[ ! "$source_branch" =~ ^[A-Za-z0-9._/-]+$ ]]; then
                echo "::error::Flatpak application source branch is invalid"
                DRIFT=1
            else
                mapfile -t ref_shas < <(remote_ref_values "$remote_refs" "refs/heads/${source_branch}")
                if [ "${#ref_shas[@]}" -ne 1 ]; then
                    echo "::error::Flatpak application source branch is missing or ambiguous: ${source_branch}"
                    DRIFT=1
                elif [ "${ref_shas[0],,}" != "${source_commit,,}" ]; then
                    echo "::error::Flatpak application source commit does not match branch ${source_branch}"
                    DRIFT=1
                fi
            fi
        else
            mapfile -t advertised_commits < <(remote_commit_refs "$remote_refs" "$source_commit")
            if [ "${#advertised_commits[@]}" -eq 0 ]; then
                echo "::error::Flatpak application source commit is not advertised by its remote"
                DRIFT=1
            fi
        fi
    fi

    if ! git -C "$ROOT" cat-file -e "${source_commit}^{commit}" 2>/dev/null; then
        echo "::error::pinned Flatpak application source commit is unavailable locally"
        DRIFT=1
    elif ! source_cmake=$(git -C "$ROOT" show "${source_commit}:fincept-qt/CMakeLists.txt" 2>/dev/null); then
        echo "::error::could not read CMakeLists.txt from pinned Flatpak application source"
        DRIFT=1
    else
        mapfile -t source_versions < <(
            printf '%s\n' "$source_cmake" |
                sed -nE 's/^[[:space:]]*project\(FinceptTerminal[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+)([[:space:]]|$).*/\1/p'
        )
        if [ "${#source_versions[@]}" -ne 1 ] || [ -z "${source_versions[0]}" ]; then
            echo "::error::expected exactly one active project version in pinned Flatpak application source"
            DRIFT=1
        else
            pinned_source_version="${source_versions[0]}"
            echo "Pinned Flatpak application source version: ${pinned_source_version}"
            if [ "$pinned_source_version" != "$VERSION" ]; then
                echo "::error::pinned Flatpak application source version is ${pinned_source_version}, expected CMake ${VERSION}"
                DRIFT=1
            fi
        fi
    fi

    if [ -f "$FLATPAK_METAINFO" ]; then
        mapfile -t release_versions < <(extract_newest_release_version "$FLATPAK_METAINFO")
    fi
    if [ "${#release_versions[@]}" -ne 1 ] || [ -z "${release_versions[0]}" ]; then
        echo "::error::could not resolve exactly one newest Flatpak release for source comparison"
        DRIFT=1
    else
        newest_release_version="${release_versions[0]}"
        if [ -n "$pinned_source_version" ] && [ "$pinned_source_version" != "$newest_release_version" ]; then
            echo "::error::newest Flatpak release ${newest_release_version} does not match pinned source version ${pinned_source_version}"
            DRIFT=1
        elif [ -n "$pinned_source_version" ]; then
            echo "OK: Flatpak newest release=${newest_release_version} matches pinned source"
        fi
    fi
}

check_packaging_path_coverage
check_version_field \
    "$FLATPAK_METAINFO" \
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
check_flatpak_application_source

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
