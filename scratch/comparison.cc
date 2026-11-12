#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

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
    Ptr<PacketSink> sink;
    Ptr<OnOffApplication> app;
    uint64_t lastRx = 0;
    double rate = 15.0;
    uint32_t id = 0;
};

std::vector<Sender> senders;
ControllerType currentController;
std::ofstream controlLog;

int Predict(double rate, double throughput)
{
    std::ostringstream command;

    command << ".venv/bin/python predict.py "
            << std::fixed << std::setprecision(4)
            << rate << " " << throughput;

    FILE* pipe = popen(command.str().c_str(), "r");

    if (!pipe) return -1;

    char buffer[128];
    std::string output;

    while (fgets(buffer, sizeof(buffer), pipe))
        output += buffer;

    int status = pclose(pipe);

    if (status != 0 || output.empty())
        return -1;

    try
    {
        int result = std::stoi(output);
        return (result == 0 || result == 1) ? result : -1;
    }
    catch (...)
    {
        return -1;
    }
}

void ControlLoop()
{
    for (auto& sender : senders)
    {
        uint64_t currentRx = sender.sink->GetTotalRx();
        uint64_t received = currentRx - sender.lastRx;

        double throughput =
            received * 8.0 / 1000000.0;

        sender.lastRx = currentRx;

        double oldRate = sender.rate;
        int prediction = 0;

        if (currentController == AI)
        {
            prediction = Predict(sender.rate, throughput);
        }
        else
        {
            prediction =
                (throughput < sender.rate * 0.85) ? 1 : 0;
        }

        if (prediction == 1)
        {
            sender.rate =
                std::max(1.0, sender.rate * 0.70);
        }
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

    if (Simulator::Now() < Seconds(10))
    {
        Simulator::Schedule(
            Seconds(1),
            &ControlLoop
        );
    }
}

void RunExperiment(
    ControllerType type,
    const std::string& name,
    std::ofstream& summary,
    uint32_t run)
{
    std::cout << "\n========== " << name
              << " EXPERIMENT | RUN " << run
              << " ==========\n";

    // Reset the random configuration for a fair comparison.
    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(run);

    currentController = type;
    senders.clear();

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

    InternetStackHelper internet;
    internet.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase(
        "10.1.1.0",
        "255.255.255.0"
    );

    Ipv4InterfaceContainer interfaces =
        address.Assign(devices);

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

        Ptr<PacketSink> sink =
            DynamicCast<PacketSink>(
                sinkApps.Get(0)
            );

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

        sender.sink = sink;

        sender.app =
            DynamicCast<OnOffApplication>(
                sourceApps.Get(0)
            );

        sender.lastRx = 0;
        sender.rate = 15.0;
        sender.id = i + 1;

        senders.push_back(sender);
    }

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
            Seconds(2),
            &ControlLoop
        );
    }

    FlowMonitorHelper flowHelper;

    Ptr<FlowMonitor> monitor =
        flowHelper.InstallAll();

    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(
            flowHelper.GetClassifier()
        );

    Simulator::Stop(Seconds(12));
    Simulator::Run();

    monitor->CheckForLostPackets();

    auto stats = monitor->GetFlowStats();

    double totalRxMbps = 0;
    double totalTxPackets = 0;
    double totalRxPackets = 0;
    double totalDelay = 0;
    double totalDelayPackets = 0;

    std::vector<double> senderThroughputs(
        3, 0.0
    );

    for (auto it = stats.begin();
         it != stats.end(); ++it)
    {
        auto tuple =
            classifier->FindFlow(it->first);

        if (tuple.destinationPort < 5000 ||
            tuple.destinationPort > 5002)
        {
            continue;
        }

        uint32_t id =
            tuple.destinationPort - 5000;

        auto flow = it->second;

        double duration = 10.0;

        double throughput =
            flow.rxBytes * 8.0 /
            duration / 1000000.0;

        senderThroughputs[id] += throughput;

        totalRxMbps += throughput;
        totalTxPackets += flow.txPackets;
        totalRxPackets += flow.rxPackets;

        totalDelay +=
            flow.delaySum.GetSeconds();

        totalDelayPackets += flow.rxPackets;
    }

    double loss = 0;

    if (totalTxPackets > 0)
    {
        loss =
            (totalTxPackets - totalRxPackets)
            * 100.0 / totalTxPackets;
    }

    double avgDelay = 0;

    if (totalDelayPackets > 0)
    {
        avgDelay =
            totalDelay * 1000.0 /
            totalDelayPackets;
    }

    double sum = 0;
    double sumSquares = 0;

    for (double value : senderThroughputs)
    {
        sum += value;
        sumSquares += value * value;
    }

    double fairness = 0;

    if (sumSquares > 0)
    {
        fairness =
            (sum * sum) /
            (3.0 * sumSquares);
    }

    summary
        << run << ","
        << name << ","
        << totalRxMbps << ","
        << avgDelay << ","
        << loss << ","
        << fairness << "\n";

    std::cout
        << "\nRESULTS: " << name << "\n";

    std::cout
        << "Throughput: "
        << totalRxMbps << " Mbps\n";

    std::cout
        << "Average delay: "
        << avgDelay << " ms\n";

    std::cout
        << "Packet loss: "
        << loss << " %\n";

    std::cout
        << "Jain fairness: "
        << fairness << "\n";

    if (type != FIXED)
    {
        controlLog.close();
    }

    Simulator::Destroy();
}

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

    std::string filename =
        "comparison-results-run-" +
        std::to_string(run) + ".csv";

    std::ofstream summary(filename);

    summary
        << "Run,Controller,Throughput_Mbps,"
        << "Average_Delay_ms,"
        << "Packet_Loss_Percentage,"
        << "Jain_Fairness\n";

    RunExperiment(
        FIXED,
        "Fixed",
        summary,
        run
    );

    RunExperiment(
        THRESHOLD,
        "Threshold",
        summary,
        run
    );

    RunExperiment(
        AI,
        "AI",
        summary,
        run
    );

    summary.close();

    std::cout
        << "\n================================\n";

    std::cout
        << "ALL COMPARISONS COMPLETED!\n";

    std::cout
        << "File: " << filename << "\n";

    std::cout
        << "================================\n";

    return 0;
}

