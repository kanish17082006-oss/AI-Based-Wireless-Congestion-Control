#!/usr/bin/env python3
"""
verify_prediction_equivalence.py
Phase 6: Comprehensive Prediction Equivalence & Robustness Test Harness

Validates:
1. PING / PONG protocol handshake.
2. 100% Prediction and rate-adjustment equivalence across 22 representative test cases
   (comparing persistent predict_worker.py with original one-shot predict.py).
3. Malformed input rejection (non-numeric strings, wrong number of fields).
4. Non-finite value rejection (NaN, +Inf, -Inf via math.isfinite()).
5. Continued worker usability following error conditions.
6. Guaranteed process reaping and stream cleanup in try/finally blocks.
7. Proper process exit status verification.
"""

import subprocess
import sys
import os
import joblib
import pandas as pd

MODEL_PATH = "congestion_model.pkl"
PYTHON_BIN = ".venv/bin/python"

# 22 Representative test cases across low, moderate, high, boundary, and historical values
TEST_CASES = [
    # Category 1: Low rate and throughput
    (1.0, 0.5, "Low rate & throughput"),
    (2.0, 1.8, "Low rate & throughput"),
    (5.0, 4.9, "Low rate & throughput"),
    
    # Category 2: Moderate rate and throughput
    (12.0, 11.5, "Moderate rate & throughput"),
    (15.0, 14.8, "Moderate rate & throughput"),
    (16.0, 15.2, "Moderate rate & throughput"),
    
    # Category 3: High rate and throughput
    (25.0, 24.5, "High rate & throughput"),
    (30.0, 28.0, "High rate & throughput"),
    
    # Category 4: Congestion boundary and clear congestion
    (18.0, 5.0, "Severe Congestion (low throughput)"),
    (20.0, 8.0, "Severe Congestion"),
    (28.0, 12.0, "Severe Congestion"),
    (15.0, 7.0, "Moderate Congestion"),
    
    # Category 5: Historical dataset points from ai-control-results.csv
    (16.0, 14.8931, "Historical T=2 S=1"),
    (16.0, 11.8538, "Historical T=2 S=2"),
    (16.0, 8.81459, "Historical T=2 S=3"),
    (17.0, 15.9498, "Historical T=3 S=1"),
    (18.0, 16.9902, "Historical T=4 S=1"),
    (18.0, 16.6789, "Historical T=5 S=1 (Congested)"),
    (18.0, 17.8913, "Historical T=5 S=2 (Congested)"),
    (18.0, 17.6620, "Historical T=5 S=3 (Congested)"),
    (12.6, 13.8691, "Historical T=6 S=1"),
    (17.6, 16.7936, "Historical T=10 S=1")
]


def run_original_predict(rate, throughput):
    """Executes original predict.py as an isolated subprocess."""
    cmd = [PYTHON_BIN, "predict.py", str(rate), str(throughput)]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return int(proc.stdout.strip())


def compute_rate_adaptation(rate, prediction):
    """Applies the invariant rate adjustment rules."""
    if prediction == 1:
        return max(1.0, rate * 0.70)
    elif prediction == 0:
        return min(30.0, rate + 1.0)
    else:
        # Error (-1): rate held constant
        return rate


def send_and_receive(proc, msg):
    """Writes a message line to the worker and safely reads a single response line."""
    if proc.poll() is not None:
        return None
    try:
        proc.stdin.write(msg.strip() + "\n")
        proc.stdin.flush()
        line = proc.stdout.readline()
        return line.strip() if line else None
    except (BrokenPipeError, OSError):
        return None


