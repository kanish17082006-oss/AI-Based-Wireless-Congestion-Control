/*
 * comparison.cc — AI-Based Congestion Control for Wireless Networks
 *
 * Validated measurement design:
 *   - AI and Threshold controllers use FlowMonitor IP-layer rxBytes,
 *     matching the throughput feature used to train the Random Forest model.
 *   - First controller observation is at 3 seconds (cumulative bytes from
 *     simulation start), followed by one-second byte-difference windows.
 *   - Final throughput and Jain fairness use one common interval: 2s to 10s.
 *   - Average delay and packet loss are reported over the complete simulation.
 *   - New output filenames are used so previous experiment results are preserved.
 *
 * Note: This program does not calculate PHY channel-busy utilization.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

#include <map>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace ns3;

enum ControllerType
{
    FIXED,
    THRESHOLD,
    AI
};

struct Sender
{
    Ptr<OnOffApplication> app;
    double rate = 15.0;
    uint32_t id = 0;
};

std::vector<Sender> senders;

ControllerType currentController;
std::ofstream controlLog;

Ptr<FlowMonitor> g_monitor;
Ptr<Ipv4FlowClassifier> g_classifier;

std::map<uint32_t, uint64_t> previousRxBytes;
std::map<uint32_t, uint64_t> windowStartRxBytes;
std::map<uint32_t, uint64_t> windowEndRxBytes;

constexpr double MEASUREMENT_START = 2.0;
constexpr double MEASUREMENT_END = 10.0;
constexpr double MEASUREMENT_DURATION =
    MEASUREMENT_END - MEASUREMENT_START;


// Capture FlowMonitor counters at the beginning of the common window.
void CaptureWindowStart()
{
    windowStartRxBytes.clear();

    auto stats = g_monitor->GetFlowStats();

    for (const auto& entry : stats)
    {
        windowStartRxBytes[entry.first] =
            entry.second.rxBytes;
    }
}


// Capture FlowMonitor counters at the end of the common window.
void CaptureWindowEnd()
{
    windowEndRxBytes.clear();

    auto stats = g_monitor->GetFlowStats();

    for (const auto& entry : stats)
    {
        windowEndRxBytes[entry.first] =
            entry.second.rxBytes;
    }
}


// Get congestion prediction from the trained Python model.
int Predict(double rate, double throughput)
{
    std::ostringstream command;

    command << ".venv/bin/python predict.py "
            << std::fixed << std::setprecision(4)
            << rate << " " << throughput;

    FILE* pipe = popen(command.str().c_str(), "r");

    if (!pipe)
        return -1;

    char buffer[128];
    std::string output;

    while (fgets(buffer, sizeof(buffer), pipe))
    {
        output += buffer;
    }

    int status = pclose(pipe);

    if (status != 0 || output.empty())
        return -1;

    try
    {
        int result = std::stoi(output);

        return (result == 0 || result == 1)
                   ? result
                   : -1;
    }
    catch (...)
    {
        return -1;
    }
}


// Periodically measure throughput and update sender rates.
void ControlLoop()
{
    auto stats = g_monitor->GetFlowStats();

    for (auto& sender : senders)
    {
        uint64_t receivedBytes = 0;

        for (const auto& entry : stats)
        {
            auto tuple =
                g_classifier->FindFlow(entry.first);

            if (tuple.destinationPort == 4999 + sender.id)
            {
                uint64_t currentRxBytes =
                    entry.second.rxBytes;

                uint64_t previousBytes = 0;

                auto previous =
                    previousRxBytes.find(entry.first);

                if (previous != previousRxBytes.end())
                {
                    previousBytes = previous->second;
                }

                if (currentRxBytes >= previousBytes)
                {
                    receivedBytes =
                        currentRxBytes - previousBytes;
                }

                previousRxBytes[entry.first] =
                    currentRxBytes;

                break;
            }
        }

        // The first observation at t=3 is cumulative.
        // Subsequent observations represent one-second intervals.
        double throughput =
            receivedBytes * 8.0 / 1000000.0;

        double oldRate = sender.rate;

        int prediction = 0;

        if (currentController == AI)
        {
            prediction =
                Predict(sender.rate, throughput);
        }
        else
        {
            prediction =
                (throughput < sender.rate * 0.85)
                    ? 1
                    : 0;
        }

        // Multiplicative decrease during congestion.
        if (prediction == 1)
        {
            sender.rate =
                std::max(1.0, sender.rate * 0.70);
        }

        // Additive increase during normal conditions.
        else if (prediction == 0)
        {
            sender.rate =
                std::min(30.0, sender.rate + 1.0);
        }

        uint64_t bps =
            static_cast<uint64_t>(
                sender.rate * 1000000.0
            );

        sender.app->SetAttribute(
            "DataRate",
            DataRateValue(DataRate(bps))
        );

        controlLog
            << Simulator::Now().GetSeconds() << ","
            << sender.id << ","
            << oldRate << ","
            << throughput << ","
            << sender.rate << ","
            << prediction << "\n";
    }

    if (Simulator::Now() < Seconds(MEASUREMENT_END))
    {
        Simulator::Schedule(
            Seconds(1),
            &ControlLoop
        );
    }
}


// Run one experiment for a selected controller.
void RunExperiment(
    ControllerType type,
    const std::string& name,
    std::ofstream& summary,
    std::ofstream& correctedSummary,
    uint32_t run)
{
    std::cout << "\n========== " << name
              << " EXPERIMENT | RUN " << run
              << " ==========\n";

    // Reset random configuration.
    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(run);

    currentController = type;

    senders.clear();

    previousRxBytes.clear();
    windowStartRxBytes.clear();
    windowEndRxBytes.clear();

    // Create four nodes:
    // Three senders and one receiver.
    NodeContainer nodes;
    nodes.Create(4);

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211n);

    YansWifiChannelHelper channel =
        YansWifiChannelHelper::Default();

    YansWifiPhyHelper phy;
    phy.SetChannel(channel.Create());

    WifiMacHelper mac;
    mac.SetType("ns3::AdhocWifiMac");

    NetDeviceContainer devices =
        wifi.Install(phy, mac, nodes);

    // Configure node positions.
    MobilityHelper mobility;

    Ptr<ListPositionAllocator> positions =
        CreateObject<ListPositionAllocator>();

    positions->Add(Vector(0, 0, 0));
    positions->Add(Vector(5, 0, 0));
    positions->Add(Vector(0, 5, 0));
    positions->Add(Vector(5, 5, 0));

    mobility.SetPositionAllocator(positions);

    mobility.SetMobilityModel(
        "ns3::ConstantPositionMobilityModel"
    );

    mobility.Install(nodes);

    // Install Internet stack.
    InternetStackHelper internet;
    internet.Install(nodes);

    // Assign IP addresses.
    Ipv4AddressHelper address;

    address.SetBase(
        "10.1.1.0",
        "255.255.255.0"
    );

    Ipv4InterfaceContainer interfaces =
        address.Assign(devices);

    // Install three UDP senders.
    for (uint32_t i = 0; i < 3; i++)
    {
        uint16_t port = 5000 + i;

        PacketSinkHelper sinkHelper(
            "ns3::UdpSocketFactory",
            InetSocketAddress(
                Ipv4Address::GetAny(),
                port
            )
        );

        ApplicationContainer sinkApps =
            sinkHelper.Install(nodes.Get(3));

        sinkApps.Start(Seconds(0));
        sinkApps.Stop(Seconds(12));

        OnOffHelper source(
            "ns3::UdpSocketFactory",
            InetSocketAddress(
                interfaces.GetAddress(3),
                port
            )
        );

        source.SetAttribute(
            "PacketSize",
            UintegerValue(1024)
        );

        source.SetAttribute(
            "OnTime",
            StringValue(
                "ns3::ConstantRandomVariable[Constant=1]"
            )
        );

        source.SetAttribute(
            "OffTime",
            StringValue(
                "ns3::ConstantRandomVariable[Constant=0]"
            )
        );

        source.SetAttribute(
            "DataRate",
            DataRateValue(DataRate(15000000))
        );

        ApplicationContainer sourceApps =
            source.Install(nodes.Get(i));

        sourceApps.Start(
            Seconds(1.0 + i * 0.2)
        );

        sourceApps.Stop(
            Seconds(11.0 + i * 0.2)
        );

        Sender sender;

        sender.app =
            DynamicCast<OnOffApplication>(
                sourceApps.Get(0)
            );

        sender.rate = 15.0;
        sender.id = i + 1;

        senders.push_back(sender);
    }

    // Install FlowMonitor.
    FlowMonitorHelper flowHelper;

    Ptr<FlowMonitor> monitor =
        flowHelper.InstallAll();

    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(
            flowHelper.GetClassifier()
        );

    g_monitor = monitor;
    g_classifier = classifier;

    // Capture counters over the same [2s, 10s) interval.
    Simulator::Schedule(
        Seconds(MEASUREMENT_START),
        &CaptureWindowStart
    );

    Simulator::Schedule(
        Seconds(MEASUREMENT_END),
        &CaptureWindowEnd
    );

    // Start adaptive controllers.
    if (type != FIXED)
    {
        std::string logName =
            name + "-control-run-" +
            std::to_string(run) + ".csv";

        controlLog.open(logName);

        controlLog
            << "Time,Sender_ID,Old_Rate,Throughput,"
            << "New_Rate,Prediction\n";

        Simulator::Schedule(
            Seconds(3),
            &ControlLoop
        );
    }

    Simulator::Stop(Seconds(12));

    Simulator::Run();

    monitor->CheckForLostPackets();

    auto stats = monitor->GetFlowStats();

    // Calculate whole-simulation delay and packet loss.
    double totalTxPackets = 0.0;
    double totalRxPackets = 0.0;

    double totalDelaySeconds = 0.0;
    double totalDelayPackets = 0.0;

    for (const auto& entry : stats)
    {
        auto tuple =
            classifier->FindFlow(entry.first);

        if (tuple.destinationPort < 5000 ||
            tuple.destinationPort > 5002)
        {
            continue;
        }

        const auto& flow = entry.second;

        totalTxPackets += flow.txPackets;
        totalRxPackets += flow.rxPackets;

        totalDelaySeconds +=
            flow.delaySum.GetSeconds();

        totalDelayPackets += flow.rxPackets;
    }

    double lossPercentage = 0.0;

    if (totalTxPackets > 0.0)
    {
        lossPercentage =
            (totalTxPackets - totalRxPackets) * 100.0 /
            totalTxPackets;
    }

    double averageDelayMs = 0.0;

    if (totalDelayPackets > 0.0)
    {
        averageDelayMs =
            totalDelaySeconds * 1000.0 /
            totalDelayPackets;
    }

    // Calculate per-sender throughput over the common interval.
    std::vector<double> senderThroughputs(3, 0.0);

    for (const auto& entry : windowEndRxBytes)
    {
        uint32_t flowId = entry.first;

        auto tuple =
            classifier->FindFlow(flowId);

        if (tuple.destinationPort < 5000 ||
            tuple.destinationPort > 5002)
        {
            continue;
        }

        uint32_t senderIndex =
            tuple.destinationPort - 5000;

        uint64_t startRxBytes = 0;

        auto start =
            windowStartRxBytes.find(flowId);

        if (start != windowStartRxBytes.end())
        {
            startRxBytes = start->second;
        }

        uint64_t endRxBytes = entry.second;

        uint64_t intervalRxBytes =
            (endRxBytes >= startRxBytes)
                ? endRxBytes - startRxBytes
                : 0;

        senderThroughputs[senderIndex] +=
            intervalRxBytes * 8.0 /
            MEASUREMENT_DURATION / 1000000.0;
    }

    // Calculate aggregate throughput and Jain fairness.
    double totalThroughputMbps = 0.0;

    double fairnessNumerator = 0.0;
    double fairnessDenominator = 0.0;

    for (double throughput : senderThroughputs)
    {
        totalThroughputMbps += throughput;

        fairnessNumerator += throughput;

        fairnessDenominator +=
            throughput * throughput;
    }

    double jainFairness = 0.0;

    if (fairnessDenominator > 0.0)
    {
        jainFairness =
            (fairnessNumerator * fairnessNumerator) /
            (3.0 * fairnessDenominator);
    }

    // Write common-window aggregate results.
    summary
        << run << ","
        << name << ","
        << totalThroughputMbps << ","
        << averageDelayMs << ","
        << lossPercentage << ","
        << jainFairness << "\n";

    // Write individual sender throughput results.
    correctedSummary
        << run << ","
        << name << ","
        << MEASUREMENT_START << ","
        << MEASUREMENT_END << ","
        << senderThroughputs[0] << ","
        << senderThroughputs[1] << ","
        << senderThroughputs[2] << ","
        << totalThroughputMbps << ","
        << jainFairness << "\n";

    // Display experiment results.
    std::cout
        << "\nRESULTS: " << name << "\n";

    std::cout
        << "Aggregate throughput (2s-10s): "
        << totalThroughputMbps << " Mbps\n";

    std::cout
        << "Sender throughputs (2s-10s): "
        << senderThroughputs[0] << ", "
        << senderThroughputs[1] << ", "
        << senderThroughputs[2] << " Mbps\n";

    std::cout
        << "Average delay (whole simulation): "
        << averageDelayMs << " ms\n";

    std::cout
        << "Packet loss (whole simulation): "
        << lossPercentage << " %\n";

    std::cout
        << "Jain fairness (2s-10s): "
        << jainFairness << "\n";

    if (type != FIXED)
    {
        controlLog.close();
    }

    Simulator::Destroy();
}


// Main function.
int main(int argc, char* argv[])
{
    uint32_t run = 1;

    CommandLine cmd;

    cmd.AddValue(
        "run",
        "Random run number",
        run
    );

    cmd.Parse(argc, argv);

    // Common-window summary CSV.
    std::string filename =
        "comparison-results-validated-run-" +
        std::to_string(run) + ".csv";

    // Detailed per-sender CSV.
    std::string correctedFilename =
        "comparison-results-window-details-run-" +
        std::to_string(run) + ".csv";

    std::ofstream summary(filename);

    summary
        << "Run,Controller,Throughput_Mbps_2_to_10s,"
        << "Average_Delay_ms_Whole_Simulation,"
        << "Packet_Loss_Percentage_Whole_Simulation,"
        << "Jain_Fairness_2_to_10s\n";

    std::ofstream correctedSummary(correctedFilename);

    correctedSummary
        << "Run,Controller,Window_Start_s,Window_End_s,"
        << "Sender1_Throughput_Mbps,Sender2_Throughput_Mbps,"
        << "Sender3_Throughput_Mbps,Aggregate_Throughput_Mbps,"
        << "Jain_Fairness\n";

    // Run Fixed controller.
    RunExperiment(
        FIXED,
        "Fixed",
        summary,
        correctedSummary,
        run
    );

    // Run Threshold controller.
    RunExperiment(
        THRESHOLD,
        "Threshold",
        summary,
        correctedSummary,
        run
    );

    // Run AI controller.
    RunExperiment(
        AI,
        "AI",
        summary,
        correctedSummary,
        run
    );

    summary.close();
    correctedSummary.close();

    std::cout
        << "\n================================\n";

    std::cout
        << "ALL COMPARISONS COMPLETED!\n";

    std::cout
        << "Validated results: " << filename << "\n";

    std::cout
        << "Per-sender details: "
        << correctedFilename << "\n";

    std::cout
        << "================================\n";

    return 0;
}