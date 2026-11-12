
import { useState, useEffect } from "react";
import "./App.css";

const API = "http://localhost:5000";

function Icon({ children }) {
  return <span className="icon">{children}</span>;
}

function MetricCard({ title, value, unit, icon, color, note }) {
  return (
    <div className="metric-card">
      <div className="metric-top">
        <span>{title}</span>
        <Icon>{icon}</Icon>
      </div>
      <div className={`metric-value ${color}`}>
        {value}<small>{unit}</small>
      </div>
      <div className="metric-note">{note}</div>
    </div>
  );
}

function ComparisonChart({ metric, label, unit, results }) {
  const max = Math.max(...results.map(r => r[metric]), 0.000001) * 1.18;

  return (
    <div className="chart-card">
      <div className="section-heading">
        <div>
          <h3>{label}</h3>
          <p>Comparison across all controllers</p>
        </div>
      </div>

      <div className="bar-chart">
        {results.map((item, index) => (
          <div className="bar-item" key={item.name}>
            <div className="bar-number">
              {Number(item[metric]).toFixed(metric === "loss" ? 4 : 2)} {unit}
            </div>
            <div className="bar-track">
              <div
                className={`bar-fill bar-${index}`}
                style={{
                  width: `${Math.max(2, item[metric] / max * 100)}%`
                }}
              />
            </div>
            <span>{item.name}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

export default function App() {
  const [page, setPage] = useState("Overview");
  const [selected, setSelected] = useState("AI Adaptive");

  const [comparison, setComparison] = useState([]);
  const [senders, setSenders] = useState([]);
  const [dataset, setDataset] = useState(null);
  const [apiStatus, setApiStatus] = useState("Connecting...");

  useEffect(() => {
    async function loadData() {
      try {
        const [comparisonRes, networkRes, datasetRes] =
          await Promise.all([
            fetch(`${API}/api/comparison`),
            fetch(`${API}/api/ai-results`),
            fetch(`${API}/api/dataset`)
          ]);

        if (!comparisonRes.ok || !networkRes.ok || !datasetRes.ok) {
          throw new Error("Backend request failed");
        }

        const comparisonData = await comparisonRes.json();
        const networkData = await networkRes.json();
        const datasetData = await datasetRes.json();

        setComparison(comparisonData.map(item => ({
          name: item.Controller === "AI" ? "AI Adaptive" : item.Controller,
          throughput: Number(item.Throughput_Mbps),
          delay: Number(item.Average_Delay_ms),
          loss: Number(item.Packet_Loss_Percentage),
          fairness: Number(item.Jain_Fairness)
        })));

        // Keep the latest recorded measurement for each sender
        const latestBySender = {};

        networkData.forEach(item => {
          const id = item.Sender_ID;

          if (!latestBySender[id] ||
              item.Time > latestBySender[id].Time) {
            latestBySender[id] = item;
          }
        });

        setSenders(Object.values(latestBySender).map(item => ({
          id: item.Sender_ID,
          rate: Number(item.Rate_Mbps),
          throughput: Number(item.Throughput_Mbps),
          status: item.AI_Prediction
        })));

        setDataset(datasetData);
        setApiStatus("Connected");
      } catch (error) {
        console.error("Backend connection error:", error);
        setApiStatus("Disconnected");
      }
    }

    loadData();
    const interval = setInterval(loadData, 5000);

    return () => clearInterval(interval);
  }, []);

  const displayResults = comparison;
  const current = displayResults.find(r => r.name === selected);

  const navigation = [
    { name: "Overview", icon: "◫" },
    { name: "Network", icon: "⌘" },
    { name: "AI Analysis", icon: "✧" },
    { name: "Comparison", icon: "▥" }
  ];

  const aiResult = displayResults.find(r => r.name === "AI Adaptive");

  return (
    <div className="app">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand-logo">N</div>
          <div>
            <h2>NetControl<span>AI</span></h2>
            <p>NETWORK INTELLIGENCE</p>
          </div>
        </div>

        <div className="nav-label">WORKSPACE</div>

        <nav>
          {navigation.map(item => (
            <button
              key={item.name}
              className={page === item.name ? "nav-item active" : "nav-item"}
              onClick={() => setPage(item.name)}
            >
              <span>{item.icon}</span>{item.name}
            </button>
          ))}
        </nav>

        <div className="sidebar-bottom">
          <div className="connection">
            <span className={`live-dot ${apiStatus === "Connected" ? "" : "offline"}`} />
            <div>
              <strong>{apiStatus}</strong>
              <small>Flask API · Port 5000</small>
            </div>
          </div>

          <div className="profile">
            <div className="avatar">K</div>
            <div>
              <strong>Project Dashboard</strong>
              <small>Administrator</small>
            </div>
          </div>
        </div>
      </aside>

      <main className="main">
        <header className="topbar">
          <div>
            <p className="breadcrumb">Workspace / {page}</p>
            <h1>{page === "Overview" ? "Network Overview" : page}</h1>
          </div>

          <div className="top-actions">
            <span className="status-pill">
              <span className={`live-dot ${apiStatus === "Connected" ? "" : "offline"}`} />
              {apiStatus}
            </span>
            <div className="avatar">K</div>
          </div>
        </header>

        {apiStatus !== "Connected" && (
          <div className="panel" style={{ color: "#ffbd69" }}>
            Backend disconnected. Start Flask using <code>python app.py</code>.
          </div>
        )}

        {page === "Overview" && (
          <>
            <section className="welcome">
              <div>
                <span className="eyebrow">AI-POWERED WIRELESS NETWORK</span>
                <h2>Intelligent Congestion Control</h2>
                <p>
                  Monitor network performance and analyze AI-driven traffic optimization.
                </p>
              </div>
              <button className="primary-button" onClick={() => setPage("Comparison")}>
                View comparison →
              </button>
            </section>

            <div className="section-heading">
              <div>
                <h3>Performance Metrics</h3>
                <p>Actual AI experiment results from Flask API</p>
              </div>
              <span className="tag">AI ADAPTIVE</span>
            </div>

            <section className="metric-grid">
              <MetricCard
                title="Network Throughput"
                value={aiResult ? aiResult.throughput.toFixed(2) : "--"}
                unit="Mbps" icon="↗" color="green"
                note="Total received throughput"
              />
              <MetricCard
                title="Average Delay"
                value={aiResult ? aiResult.delay.toFixed(2) : "--"}
                unit="ms" icon="◷" color="blue"
                note="End-to-end packet delay"
              />
              <MetricCard
                title="Packet Loss"
                value={aiResult ? aiResult.loss.toFixed(4) : "--"}
                unit="%" icon="⌁" color="purple"
                note="Packets not received"
              />
              <MetricCard
                title="Jain's Fairness"
                value={aiResult ? aiResult.fairness.toFixed(5) : "--"}
                unit="" icon="◎" color="orange"
                note="Fairness among senders"
              />
            </section>

            <div className="content-grid">
              <section className="panel">
                <div className="section-heading">
                  <div>
                    <h3>Wireless Network Topology</h3>
                    <p>Three senders communicating with one receiver</p>
                  </div>
                  <span className="tag">802.11n</span>
                </div>

                <div className="topology">
                  <div className="sender-nodes">
                    {[1, 2, 3].map(id => (
                      <div className="node" key={id}>
                        <div className="node-icon">⌁</div>
                        <strong>Sender {id}</strong>
                        <small>Node {id - 1}</small>
                      </div>
                    ))}
                  </div>

                  <div className="wireless-lines">
                    <div/><div/><div/>
                    <span>Wireless channel</span>
                  </div>

                  <div className="receiver">
                    <div className="receiver-icon">▣</div>
                    <strong>Receiver</strong>
                    <small>Destination node</small>
                  </div>
                </div>

                <div className="topology-footer">
                  <span className="live-dot"/>
                  Network topology from ns-3 simulation
                </div>
              </section>

              <section className="panel">
                <div className="section-heading">
                  <div>
                    <h3>AI Decision Engine</h3>
                    <p>Random Forest classification model</p>
                  </div>
                  <span className="ai-icon">✧</span>
                </div>

                <div className="ai-score">
                  <div className="score-ring">
                    <strong>99.05<small>%</small></strong>
                  </div>
                  <div>
                    <strong>Model Accuracy</strong>
                    <p>Held-out test dataset</p>
                  </div>
                </div>

                <div className="divider"/>
                <div className="decision-row">
                  <span>Algorithm</span><strong>Random Forest</strong>
                </div>
                <div className="decision-row">
                  <span>Training records</span><strong>2,112</strong>
                </div>
                <div className="decision-row">
                  <span>Testing records</span><strong>528</strong>
                </div>
                <div className="decision-row">
                  <span>Input features</span><strong>2</strong>
                </div>
                <div className="feature-chips">
                  <span>Transmission rate</span><span>Throughput</span>
                </div>
              </section>
            </div>

            <section className="panel sender-panel">
              <div className="section-heading">
                <div>
                  <h3>Sender Monitoring</h3>
                  <p>Latest recorded AI control measurements</p>
                </div>
                <span className="tag">3 SENDERS</span>
              </div>

              <div className="table-wrap">
                <table>
                  <thead>
                    <tr>
                      <th>NODE</th>
                      <th>TRANSMISSION RATE</th>
                      <th>THROUGHPUT</th>
                      <th>STATUS</th>
                    </tr>
                  </thead>
                  <tbody>
                    {senders.map(s => (
                      <tr key={s.id}>
                        <td><strong>Sender {s.id}</strong></td>
                        <td>{s.rate.toFixed(2)} Mbps</td>
                        <td>{s.throughput.toFixed(2)} Mbps</td>
                        <td>
                          <span className={s.status === "CONGESTED" ? "normal-status congested" : "normal-status"}>
                            ● {s.status}
                          </span>
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </section>
          </>
        )}

        {page === "Network" && (
          <>
            <section className="welcome">
              <div>
                <span className="eyebrow">WIRELESS MONITORING</span>
                <h2>Network Nodes</h2>
                <p>Latest sender-level measurements retrieved from Flask.</p>
              </div>
            </section>

            <section className="metric-grid">
              {senders.map(s => (
                <div className="metric-card" key={s.id}>
                  <div className="metric-top">
                    <span>Sender {s.id}</span><Icon>⌁</Icon>
                  </div>
                  <div className="metric-value blue">
                    {s.rate.toFixed(2)}<small>Mbps rate</small>
                  </div>
                  <div className="metric-note">
                    Throughput: {s.throughput.toFixed(3)} Mbps
                  </div>
                </div>
              ))}
            </section>

            <section className="panel">
              <h3>Network Configuration</h3>
              <div className="decision-row">
                <span>Standard</span><strong>IEEE 802.11n</strong>
              </div>
              <div className="decision-row">
                <span>Topology</span><strong>Ad hoc wireless</strong>
              </div>
              <div className="decision-row">
                <span>Nodes</span><strong>3 senders + 1 receiver</strong>
              </div>
              <div className="decision-row">
                <span>Transport</span><strong>UDP</strong>
              </div>
              <div className="decision-row">
                <span>Simulator</span><strong>ns-3.46.1</strong>
              </div>
            </section>
          </>
        )}

        {page === "AI Analysis" && (
          <>
            <section className="welcome">
              <div>
                <span className="eyebrow">MACHINE LEARNING</span>
                <h2>AI Congestion Analysis</h2>
                <p>Random Forest model trained on one-second wireless measurements.</p>
              </div>
            </section>

            <section className="metric-grid">
              <MetricCard
                title="Test Accuracy" value="99.05" unit="%"
                icon="✧" color="green" note="528 held-out samples"
              />
              <MetricCard
                title="Training Samples"
                value={dataset ? dataset.total_records - 528 : "--"}
                unit="" icon="▤" color="blue" note="Training records"
              />
              <MetricCard
                title="Congested Samples"
                value={dataset ? dataset.congested_records : "--"}
                unit="" icon="⚠" color="purple" note="Delay above 100 ms"
              />
              <MetricCard
                title="Features" value="2" unit=""
                icon="⌘" color="orange" note="Rate and throughput"
              />
            </section>

            {dataset && (
              <section className="panel">
                <h3>Training Dataset Information</h3>
                <div className="decision-row">
                  <span>Total records</span><strong>{dataset.total_records}</strong>
                </div>
                <div className="decision-row">
                  <span>Normal records</span><strong>{dataset.normal_records}</strong>
                </div>
                <div className="decision-row">
                  <span>Congested records</span><strong>{dataset.congested_records}</strong>
                </div>
                <div className="decision-row">
                  <span>Traffic rates</span><strong>{dataset.traffic_rates.join(", ")} Mbps</strong>
                </div>
              </section>
            )}

            <section className="panel">
              <h3>How the AI controller works</h3>
              <div className="workflow">
                <div>
                  <b>01</b><strong>Measure</strong>
                  <p>Collect sender rate and one-second throughput.</p>
                </div>
                <span>→</span>
                <div>
                  <b>02</b><strong>Predict</strong>
                  <p>Random Forest classifies network congestion.</p>
                </div>
                <span>→</span>
                <div>
                  <b>03</b><strong>Control</strong>
                  <p>Decrease or increase the sender rate.</p>
                </div>
              </div>
              <div className="divider"/>
              <div className="decision-row">
                <span>Congested prediction</span>
                <strong>Multiplicative decrease × 0.70</strong>
              </div>
              <div className="decision-row">
                <span>Normal prediction</span>
                <strong>Additive increase +1 Mbps</strong>
              </div>
            </section>
          </>
        )}

        {page === "Comparison" && (
          <>
            <section className="welcome">
              <div>
                <span className="eyebrow">EXPERIMENTAL RESULTS</span>
                <h2>Controller Comparison</h2>
                <p>Actual results fetched from your ns-3 experiments.</p>
              </div>
            </section>

            {current && (
              <>
                <section className="section-heading">
                  <div>
                    <h3>Choose a controller</h3>
                    <p>View its measured performance</p>
                  </div>
                </section>

                <div className="controller-tabs">
                  {displayResults.map(r => (
                    <button
                      key={r.name}
                      className={selected === r.name ? "controller-tab selected" : "controller-tab"}
                      onClick={() => setSelected(r.name)}
                    >
                      {r.name}
                    </button>
                  ))}
                </div>

                <section className="metric-grid">
                  <MetricCard title="Throughput" value={current.throughput.toFixed(2)} unit="Mbps" icon="↗" color="green" note="Total received throughput"/>
                  <MetricCard title="Average Delay" value={current.delay.toFixed(2)} unit="ms" icon="◷" color="blue" note="End-to-end delay"/>
                  <MetricCard title="Packet Loss" value={current.loss.toFixed(4)} unit="%" icon="⌁" color="purple" note="Packet loss percentage"/>
                  <MetricCard title="Jain's Fairness" value={current.fairness.toFixed(5)} unit="" icon="◎" color="orange" note="Fairness across senders"/>
                </section>

                <div className="content-grid">
                  <ComparisonChart metric="throughput" label="Throughput" unit="Mbps" results={displayResults}/>
                  <ComparisonChart metric="delay" label="Average Delay" unit="ms" results={displayResults}/>
                  <ComparisonChart metric="loss" label="Packet Loss" unit="%" results={displayResults}/>
                  <ComparisonChart metric="fairness" label="Jain's Fairness" unit="" results={displayResults}/>
                </div>

                <section className="panel">
                  <h3>Complete comparison data</h3>
                  <div className="table-wrap">
                    <table>
                      <thead>
                        <tr>
                          <th>CONTROLLER</th><th>THROUGHPUT</th>
                          <th>DELAY</th><th>PACKET LOSS</th><th>FAIRNESS</th>
                        </tr>
                      </thead>
                      <tbody>
                        {displayResults.map(r => (
                          <tr key={r.name}>
                            <td><strong>{r.name}</strong></td>
                            <td>{r.throughput.toFixed(3)}</td>
                            <td>{r.delay.toFixed(3)}</td>
                            <td>{r.loss.toFixed(5)}%</td>
                            <td>{r.fairness.toFixed(5)}</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                </section>
              </>
            )}
          </>
        )}

        <footer>
          NetControl AI · AI-Based Congestion Control for Wireless Networks
          <span>React + Flask + ns-3 + Random Forest</span>
        </footer>
      </main>
    </div>
  );
}
