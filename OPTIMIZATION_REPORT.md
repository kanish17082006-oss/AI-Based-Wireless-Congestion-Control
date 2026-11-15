# Optimization Report: Persistent AI Prediction Worker for ns-3 Congestion Control

**Project Title:** AI-Based Congestion Control for Wireless Networks  
**Environment:** ns-3.46.1 | C++17 | Python 3.14 (WSL Ubuntu 2) | Scikit-Learn | Random Forest Classifier | Flask | React (Vite)  
**Date:** November 2026  
**Status:** Validated, Verified & Benchmark Completed  

---

## 1. Executive Summary

In network simulation research combining ns-3 and machine learning, a recurring systems bottleneck is the IPC interface between C++ discrete-event simulations and Python machine learning runtimes. In the baseline implementation of this project, the C++ adaptive congestion controller invoked Python predictions via `popen()` once per sender per control second. For a 14-second simulation with 3 senders across 9 control intervals (27 predictions), this resulted in 27 distinct Python interpreter processes being spawned, repeatedly incurring library imports (`scikit-learn`, `pandas`, `joblib`, `numpy`) and Random Forest model deserialization (`congestion_model.pkl`).

This optimization replaces the transient `popen()` architecture with a **High-Performance Persistent Python Prediction Worker** communicating over bidirectional POSIX standard input/output (`stdin`/`stdout`) pipes.

### Key Measurable Outcomes
- **100% Prediction Equivalence:** Confirmed across 22 representative test cases (including low, moderate, high, congestion boundary, and historical dataset values), matching both prediction labels and subsequent rate adjustments exactly.
- **Bit-for-Bit Output Fidelity:** The generated `ai-control-results.csv` matched the baseline dataset exactly with an identical SHA-256 hash: `CA054DA3B4DD1F9ED8C6AD6BA52E9A987DAE020E02CCC8E9EFF0D8CD1F47EAE8`.
- **Interpreter Launches Reduced:** From 27 launches down to **exactly 1 launch** per simulation.
- **Model Deserialization Reduced:** From 27 loads down to **exactly 1 load** resident in RAM.
- **Simulation Wall-Clock Time:** Reduced from **1,098.32 seconds (~18.3 minutes)** down to **19.15–21.79 seconds** (**50.4x to 57.3x measured speedup**).

---

## 2. Original Architecture & Identified Bottlenecks

### 2.1 The Baseline Design
In `scratch/ai-congestion-control.cc`, each sender's congestion status was determined every second by calling:
```cpp
int PredictCongestion(double rate, double throughput)
{
    std::ostringstream command;
    command << ".venv/bin/python predict.py "
            << std::fixed << std::setprecision(4)
            << rate << " " << throughput;

    FILE* pipe = popen(command.str().c_str(), "r");
    // read single output line, pclose()
    ...
}
```

### 2.2 Systems Root Cause Analysis
Every individual prediction incurred a five-stage penalty:
1. **Shell and OS Fork/Exec:** `popen()` invokes `/bin/sh -c`, spawning an intermediate shell before spawning the Python binary.
2. **Interpreter Initialization:** Python runtime initialization, bytecode loading, and module search path resolution.
3. **Heavy Dependency Imports:** Loading dynamic shared objects (`.so`) for NumPy, SciPy, Scikit-learn, and Pandas. On WSL2 / cross-filesystem mounts (`/mnt/d/...`), metadata and I/O latency exacerbate import times to ~30–45 seconds per invocation.
4. **Model Deserialization:** Reading and unpickling `congestion_model.pkl` (200 trees, max depth 8) from disk into RAM.
5. **Process Tear-down:** Python process termination and file descriptor reclamation.

Multiplied across 3 senders $\times$ 9 intervals = 27 calls, the simulation spent over 98% of its wall-clock execution time starting up and tearing down the Python runtime rather than progressing the discrete-event simulation.

---

## 3. Optimized Architecture: Persistent IPC Worker

