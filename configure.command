#!/bin/bash
# Opens the launcher's mod configuration (macOS and Linux); Play starts the game with your mods.
exec "$(cd "$(dirname "$0")" && pwd)/launch.command" --configure "$@"
