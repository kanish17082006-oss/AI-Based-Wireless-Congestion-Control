import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv("comparison-results.csv")

metrics = [
    ("Throughput_Mbps", "Throughput Comparison", "Mbps"),
    ("Average_Delay_ms", "Average Delay Comparison", "Milliseconds"),
    ("Packet_Loss_Percentage", "Packet Loss Comparison", "%"),
    ("Jain_Fairness", "Jain's Fairness Comparison", "Index")
]

for column, title, ylabel in metrics:
    plt.figure(figsize=(8, 5))
    bars = plt.bar(df["Controller"], df[column])

    plt.title(title)
    plt.xlabel("Congestion Control Method")
    plt.ylabel(ylabel)
    plt.grid(axis="y", alpha=0.3)

    for bar in bars:
        value = bar.get_height()
        plt.text(
            bar.get_x() + bar.get_width() / 2,
            value,
            f"{value:.4f}",
            ha="center",
            va="bottom"
        )

    plt.tight_layout()
    filename = column.lower() + "_comparison.png"
    plt.savefig(filename, dpi=300)
    plt.close()

print("All four comparison graphs generated!")
