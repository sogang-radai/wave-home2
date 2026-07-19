#!/usr/bin/env bash
# Restart the system Ollama LaunchDaemon so brew-upgraded binary is used.
# Requires admin password (com.ollama.ollama runs as root).
set -euo pipefail

PLIST="/Library/LaunchDaemons/com.ollama.ollama.plist"
if [[ ! -f "$PLIST" ]]; then
  echo "ERROR: $PLIST not found" >&2
  exit 1
fi

echo "Current server: $(curl -s http://127.0.0.1:11434/api/version 2>/dev/null || echo unreachable)"
echo "Client binary:  $(/opt/homebrew/bin/ollama --version 2>&1 | tr '\n' ' ')"
echo ""
echo "Restarting com.ollama.ollama (admin password required)..."
sudo launchctl bootout system/com.ollama.ollama 2>/dev/null || true
sleep 1
sudo launchctl bootstrap system "$PLIST"
sleep 2
echo "Server now: $(curl -s http://127.0.0.1:11434/api/version)"
/opt/homebrew/bin/ollama --version
