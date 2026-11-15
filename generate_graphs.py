import csv
import matplotlib.pyplot as plt

with open("comparison-average-results.csv", "r") as file:
    rows = list(csv.DictReader(file))

controllers = [row["Controller"] for row in rows]

metrics = [
    ("Throughput_Mbps_2_to_10s", "Throughput Comparison", "Throughput (Mbps)", "throughput_comparison.png"),
    ("Average_Delay_ms_Whole_Simulation", "Average Delay Comparison", "Delay (ms)", "delay_comparison.png"),
    ("Packet_Loss_Percentage_Whole_Simulation", "Packet Loss Comparison", "Packet Loss (%)", "packet_loss_comparison.png"),
    ("Jain_Fairness_2_to_10s", "Jain Fairness Comparison", "Jain Fairness Index", "fairness_comparison.png")
]

for column, title, ylabel, filename in metrics:
    values = [float(row[column]) for row in rows]

    plt.figure(figsize=(8, 5))
    bars = plt.bar(controllers, values)

    plt.title(title)
    plt.xlabel("Congestion Control Method")
    plt.ylabel(ylabel)
    plt.grid(axis="y", linestyle="--", alpha=0.4)

    for bar, value in zip(bars, values):
        plt.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height(),
            f"{value:.4f}",
            ha="center",
            va="bottom"
        )

    if column == "Jain_Fairness_2_to_10s":
        plt.ylim(0.99, 1.001)

    plt.tight_layout()
    plt.savefig(filename, dpi=300)
    plt.close()

    print("Created:", filename)

print("\nAll four graphs generated successfully.")
