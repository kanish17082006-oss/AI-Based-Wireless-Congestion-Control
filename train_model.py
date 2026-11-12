import pandas as pd
import joblib
import matplotlib.pyplot as plt

from sklearn.ensemble import RandomForestClassifier
from sklearn.model_selection import GroupShuffleSplit
from sklearn.metrics import (
    accuracy_score,
    classification_report,
    confusion_matrix,
    ConfusionMatrixDisplay
)

# 1. Load one-second simulation dataset
df = pd.read_csv("window-results.csv")

# 2. Create congestion labels
df["Congestion"] = (
    df["Average_Delay_ms"] > 100
).astype(int)

# 3. Select runtime features
features = ["Rate_Mbps", "Throughput_Mbps"]
X = df[features]
y = df["Congestion"]

# 4. Split by simulation seed
splitter = GroupShuffleSplit(
    n_splits=1,
    test_size=0.2,
    random_state=42
)

train_idx, test_idx = next(
    splitter.split(X, y, groups=df["Seed"])
)

X_train = X.iloc[train_idx]
X_test = X.iloc[test_idx]
y_train = y.iloc[train_idx]
y_test = y.iloc[test_idx]

# 5. Train Random Forest
model = RandomForestClassifier(
    n_estimators=200,
    max_depth=8,
    class_weight="balanced",
    random_state=42
)

model.fit(X_train, y_train)

# 6. Evaluate
predictions = model.predict(X_test)

print("\n========== MODEL RESULTS ==========")
print("Total records:", len(df))
print("Normal records:", (y == 0).sum())
print("Congested records:", (y == 1).sum())
print("Training records:", len(X_train))
print("Testing records:", len(X_test))

print("\nAccuracy:",
      round(accuracy_score(y_test, predictions) * 100, 2), "%")

print("\nClassification Report:")
print(classification_report(
    y_test,
    predictions,
    target_names=["Normal", "Congested"],
    zero_division=0
))

# 7. Save model
joblib.dump(model, "congestion_model.pkl")
print("\nModel saved: congestion_model.pkl")

# 8. Save confusion matrix
cm = confusion_matrix(y_test, predictions)

display = ConfusionMatrixDisplay(
    confusion_matrix=cm,
    display_labels=["Normal", "Congested"]
)

display.plot(cmap="Blues")
plt.title("AI Congestion Detection")
plt.savefig("confusion_matrix.png")
plt.close()

# 9. Save feature importance
importance = pd.Series(
    model.feature_importances_,
    index=features
).sort_values()

importance.plot(kind="barh")
plt.title("Feature Importance")
plt.xlabel("Importance")
plt.tight_layout()
plt.savefig("feature_importance.png")
plt.close()

print("Graphs saved successfully!")
