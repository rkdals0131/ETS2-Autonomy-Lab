#!/usr/bin/env bash
set -e
APP_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec /usr/bin/python3 "$APP_ROOT/launcher/linux_launcher.py" "$@"
