# AI-Based Congestion Control for Wireless Networks

An intelligent wireless network congestion-control system using ns-3 simulation and a Random Forest machine learning model.

## Project Overview

This project simulates a wireless network with three senders and one receiver. A machine learning model predicts network congestion using transmission rate and throughput. Based on the prediction, the system dynamically adjusts the transmission rate.

## Architecture

1. ns-3 simulates wireless network traffic.
2. Network measurements are collected into CSV datasets.
3. A Random Forest classifier predicts NORMAL or CONGESTED.
4. The adaptive controller adjusts sender transmission rates.
5. Flask provides simulation results through APIs.
6. React displays network metrics and comparison charts.

## Technologies Used

- C++ and ns-3.46.1
- Python
- Scikit-learn and Random Forest
- Pandas and NumPy
- Flask and Flask-CORS
- React and Vite
- JavaScript

## Machine Learning

- Dataset records: 2,640
- Input features: Transmission rate and throughput
- Model: Random Forest Classifier
- Test accuracy: 99.05%
- Output: Normal (0) or Congested (1)

Accuracy is measured on a held-out test set generated from ns-3 simulations.

## Congestion Control

- Normal prediction: Increase transmission rate by 1 Mbps.
- Congested prediction: Reduce transmission rate by 30%.

## Experimental Comparison

Average results across five simulation runs:

| Metric | Fixed | Threshold | AI Adaptive |
|---|---:|---:|---:|
| Throughput (Mbps) | 46.22 | 48.72 | 48.40 |
| Average Delay (ms) | 7.77 | 51.02 | 20.64 |
| Packet Loss (%) | 0.0273 | 0.1150 | 0.0334 |
| Jain Fairness | 1.0000 | 0.9824 | 0.99999 |

AI Adaptive provides a balance between throughput, delay, packet loss and fairness. Fixed control has lower average delay and packet loss in these experiments.

## Project Structure

- `scratch/` - ns-3 C++ simulation programs
- `train_model.py` - Random Forest training
- `predict.py` - Congestion prediction
- `congestion_model.pkl` - Trained model
- `window-results.csv` - ML dataset
- `wireless-results.csv` - Baseline dataset
- `ai-control-results.csv` - AI simulation results
- `comparison-results.csv` - Five-run average results
- `comparison-standard-deviation.csv` - Standard deviations
- `app.py` - Flask backend
- `dashboard/` - React frontend
- `plot_results.py` - Comparison graph generation
- `*.png` - Performance graphs and ML analysis

## Running the Project

### 1. Run the ns-3 AI simulation

From the ns-3.46.1 directory:

```bash
source .venv/bin/activate
./ns3 run scratch/ai-congestion-control

```

### 2. Start Flask backend

From the project directory:

```bash
source .venv/bin/activate
python app.py
```

### 3. Start React dashboard

Open another terminal, navigate to the dashboard directory and run:

```bash
npm install
npm run dev
```

Open http://localhost:5173/ in your browser.

## Limitations

- Experiments use a simulated wireless network.
- Model accuracy on simulated data does not establish real-world accuracy.
- Testing with additional network topologies and traffic conditions is needed.

## Future Improvements

- Optimize prediction execution time.
- Test larger wireless networks.
- Evaluate additional traffic conditions.
- Explore real-world deployment.

