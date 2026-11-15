#!/usr/bin/env python3
"""
predict_worker.py
Persistent Python prediction worker for ns-3 AI-based congestion control.

Loads congestion_model.pkl once at startup and services continuous prediction
requests over standard input/output (line-based IPC), eliminating repeated
interpreter startup and model deserialization overhead.

Reliability Features:
- Rejects non-finite (NaN, +Inf, -Inf) values via math.isfinite()
- Validates model loading in both persistent and one-shot modes
- Standard output dedicated exclusively to protocol tokens
- Diagnostic/error logging strictly directed to standard error
"""

import sys
import os
import signal
import math
import joblib
import pandas as pd

MODEL_PATH = "congestion_model.pkl"
FEATURE_NAMES = ["Rate_Mbps", "Throughput_Mbps"]


def handle_signals(signum, frame):
    """Handle graceful shutdown signals."""
    sys.stderr.write(f"[predict_worker] Received signal {signum}, exiting cleanly.\n")
    sys.stderr.flush()
    sys.exit(0)


def load_model(path):
    """Load the trained machine learning model."""
    if not os.path.exists(path):
        sys.stderr.write(f"[predict_worker] ERROR: Model file not found at '{path}'\n")
        sys.stderr.flush()
        return None
    try:
        model = joblib.load(path)
        sys.stderr.write(f"[predict_worker] Model successfully loaded from '{path}'\n")
        sys.stderr.flush()
        return model
    except Exception as exc:
        sys.stderr.write(f"[predict_worker] ERROR loading model '{path}': {exc}\n")
        sys.stderr.flush()
        return None


def run_worker():
    """Persistent IPC request-response loop."""
    # Register termination signals
    signal.signal(signal.SIGINT, handle_signals)
    signal.signal(signal.SIGTERM, handle_signals)

    sys.stderr.write("[predict_worker] Starting persistent prediction worker...\n")
    sys.stderr.flush()

    model = load_model(MODEL_PATH)
    if model is None:
        print("ERROR_LOAD_FAILED")
        sys.stdout.flush()
        sys.exit(1)

    # Warm-up run to verify model inference path
    try:
        warmup_df = pd.DataFrame([{"Rate_Mbps": 15.0, "Throughput_Mbps": 14.0}], columns=FEATURE_NAMES)
        _ = model.predict(warmup_df)
    except Exception as exc:
        sys.stderr.write(f"[predict_worker] FATAL: Warm-up prediction failed: {exc}\n")
        sys.stderr.flush()
        print("ERROR_WARMUP_FAILED")
        sys.stdout.flush()
        sys.exit(1)

    # Handshake: notify parent process that worker is initialized and ready
    print("READY")
    sys.stdout.flush()
    sys.stderr.write("[predict_worker] Worker ready and listening on stdin.\n")
    sys.stderr.flush()

    # Request-response loop
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        # Check for control commands
        cmd = line.upper()
        if cmd in ("QUIT", "EXIT", "STOP"):
            sys.stderr.write("[predict_worker] Clean shutdown request received.\n")
            sys.stderr.flush()
            break
        elif cmd == "PING":
            print("PONG")
            sys.stdout.flush()
            continue

        # Parse prediction features: <Rate_Mbps> <Throughput_Mbps>
        parts = line.split()
        if len(parts) != 2:
            sys.stderr.write(f"[predict_worker] Invalid input format: '{line}'. Expected '<Rate_Mbps> <Throughput_Mbps>'\n")
            sys.stderr.flush()
            print("-1")
            sys.stdout.flush()
            continue

        try:
            rate = float(parts[0])
            throughput = float(parts[1])
        except ValueError as exc:
            sys.stderr.write(f"[predict_worker] Number parsing error for line '{line}': {exc}\n")
            sys.stderr.flush()
            print("-1")
            sys.stdout.flush()
            continue

        # Reject non-finite (NaN, Inf, -Inf) feature values using math.isfinite()
        if not (math.isfinite(rate) and math.isfinite(throughput)):
            sys.stderr.write(f"[predict_worker] Invalid non-finite feature values: rate={rate}, throughput={throughput}. Rejected.\n")
            sys.stderr.flush()
            print("-1")
            sys.stdout.flush()
            continue

        try:
            # Construct DataFrame with exact column names and ordering
            features = pd.DataFrame([{
                "Rate_Mbps": rate,
                "Throughput_Mbps": throughput
            }], columns=FEATURE_NAMES)

            prediction = int(model.predict(features)[0])
            print(prediction)
            sys.stdout.flush()
        except Exception as exc:
            sys.stderr.write(f"[predict_worker] Prediction error for inputs ({rate}, {throughput}): {exc}\n")
            sys.stderr.flush()
            print("-1")
            sys.stdout.flush()

    sys.stderr.write("[predict_worker] Worker terminating normally.\n")
    sys.stderr.flush()
    sys.exit(0)


if __name__ == "__main__":
    # If invoked with command-line arguments, support one-shot mode for compatibility
    if len(sys.argv) == 3:
        try:
            m = load_model(MODEL_PATH)
            if m is None:
                sys.stderr.write("[predict_worker] ERROR: Model load failed in one-shot mode.\n")
                sys.stderr.flush()
                print("-1")
                sys.exit(1)

            r = float(sys.argv[1])
            t = float(sys.argv[2])

            if not (math.isfinite(r) and math.isfinite(t)):
                sys.stderr.write(f"[predict_worker] Invalid non-finite feature values: rate={r}, throughput={t}. Rejected.\n")
                sys.stderr.flush()
                print("-1")
                sys.exit(1)

            f = pd.DataFrame([{"Rate_Mbps": r, "Throughput_Mbps": t}], columns=FEATURE_NAMES)
            p = int(m.predict(f)[0])
            print(p)
            sys.exit(0)
        except Exception as e:
            sys.stderr.write(f"[predict_worker] One-shot execution error: {e}\n")
            sys.stderr.flush()
            print("-1")
            sys.exit(1)
    else:
        run_worker()
