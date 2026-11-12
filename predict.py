import sys
import joblib
import pandas as pd

model = joblib.load("congestion_model.pkl")

rate = float(sys.argv[1])
throughput = float(sys.argv[2])

features = pd.DataFrame([{
    "Rate_Mbps": rate,
    "Throughput_Mbps": throughput
}])

prediction = int(model.predict(features)[0])

print(prediction)
