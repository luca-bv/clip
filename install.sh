#!/usr/bin/env bash
# Installs (or updates) Clip from the latest build. Run in Git Bash on Windows 11:
#   curl -fsSL https://raw.githubusercontent.com/luca-bv/clip/main/install.sh | bash
# Uninstall (keeps your clips and settings):
#   curl -fsSL https://raw.githubusercontent.com/luca-bv/clip/main/install.sh | bash -s -- --uninstall
set -euo pipefail

url="https://github.com/luca-bv/clip/releases/latest/download/Clip.zip"

case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*) ;;
  *) echo "Clip is a Windows app. Run this in Git Bash on your Windows PC." >&2; exit 1 ;;
esac

app="$(cygpath -u "$LOCALAPPDATA")/Clip/app"
shortcut="$(cygpath -u "$APPDATA")/Microsoft/Windows/Start Menu/Programs/Clip.lnk"
# Paths reach PowerShell as environment variables, never inside its command text, so a ' in a path is fine.
ps() { powershell.exe -NoProfile -NonInteractive -Command "$1"; }
export CLIP_EXE="$(cygpath -w "$app/Clip.exe")"

# Clip holds its own files open, so stop it first: only the Clip.exe installed here, not anything else named Clip.
ps 'Get-Process Clip -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $env:CLIP_EXE } | Stop-Process -Force' || true
sleep 1
# Moves the installed Clip aside, so nothing is deleted while it's still running (as administrator, say).
move_aside() {
  rm -rf "$app.old"
  if [[ -d "$app" ]] && ! mv "$app" "$app.old" 2>/dev/null; then
    echo "Clip is still running. Quit it from its tray icon and run this again." >&2
    exit 1
  fi
}

if [[ "${1:-}" == "--uninstall" ]]; then
  move_aside
  rm -rf "$app.old" "$shortcut"
  reg delete 'HKCU\Software\Microsoft\Windows\CurrentVersion\Run' //v Clip //f >/dev/null 2>&1 || true
  echo "Clip removed. Your clips (Videos\\Clip) and settings (%APPDATA%\\Clip) are still there."
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "Downloading Clip..."
curl -fL --retry 3 --progress-bar -o "$tmp/Clip.zip" "$url"
CLIP_ZIP="$(cygpath -w "$tmp/Clip.zip")" CLIP_DEST="$(cygpath -w "$tmp/Clip")" \
  ps 'Expand-Archive -Force -LiteralPath $env:CLIP_ZIP -DestinationPath $env:CLIP_DEST'
[[ -f "$tmp/Clip/Clip.exe" ]] || { echo "The download didn't contain Clip.exe." >&2; exit 1; }

move_aside
mkdir -p "$(dirname "$app")"
mv "$tmp/Clip" "$app" || { [[ -d "$app.old" ]] && mv "$app.old" "$app"; exit 1; }
rm -rf "$app.old"

# Clip adds itself to the Start menu and to startup when it runs.
ps 'Start-Process -FilePath $env:CLIP_EXE'

echo "Clip is installed in $(cygpath -w "$app") and running in the tray."
echo "It's in the Start menu and starts with Windows. Press F8 in a game to save a clip."
