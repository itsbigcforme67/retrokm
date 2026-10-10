#!/bin/sh
# One-time setup: Python venv with speech-to-text and text-to-speech
set -e
cd "$(dirname "$0")"
python3 -m venv .venv
.venv/bin/pip install faster-whisper piper-tts
mkdir -p voices
for f in en_US-lessac-low.onnx en_US-lessac-low.onnx.json; do
  [ -f voices/$f ] || curl -L -o voices/$f \
    "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/low/$f"
done
echo "Done. Start the bridge with: sh run.sh"