def main():
    print("=" * 80)
    print("PREDICTION WORKER COMPREHENSIVE VERIFICATION SUITE")
    print("=" * 80)

    worker_proc = None
    tests_passed = True
    mismatches = []

    try:
        # Launch persistent worker process
        worker_proc = subprocess.Popen(
            [PYTHON_BIN, "predict_worker.py"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1
        )

        # 1. Verify READY Handshake
        handshake = worker_proc.stdout.readline()
        if not handshake:
            err = worker_proc.stderr.read()
            print(f"FATAL: Worker closed stdout before handshake. Stderr: {err}")
            sys.exit(1)

        handshake = handshake.strip()
        if handshake != "READY":
            print(f"FATAL: Handshake failed. Expected 'READY', received '{handshake}'")
            sys.exit(1)
        print("[STAGE 1: PASS] Handshake 'READY' verified.")

        # 2. Verify PING / PONG Protocol
        pong = send_and_receive(worker_proc, "PING")
        if pong == "PONG":
            print("[STAGE 2: PASS] PING / PONG protocol response verified.")
        else:
            print(f"[STAGE 2: FAIL] PING failed. Received: '{pong}' (Expected 'PONG')")
            tests_passed = False

        # 3. Verify Prediction and Rate Adaptation Equivalence (22 cases)
        print("\n[STAGE 3: EQUIVALENCE EVALUATION]")
        print(f"{'Rate':>6} | {'Throughput':>10} | {'Category':<32} | {'Orig':>4} | {'Worker':>6} | {'Match?':<6} | {'New Rate':>8}")
        print("-" * 80)

        for rate, throughput, category in TEST_CASES:
            orig_pred = run_original_predict(rate, throughput)
            worker_resp = send_and_receive(worker_proc, f"{rate:.4f} {throughput:.4f}")

            if worker_resp is None:
                print(f"ERROR: No response from worker for input ({rate}, {throughput})")
                worker_pred = -1
            else:
                try:
                    worker_pred = int(worker_resp)
                except ValueError:
                    print(f"ERROR: Malformed worker response: '{worker_resp}'")
                    worker_pred = -1

            matched = (orig_pred == worker_pred)
            orig_new_rate = compute_rate_adaptation(rate, orig_pred)
            worker_new_rate = compute_rate_adaptation(rate, worker_pred)
            rate_matched = (abs(orig_new_rate - worker_new_rate) < 1e-4)

            if not (matched and rate_matched):
                tests_passed = False
                mismatches.append((rate, throughput, orig_pred, worker_pred, orig_new_rate, worker_new_rate))

            status_str = "PASS" if (matched and rate_matched) else "FAIL"
            print(f"{rate:6.1f} | {throughput:10.4f} | {category:<32} | {orig_pred:4d} | {worker_pred:6d} | {status_str:<6} | {worker_new_rate:8.2f}")

        # 4. Malformed and Invalid Inputs Handling
        print("\n[STAGE 4: MALFORMED & NON-FINITE INPUT REJECTION]")
        malformed_cases = [
            ("abc def", "Non-numeric strings"),
            ("15.0 xyz", "Non-numeric throughput"),
            ("15.0", "Single field (missing throughput)"),
            ("15.0 14.0 13.0", "Three fields (excess fields)"),
            ("nan 10.0", "NaN rate"),
            ("15.0 inf", "+Inf throughput"),
            ("-inf 10.0", "-Inf rate"),
            ("nan nan", "Double NaN")
        ]

        for input_str, desc in malformed_cases:
            resp = send_and_receive(worker_proc, input_str)
            val = int(resp) if (resp and resp.lstrip("-").isdigit()) else -99
            if val == -1:
                print(f"  [PASS] Correctly rejected {desc:<35} ('{input_str}') -> Response: {val}")
            else:
                print(f"  [FAIL] Failed to reject {desc:<35} ('{input_str}') -> Response: '{resp}' (Expected '-1')")
                tests_passed = False

        # 5. Verify Worker Usability After Error Conditions
        print("\n[STAGE 5: POST-ERROR WORKER USABILITY RECOVERY]")
        recovery_1 = send_and_receive(worker_proc, "15.0 14.8")
        recovery_2 = send_and_receive(worker_proc, "25.0 24.5")

        if recovery_1 == "0" and recovery_2 == "1":
            print(f"  [PASS] Worker successfully recovered: '15.0 14.8' -> {recovery_1} (Normal), '25.0 24.5' -> {recovery_2} (Congested).")
        else:
            print(f"  [FAIL] Worker state corrupted after errors: recovery_1={recovery_1}, recovery_2={recovery_2}")
            tests_passed = False

        # 6. Clean Process Termination
        print("\n[STAGE 6: CLEAN SHUTDOWN VERIFICATION]")
        worker_proc.stdin.write("QUIT\n")
        worker_proc.stdin.flush()
        worker_proc.wait(timeout=5)
        exit_code = worker_proc.returncode

        if exit_code == 0:
            print(f"  [PASS] Worker process exited cleanly with code {exit_code}.")
        else:
            print(f"  [FAIL] Worker process exited with non-zero code {exit_code}.")
            tests_passed = False

    except Exception as exc:
        print(f"EXCEPTION during test execution: {exc}")
        tests_passed = False
    finally:
        # Guarantee full stream closure and child process reaping
        if worker_proc is not None:
            try:
                if worker_proc.stdin and not worker_proc.stdin.closed:
                    worker_proc.stdin.close()
            except Exception:
                pass
            try:
                if worker_proc.stdout and not worker_proc.stdout.closed:
                    worker_proc.stdout.close()
            except Exception:
                pass
            try:
                if worker_proc.stderr and not worker_proc.stderr.closed:
                    worker_proc.stderr.close()
            except Exception:
                pass

            if worker_proc.poll() is None:
                try:
                    worker_proc.kill()
                    worker_proc.wait(timeout=2)
                except Exception:
                    pass

    print("=" * 80)
    if tests_passed and len(mismatches) == 0:
        print("ALL TESTS PASSED: Protocol, Equivalence, Robustness & Recovery Confirmed.")
        sys.exit(0)
    else:
        print(f"TEST SUITE FAILED with {len(mismatches)} equivalence mismatches or test failures.")
        sys.exit(1)


if __name__ == "__main__":
    main()