### 3.1 Architectural Overview
The optimized architecture decouples the lifecycle of the Python process from individual prediction requests:
1. **Single Worker Lifecycle:** At simulation setup (before `Simulator::Run()`), C++ launches a single Python worker (`predict_worker.py`).
2. **One-Time Initialization:** Python imports libraries and deserializes `congestion_model.pkl` once, storing the Random Forest model resident in memory.
3. **Handshake Protocol:** The worker signals readiness by emitting `READY\n` on stdout; C++ blocks on `ReadLine()` until this handshake is verified.
4. **Persistent Request/Response IPC:** In every control second, C++ writes feature pairs (`<Rate_Mbps> <Throughput_Mbps>\n`) directly into the child process's stdin pipe and reads back the integer prediction (`0\n` or `1\n`) from the child's stdout pipe in sub-millisecond time.
5. **Clean Termination:** When the simulation ends, C++ issues a `QUIT\n` command, closes pipes, and reaps the child process with `waitpid()`, guaranteeing zero zombie processes.

```
+------------------------------------+               +-----------------------------------+
|      ns-3 Simulation (C++)         |               |     predict_worker.py (Python)    |
|                                    |               |                                   |
|  main()                            |  fork/exec    |  import joblib, pandas            |
|   |--> PersistentPythonWorkerClient|-------------->|  model = joblib.load(model.pkl)   |
|   |                                |               |  print("READY")                   |
|   |    <--- "READY" Handshake -----|<--------------|                                   |
|   |                                |               |                                   |
|  ControlLoop()                     |               |                                   |
|   |--> fprintf(pipe, "%.4f %.4f\n")|-------------->|  features = pd.DataFrame(...)     |
|   |    <--- "0" or "1" \n ---------|<--------------|  pred = model.predict(features)  |
|   |    (In-memory IPC)             |               |                                   |
|   v                                |               |                                   |
|  Simulator::Destroy()              |               |                                   |
|   |--> fprintf(pipe, "QUIT\n") ----|-------------->|  Clean exit(0)                    |
|   |--> waitpid(pid)                |               +-----------------------------------+
+------------------------------------+
```

---

## 4. Systems Programming & Targeted Reliability Engineering

