#!/usr/bin/env bash
# Stop coordinator and local worker (state is saved; start.sh resumes).
pkill -f "build/snek worker" || true
pkill -f "dist/coordinator.py" || true
echo "stopped"
