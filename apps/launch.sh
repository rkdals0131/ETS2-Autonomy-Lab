#!/usr/bin/env bash
set -e
APP_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$APP_ROOT/../bridge/ros-env.sh"
exec /usr/bin/python3 "$APP_ROOT/launcher/linux_launcher.py" "$@"
