#!/usr/bin/env bash
# Start the distributed solve on this (Linux) machine: coordinator + a local worker.
#   THREADS=12 ./dist/start.sh      (default: all hardware threads)
set -e
cd "$(dirname "$0")/.."
[ -x build/snek ] || bash dist/build.sh
mkdir -p results/dist
if ! pgrep -f "dist/coordinator.py" > /dev/null; then
  nohup python3 dist/coordinator.py >> results/dist/coordinator.out 2>&1 &
  sleep 3
fi
THREADS=${THREADS:-$(nproc)}
if ! pgrep -f "build/snek worker" > /dev/null; then
  nohup nice -n 10 ./build/snek worker 127.0.0.1:8090 "$THREADS" >> results/dist/worker.out 2>&1 &
fi
IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo "running. status page: http://${IP:-localhost}:8091/"
echo "other machines join with:  snek worker ${IP:-<this-ip>}:8090 <threads>"
