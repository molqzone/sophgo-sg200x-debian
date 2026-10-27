#!/usr/bin/env bash
# Host launcher for the MaixCAM camera viewer on Linux and other POSIX systems.
#
# It keeps a virtual environment next to this script, installs the pinned
# dependencies only when requirements.txt changes, and then hands every argument
# to viewer.py.  The viewer records by default; pass --no-record to watch without
# writing, or --url to open another stream.
#
#   tools/maixcam-camera/run-viewer.sh
#   tools/maixcam-camera/run-viewer.sh --no-record
#
# The window needs a display; over SSH forward one with `ssh -X` or run it on the
# machine that has the screen.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
venv="$here/.venv"
python=${PYTHON:-python3}
requirements="$here/requirements.txt"
stamp="$venv/requirements.sha256"

if [ ! -x "$venv/bin/python" ]; then
	echo "creating $venv"
	"$python" -m venv "$venv"
fi

current=$(sha256sum "$requirements" | cut -d' ' -f1)
if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$current" ]; then
	echo "installing viewer dependencies"
	"$venv/bin/python" -m pip install --disable-pip-version-check \
		--requirement "$requirements"
	printf '%s\n' "$current" > "$stamp"
fi

exec "$venv/bin/python" "$here/viewer.py" "$@"
