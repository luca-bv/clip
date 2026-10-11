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
ps() { powershell.exe -NoProfile -NonInteractive -Command "$1"; }

# Clip holds its own files open, so stop it first. Fails harmlessly if it isn't running.
taskkill //IM Clip.exe //F >/dev/null 2>&1 && sleep 1 || true

if [[ "${1:-}" == "--uninstall" ]]; then
  rm -rf "$app" "$shortcut"
  reg delete 'HKCU\Software\Microsoft\Windows\CurrentVersion\Run' //v Clip //f >/dev/null 2>&1 || true
  echo "Clip removed. Your clips (Videos\\Clip) and settings (%APPDATA%\\Clip) are still there."
  exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "Downloading Clip..."
curl -fL --retry 3 --progress-bar -o "$tmp/Clip.zip" "$url"
ps "Expand-Archive -Force -LiteralPath '$(cygpath -w "$tmp/Clip.zip")' -DestinationPath '$(cygpath -w "$tmp/Clip")'"
[[ -f "$tmp/Clip/Clip.exe" ]] || { echo "The download didn't contain Clip.exe." >&2; exit 1; }

rm -rf "$app"
mkdir -p "$(dirname "$app")"
mv "$tmp/Clip" "$app"

ps "\$s = (New-Object -ComObject WScript.Shell).CreateShortcut('$(cygpath -w "$shortcut")');
    \$s.TargetPath = '$(cygpath -w "$app/Clip.exe")'; \$s.Save()"
ps "Start-Process -FilePath '$(cygpath -w "$app/Clip.exe")'"

echo "Clip is installed in $(cygpath -w "$app") and running in the tray."
echo "It's in the Start menu and starts with Windows. Press Alt+F9 in a game to save a clip."
