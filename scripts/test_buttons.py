#!/usr/bin/env python3
"""
gpio_monitor.py
Monitor GPIO23, GPIO24, GPIO16, GPIO5 — active-low inputs.

Prints a timestamped line to stdout on every rising and falling edge.
Active-low convention: pin LOW = asserted (ACTIVE), pin HIGH = idle.

Requires: pigpio daemon running
    sudo pigpiod
    pip install pigpio
"""

import sys
import signal
import time
import pigpio

# ── Configuration ──────────────────────────────────────────────────────────
PINS = [23, 24, 16, 5]   # BCM numbering

PIN_LABELS = {
    23: "GPIO23",
    24: "GPIO24",
    16: "GPIO16",
     5: "POT SW",
}

T0 = time.monotonic()   # reference time for relative timestamps


# ── Helpers ────────────────────────────────────────────────────────────────
def ts() -> str:
    """Relative timestamp in seconds since script start."""
    return f"{time.monotonic() - T0:10.4f}s"


def level_str(pi: pigpio.pi, pin: int) -> str:
    """Return ACTIVE or IDLE based on active-low logic."""
    return "ACTIVE" if pi.read(pin) == 0 else "IDLE  "


# ── Edge callback ──────────────────────────────────────────────────────────
def make_callback(label: str):
    """Return a pigpio edge callback for the given pin label."""
    def _cb(gpio, level, tick):
        # level: 0 = falling (ACTIVE), 1 = rising (IDLE), 2 = watchdog
        if level == pigpio.LOW:
            print(f"[{ts()}]  {label:<8}  ↓ FALL  → ACTIVE  (pin LOW)")
        elif level == pigpio.HIGH:
            print(f"[{ts()}]  {label:<8}  ↑ RISE  → IDLE    (pin HIGH)")
    return _cb


# ── Setup ──────────────────────────────────────────────────────────────────
def setup(pi: pigpio.pi) -> list:
    callbacks = []

    for pin in PINS:
        pi.set_mode(pin, pigpio.INPUT)
        pi.set_pull_up_down(pin, pigpio.PUD_UP)   # active-low: idle = HIGH

        label = PIN_LABELS.get(pin, f"GPIO{pin}")
        cb = pi.callback(pin, pigpio.EITHER_EDGE, make_callback(label))
        callbacks.append(cb)

    print(f"Monitoring pins: {PINS}  (active-low, BCM numbering)")
    print(f"{'Pin':<8} {'Label':<10} {'State'}")
    print("-" * 35)
    for pin in PINS:
        label = PIN_LABELS.get(pin, f"GPIO{pin}")
        print(f"GPIO{pin:<4}  {label:<10}  {level_str(pi, pin)}")
    print("-" * 35)
    print("Waiting for edges... (Ctrl-C to quit)\n")

    return callbacks


# ── Teardown ───────────────────────────────────────────────────────────────
def teardown(callbacks: list, pi: pigpio.pi, signum=None, frame=None) -> None:
    print("\nCleaning up...")
    for cb in callbacks:
        cb.cancel()
    pi.stop()
    sys.exit(0)


# ── Main ───────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    pi = pigpio.pi()
    if not pi.connected:
        print("ERROR: cannot connect to pigpiod — is it running? (sudo pigpiod)")
        sys.exit(1)

    callbacks = setup(pi)

    signal.signal(signal.SIGINT,  lambda s, f: teardown(callbacks, pi))
    signal.signal(signal.SIGTERM, lambda s, f: teardown(callbacks, pi))

    while True:
        time.sleep(1)