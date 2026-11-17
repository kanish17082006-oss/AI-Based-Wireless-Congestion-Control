
from flask import Flask, jsonify
from flask_cors import CORS
import pandas as pd
import os

app = Flask(__name__)
CORS(app)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))


def read_csv(filename):
    path = os.path.join(BASE_DIR, filename)

    if not os.path.exists(path):
        return None

    return pd.read_csv(path)


@app.route("/")
def home():
    return jsonify({
        "message": "AI Congestion Control API is running",
        "status": "online"
    })


@app.route("/api/comparison")
def comparison():
    df = read_csv("comparison-average-results.csv")

    if df is None:
        return jsonify({"error": "Comparison results not found"}), 404

    df = df.rename(columns={
        "Throughput_Mbps_2_to_10s": "Throughput_Mbps",
        "Average_Delay_ms_Whole_Simulation": "Average_Delay_ms",
        "Packet_Loss_Percentage_Whole_Simulation": "Packet_Loss_Percentage",
        "Jain_Fairness_2_to_10s": "Jain_Fairness"
    })

    return jsonify(df.to_dict(orient="records"))


@app.route("/api/ai-results")
def ai_results():
    df = read_csv("ai-control-results.csv")

    if df is None:
        return jsonify({"error": "AI results not found"}), 404

    return jsonify(df.to_dict(orient="records"))


@app.route("/api/network")
def network():
    df = read_csv("ai-control-results.csv")

    if df is None or df.empty:
        return jsonify({"error": "AI results not found"}), 404

    latest = df[df["Time"] == df["Time"].max()]

    return jsonify(latest.to_dict(orient="records"))


@app.route("/api/dataset")
def dataset():
    df = read_csv("window-results.csv")

    if df is None:
        return jsonify({"error": "Dataset not found"}), 404

    return jsonify({
        "total_records": int(len(df)),
        "normal_records": int((df["Average_Delay_ms"] <= 100).sum()),
        "congested_records": int((df["Average_Delay_ms"] > 100).sum()),
        "traffic_rates": sorted(df["Rate_Mbps"].unique().tolist())
    })


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