### 4.1 C++ `PersistentPythonWorkerClient` Class
Implemented in [scratch/ai-congestion-control.cc](file:///d:/CN%20CBP/ns-3/ns-allinone-3.46.1/ns-3.46.1/scratch/ai-congestion-control.cc):
- **Linux Descriptor Safety (`SafeCloseFd`):** Invalidates the stored descriptor (`fd = -1`) immediately before calling `close(tempFd)`. On Linux/WSL, `close()` releases the descriptor even when interrupted; retrying after `EINTR` is omitted to eliminate the danger of closing a descriptor reused by another thread or component.
- **Robust `ReadLine()` Stream Loop:** Resets `errno = 0` before each `fgets()` invocation. Retries only when `ferror()` is set and `errno == EINTR`, clearing the stream error with `clearerr()` prior to retry. EOF and non-EINTR stream read errors are handled and reported with distinct diagnostics.
- **Original `SIGPIPE` Disposition Preservation:** In `Start()`, the parent process queries and saves the existing `SIGPIPE` disposition via `sigaction(SIGPIPE, &sa, &m_oldSigpipeAction)` before setting `SIG_IGN`. The original disposition is restored in `Stop()`. The child process explicitly restores `SIG_DFL` before calling `execvp()`.
- **Explicit `waitpid` Error Handling:** Helper routines `PollProcessStatus()` and `WaitForProcessExit()` explicitly check for `EINTR` (retry) and `ECHILD` (treat as reaped). If an unexpected error occurs, `m_pid` is not silently discarded, and a diagnostic warning is logged.
- **Idempotent Resource Management:** The `Stop()` method safely nullifies pointers and marks descriptors `-1`. Calling `Stop()` multiple times or on partially initialized pipelines is completely safe and leak-free.

### 4.2 Python `predict_worker.py`
Implemented in [predict_worker.py](file:///d:/CN%20CBP/ns-3/ns-allinone-3.46.1/ns-3.46.1/predict_worker.py):
- **Strict Non-Finite Feature Validation:** Validates incoming features using `math.isfinite(rate) and math.isfinite(throughput)`. Rejects `NaN`, `+Inf`, and `-Inf` with diagnostic warnings on `sys.stderr` and returns error code `-1` on `sys.stdout`.
- **Model Load Verification:** In one-shot mode (`__main__`), explicitly verifies that `load_model()` succeeded before invoking `predict()`.
- **Protocol Channel Isolation:** Standard output is reserved strictly for protocol tokens (`READY`, `PONG`, `0`, `1`, `-1`), while all status and debugging messages route to `sys.stderr`.

### 4.3 Comprehensive Verification Test Harness
Implemented in [verify_prediction_equivalence.py](file:///d:/CN%20CBP/ns-3/ns-allinone-3.46.1/ns-3.46.1/verify_prediction_equivalence.py):
- **PING/PONG Validation:** Verifies bidirectional handshake and ping response prior to feature tests.
- **Malformed & Non-Numeric Input Tests:** Verifies that non-numeric strings, missing fields, excess fields, and non-finite numbers return `-1`.
- **Post-Error Usability Verification:** Confirms that the worker process remains fully operational and computes accurate predictions after handling error inputs.
- **Resource Cleanup in `try/finally`:** Closes `stdin`, `stdout`, and `stderr`, polls/reaps the child process, and verifies that the process exits with status code 0.

---

## 5. Prediction Equivalence & Robustness Verification

Evaluated across protocol tests, 22 standard operational test cases, 8 malformed/non-finite cases, and post-error recovery:

| Test Stage / Input | Category | Expected Output | Actual Output | Status |
|:---|:---|:---:|:---:|:---:|
| `READY` Handshake | Worker Initialization | READY | READY | **PASS** |
| `PING` Command | Protocol Verification | PONG | PONG | **PASS** |
| 1.0, 0.5000 | Low rate & throughput | 0 (Normal) | 0 (Normal) | **PASS** |
| 2.0, 1.8000 | Low rate & throughput | 0 (Normal) | 0 (Normal) | **PASS** |
| 5.0, 4.9000 | Low rate & throughput | 0 (Normal) | 0 (Normal) | **PASS** |
| 12.0, 11.5000 | Moderate rate & throughput | 0 (Normal) | 0 (Normal) | **PASS** |
| 15.0, 14.8000 | Moderate rate & throughput | 0 (Normal) | 0 (Normal) | **PASS** |
| 16.0, 15.2000 | Moderate rate & throughput | 0 (Normal) | 0 (Normal) | **PASS** |
| 25.0, 24.5000 | High rate & throughput | 1 (Congested) | 1 (Congested) | **PASS** |
| 30.0, 28.0000 | High rate & throughput | 1 (Congested) | 1 (Congested) | **PASS** |
| 18.0, 5.0000 | Severe Congestion (low throughput) | 1 (Congested) | 1 (Congested) | **PASS** |
| 20.0, 8.0000 | Severe Congestion | 1 (Congested) | 1 (Congested) | **PASS** |
| 28.0, 12.0000 | Severe Congestion | 1 (Congested) | 1 (Congested) | **PASS** |
| 15.0, 7.0000 | Moderate Congestion | 0 (Normal) | 0 (Normal) | **PASS** |
| 16.0, 14.8931 | Historical Dataset T=2 S=1 | 0 (Normal) | 0 (Normal) | **PASS** |
| 16.0, 11.8538 | Historical Dataset T=2 S=2 | 0 (Normal) | 0 (Normal) | **PASS** |
| 16.0, 8.8146 | Historical Dataset T=2 S=3 | 0 (Normal) | 0 (Normal) | **PASS** |
| 17.0, 15.9498 | Historical Dataset T=3 S=1 | 0 (Normal) | 0 (Normal) | **PASS** |
| 18.0, 16.9902 | Historical Dataset T=4 S=1 | 1 (Congested) | 1 (Congested) | **PASS** |
| 18.0, 16.6789 | Historical Dataset T=5 S=1 | 1 (Congested) | 1 (Congested) | **PASS** |
| 18.0, 17.8913 | Historical Dataset T=5 S=2 | 1 (Congested) | 1 (Congested) | **PASS** |
| 18.0, 17.6620 | Historical Dataset T=5 S=3 | 1 (Congested) | 1 (Congested) | **PASS** |
| 12.6, 13.8691 | Historical Dataset T=6 S=1 | 0 (Normal) | 0 (Normal) | **PASS** |
| 17.6, 16.7936 | Historical Dataset T=10 S=1 | 1 (Congested) | 1 (Congested) | **PASS** |
| `abc def` | Malformed: Non-numeric strings | -1 | -1 | **PASS** |
| `15.0 xyz` | Malformed: Non-numeric throughput | -1 | -1 | **PASS** |
| `15.0` | Malformed: Single field | -1 | -1 | **PASS** |
| `15.0 14.0 13.0` | Malformed: Excess fields | -1 | -1 | **PASS** |
| `nan 10.0` | Non-finite: NaN rate | -1 | -1 | **PASS** |
| `15.0 inf` | Non-finite: +Inf throughput | -1 | -1 | **PASS** |
| `-inf 10.0` | Non-finite: -Inf rate | -1 | -1 | **PASS** |
| `nan nan` | Non-finite: Double NaN | -1 | -1 | **PASS** |
| `15.0 14.8` (Post-Error) | Usability Recovery: Normal | 0 | 0 | **PASS** |
| `25.0 24.5` (Post-Error) | Usability Recovery: Congested | 1 | 1 | **PASS** |
| `QUIT` Command | Clean Termination | Exit Code 0 | Exit Code 0 | **PASS** |

**Conclusion:** 100% test success across all stages.

---

## 6. Simulation & Experimental Preservation

### 6.1 Scientific Parameter Preservation
All experimental parameters were held strictly invariant:
- **Topology:** 3 Senders (nodes 0, 1, 2) $\to$ 1 Receiver (node 3) on Ad-Hoc 802.11n Wi-Fi.
- **Traffic Pattern:** UDP OnOffApplication, packet size 1024 bytes, start staggered at $1.0 + 0.2i$ s, stop at $11.0 + 0.2i$ s.
- **RNG Seeds:** `RngSeedManager::SetSeed(12345); RngSeedManager::SetRun(1);`.
- **Adaptation Rules:**
  - Congestion: Multiplicative decrease by 30% (`rate = max(1.0, rate * 0.70)`).
  - Normal: Additive increase by 1.0 Mbps (`rate = min(30.0, rate + 1.0)`).
  - Bound limits: [1.0 Mbps, 30.0 Mbps].

### 6.2 Dataset Integrity & Bit-for-Bit Validation
Comparison between baseline and newly generated `ai-control-results.csv`:
- Total rows: 27 data rows + 1 header row.
- File Size: 690 bytes.
- Baseline SHA-256: `CA054DA3B4DD1F9ED8C6AD6BA52E9A987DAE020E02CCC8E9EFF0D8CD1F47EAE8`
- Optimized SHA-256: `CA054DA3B4DD1F9ED8C6AD6BA52E9A987DAE020E02CCC8E9EFF0D8CD1F47EAE8`
- **Result:** Exact identical match.

---

## 7. Performance Benchmarking & Reporting

### 7.1 Measured Metrics Comparison

| Measurement Metric | Original Baseline | Optimized Persistent Worker | Type of Measurement | Improvement Factor |
|:---|---:|---:|:---|---:|
| **Total Wall-Clock Runtime** | **1,098.32 s (18.3 min)** | **19.15 s – 21.79 s** | Instrumented Measurement | **50.4x – 57.3x faster** |
| **Simulated Duration** | 14.00 s | 14.00 s | Simulation Invariant | Identical |
| **Python Process Launches** | 27 launches | **1 launch** | Instrumented Counter | **27x reduction** |
| **Model Loads into Memory** | 27 loads | **1 load** | Architecture-Derived | **27x reduction** |
| **Predictions Requested** | 27 | 27 | Instrumented Counter | 100% |
| **Predictions Succeeded** | 27 | 27 | Instrumented Counter | 100% |
| **Worker One-Time Startup Duration** | N/A (distributed overhead) | 13,496.01 ms – 15,962.94 ms | Instrumented Measurement | Upfront one-time cost |
| **Process Termination State** | Repeated popen teardown | Clean SIGTERM/waitpid reaping | System Exit Code | Clean, 0 zombies |

---

## 8. Backend & React Dashboard Compatibility

The Flask backend [app.py](file:///d:/CN%20CBP/ns-3/ns-allinone-3.46.1/ns-3.46.1/app.py) and React dashboard [dashboard/](file:///d:/CN%20CBP/ns-3/ns-allinone-3.46.1/ns-3.46.1/dashboard) interface directly with the generated CSV files:
- `/api/comparison`: Consumes `comparison-results.csv`.
- `/api/ai-results`: Consumes `ai-control-results.csv`.
- `/api/network`: Consumes latest timestamped row of `ai-control-results.csv`.
- `/api/dataset`: Consumes `window-results.csv`.

Because the schema, column names (`Time`, `Sender_ID`, `Rate_Mbps`, `Throughput_Mbps`, `AI_Prediction`), and exact numerical results are preserved bit-for-bit, all frontend views (**Overview**, **Network**, **AI Analysis**, and **Comparison**) continue to render seamlessly without requiring frontend changes.

---

## 9. Comparison Simulation Analysis (`scratch/comparison.cc`)

### 9.1 Inspection & Identified Parallel Bottleneck
In `scratch/comparison.cc`, three controllers are benchmarked across 5 random simulation runs:
1. `Fixed` (constant 15 Mbps rate).
2. `Threshold` (heuristic throughput thresholding).
3. `AI` (Random Forest model predictions).

Currently, `scratch/comparison.cc` contains the identical `popen()` bottleneck in its `Predict()` function:
```cpp
int Predict(double rate, double throughput)
{
    command << ".venv/bin/python predict.py " ...
    FILE* pipe = popen(command.str().c_str(), "r");
    ...
}
```
For a 5-run experiment, the AI controller triggers $5 \text{ runs} \times 27 \text{ predictions} = 135 \text{ launches}$. This causes the comparison suite to take **almost an hour** to execute.

### 9.2 Recommendation for Comparison Simulation
Following the successful validation of `scratch/ai-congestion-control.cc`, `scratch/comparison.cc` can adopt the identical `PersistentPythonWorkerClient`:
- Instantiate `g_workerClient.Start()` once during the comparison suite.
- Re-use the worker for all 5 AI runs.
- Expected wall-clock reduction for the 5-run comparison: from **~60 minutes down to ~2 minutes**.

---

## 10. Research & Experimental Quality Review

To elevate this project to high-impact academic or publication standards, the following research extensions are recommended:

1. **Multi-Seed Statistical Robustness:**
   - *Problem:* Currently evaluated on 5 seeds.
   - *Improvement:* Expand to 30+ seeds and report 95% confidence intervals and paired t-tests or Wilcoxon signed-rank tests for throughput, delay, and Jain's fairness index.
2. **Dense & Dynamic Contention Topologies:**
   - *Problem:* Evaluated only on 3 static senders.
   - *Improvement:* Evaluate 5, 10, and 20 competing nodes to assess CSMA/CA backoff collapse under high channel contention.
3. **Multi-Modal Feature Expansion:**
   - *Problem:* Features are currently limited to transmission rate and throughput.
   - *Improvement:* Incorporate MAC-layer queue occupancy (`QueueDisc`), physical layer SNR/RSSI, and RTT gradient to distinguish between wireless channel loss and buffer congestion.
4. **Benchmarking Against Standard TCP Congestion Control:**
   - *Problem:* Evaluated only against Fixed and Simple Threshold heuristics over UDP.
   - *Improvement:* Benchmark against TCP BBRv1/v2, TCP Cubic, and Westwood+ over identical wireless fading channels.

---

## 11. Remaining Limitations

1. **Comparison Simulation (`comparison.cc`):**
   - *Limitation:* Kept untouched per prompt instructions. It still incurs repeated `popen()` launches during multi-run comparisons until migrated to the persistent client.
2. **Model Telemetry Features:**
   - *Limitation:* Telemetry relies solely on two MAC/Application features (Rate and Throughput). Dynamic fading channels with high variance may confuse channel-induced loss with buffer congestion unless PHY-layer metrics (SNR, RSSI) are added in future training iterations.

---

## 12. Build & Execution Reference

### Windows + WSL Ubuntu 2 Commands

#### 1. Build the Optimized Simulation
```bash
wsl ./ns3 build scratch/ai-congestion-control
```

#### 2. Run the Optimized Simulation
```bash
wsl ./ns3 run scratch/ai-congestion-control
```

#### 3. Run Comprehensive Verification Suite
```bash
wsl .venv/bin/python verify_prediction_equivalence.py
```

#### 4. Run Flask Backend API
```powershell
python app.py
```
*(or via WSL: `wsl .venv/bin/python app.py`)*

#### 5. Launch React Dashboard
```powershell
cd dashboard
npm run dev
```
Open `http://localhost:5173` in a web browser.
