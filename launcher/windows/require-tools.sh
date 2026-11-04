# Sourced by launch.sh and build-windows.sh. Checks for the Windows build tools up front and names
# whatever is missing, instead of letting `set -e` end the build without a word. Sets PYTHON.

# Any later failure reports the command and line instead of exiting silently.
set -E
trap 'echo "error: \"$BASH_COMMAND\" failed (exit $?) at line $LINENO of $(basename "${BASH_SOURCE[0]}")" >&2' ERR

require_windows_build_tools() {
    local missing=() packages=() native=0 compiler
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) native=1 ;; esac
    if [ "$native" = 1 ] && [ "${MSYSTEM:-}" != "UCRT64" ]; then
        echo "Run this from the MSYS2 UCRT64 shell (MSYSTEM is '${MSYSTEM:-unset}'), or use launch.cmd." >&2
        return 1
    fi
    if [ -z "${PYTHON:-}" ]; then
        PYTHON=$(command -v python3 || command -v python || true)
    fi
    [ -n "$PYTHON" ] || { missing+=(python); packages+=(mingw-w64-ucrt-x86_64-python); }
    if [ "$native" = 1 ]; then
        compiler=g++
        command -v gcc >/dev/null && command -v g++ >/dev/null && command -v windres >/dev/null \
            || { missing+=(gcc); packages+=(mingw-w64-ucrt-x86_64-gcc); }
    else
        compiler=x86_64-w64-mingw32-g++
        command -v x86_64-w64-mingw32-g++-posix >/dev/null || command -v x86_64-w64-mingw32-g++ >/dev/null \
            || missing+=("MinGW-w64 cross compiler (x86_64-w64-mingw32-g++)")
    fi
    command -v cmake >/dev/null || { missing+=(cmake); packages+=(mingw-w64-ucrt-x86_64-cmake); }
    command -v ninja >/dev/null || { missing+=(ninja); packages+=(mingw-w64-ucrt-x86_64-ninja); }
    command -v git >/dev/null || { missing+=(git); packages+=(git); }
    command -v curl >/dev/null || { missing+=(curl); packages+=(curl); }
    command -v tar >/dev/null || { missing+=(tar); packages+=(tar); }
    if [ "${#missing[@]}" -gt 0 ]; then
        echo "The Windows build needs tools that are not installed: ${missing[*]}." >&2
        if [ "$native" = 1 ]; then
            # In MSYS2, offer to install them, but only with the user's go-ahead.
            if install_msys2_packages "${packages[@]}"; then
                unset PYTHON
                require_windows_build_tools
                return
            fi
            echo "Install them in the MSYS2 UCRT64 shell, then try again:" >&2
            echo "  pacman -S --needed ${packages[*]}" >&2
        else
            echo "Install them with your package manager (Debian/Ubuntu: apt install g++-mingw-w64-x86-64-posix cmake ninja-build git curl python3)." >&2
        fi
        return 1
    fi
    export PYTHON
}

# Asks before installing anything; never installs when nobody can answer (CI, piped input).
install_msys2_packages() {
    [ -t 0 ] && [ -z "${openbg_tools_offered:-}" ] || return 1
    openbg_tools_offered=1
    local answer
    echo "MSYS2 can install them now (about 1 GB download):" >&2
    echo "  pacman -S --needed $*" >&2
    read -r -p "Install these packages now? [Y/n] " answer || return 1
    case "$answer" in ""|[Yy]|[Yy][Ee][Ss]) ;; *) return 1 ;; esac
    if ! pacman -S --needed --noconfirm "$@"; then
        # A freshly installed MSYS2 can have an outdated package list; update it, then retry.
        echo "Updating MSYS2's package list and retrying…" >&2
        pacman -Syu --noconfirm || true
        if ! pacman -S --needed --noconfirm "$@"; then
            echo "MSYS2 may have updated its own core files and needs a fresh start: close this window and run launch.cmd again." >&2
            return 1
        fi
    fi
}
