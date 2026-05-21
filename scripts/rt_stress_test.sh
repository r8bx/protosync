#!/bin/bash
# rt_stress_test.sh — run both stress and cyclictest together

echo "Starting stress load..."
stress-ng --cpu $(nproc) --vm 1 --vm-bytes 128M --io 2 \
  --timeout 300s &
STRESS_PID=$!

echo "Starting cyclictest for 5 minutes..."
sudo cyclictest \
  --mlockall \
  --smp \
  --priority=80 \
  --interval=200 \
  --distance=0 \
  --histfile=/tmp/rt_histogram.txt \
  -D 300

kill $STRESS_PID

echo ""
echo "=== RESULTS SUMMARY ==="
grep -E "Min|Avg|Max" /tmp/rt_histogram.txt || \
  echo "Check output above for Max latency per thread"
