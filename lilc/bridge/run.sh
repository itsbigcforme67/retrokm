#!/bin/sh
# Start the lil' C bridge (first run: bash setup.sh)
cd "$(dirname "$0")" && exec .venv/bin/python lilc_bridge.py
