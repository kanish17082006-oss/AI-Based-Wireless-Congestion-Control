
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

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("WirelessBaseline");

void RunScenario(uint32_t rateMbps,
                 uint32_t seed,
                 std::ofstream& csv)
{
    // Use a different random run for each experiment
    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(seed);

    std::cout << "\n====================================\n";
    std::cout << "Rate: " << rateMbps << " Mbps per sender\n";
    std::cout << "Seed: " << seed << "\n";
    std::cout << "Total offered traffic: "
              << rateMbps * 3 << " Mbps\n";
    std::cout << "====================================\n";

    // Create 3 senders and 1 receiver
    NodeContainer nodes;
    nodes.Create(4);

    // Configure Wi-Fi
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

    // Assign node positions
    MobilityHelper mobility;

    Ptr<ListPositionAllocator> positions =
        CreateObject<ListPositionAllocator>();

    positions->Add(Vector(0.0, 0.0, 0.0));
    positions->Add(Vector(5.0, 0.0, 0.0));
    positions->Add(Vector(0.0, 5.0, 0.0));
    positions->Add(Vector(5.0, 5.0, 0.0));

    mobility.SetPositionAllocator(positions);

    mobility.SetMobilityModel(
        "ns3::ConstantPositionMobilityModel");

    mobility.Install(nodes);

    // Install Internet stack
    InternetStackHelper internet;
    internet.Install(nodes);

    // Assign IP addresses
    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");

    Ipv4InterfaceContainer interfaces =
        address.Assign(devices);

    uint16_t port = 5000;

    // Receiver application
    PacketSinkHelper sink(
        "ns3::UdpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), port));

    ApplicationContainer sinkApp =
        sink.Install(nodes.Get(3));

    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(10.0));

    // Configure sender traffic
    OnOffHelper source(
        "ns3::UdpSocketFactory",
        InetSocketAddress(interfaces.GetAddress(3), port));

    std::string dataRate =
        std::to_string(rateMbps) + "Mbps";

    source.SetAttribute("DataRate",
                        StringValue(dataRate));

    source.SetAttribute("PacketSize",
                        UintegerValue(1024));

    source.SetAttribute("OnTime",
        StringValue(
            "ns3::ConstantRandomVariable[Constant=1]"));

    source.SetAttribute("OffTime",
        StringValue(
            "ns3::ConstantRandomVariable[Constant=0]"));

    // Install three senders
    for (uint32_t i = 0; i < 3; i++)
    {
        ApplicationContainer app =
            source.Install(nodes.Get(i));

        app.Start(Seconds(1.0 + i * 0.2));
        app.Stop(Seconds(9.0 + i * 0.2));
    }

    // Enable FlowMonitor
    FlowMonitorHelper flowHelper;

    Ptr<FlowMonitor> monitor =
        flowHelper.InstallAll();

    Simulator::Stop(Seconds(20.0));

    Simulator::Run();

    monitor->CheckForLostPackets();

    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(
            flowHelper.GetClassifier());

    auto stats = monitor->GetFlowStats();

    std::cout << std::fixed << std::setprecision(3);

    for (const auto& flow : stats)
    {
        auto tuple =
            classifier->FindFlow(flow.first);

        if (tuple.destinationPort != port)
        {
            continue;
        }

        uint64_t txPackets = flow.second.txPackets;
        uint64_t rxPackets = flow.second.rxPackets;

        uint64_t missingPackets =
            txPackets >= rxPackets
                ? txPackets - rxPackets
                : 0;

        double lossPercentage = 0.0;

        if (txPackets > 0)
        {
            lossPercentage =
                (double)missingPackets / txPackets * 100.0;
        }

        double throughput = 0.0;

        if (rxPackets > 0)
        {
            throughput =
                flow.second.rxBytes * 8.0 /
                (8.0 * 1000000.0);
        }

        double avgDelay = 0.0;

        if (rxPackets > 0)
        {
            avgDelay =
                flow.second.delaySum.GetSeconds() /
                rxPackets * 1000.0;
        }

        std::cout << "\nSeed: " << seed
                  << " | Sender: " << tuple.sourceAddress
                  << "\n";

        std::cout << "Packets Sent: " << txPackets << "\n";
        std::cout << "Packets Received: " << rxPackets << "\n";
        std::cout << "Packets Missing: " << missingPackets << "\n";
        std::cout << "Packet Loss: " << lossPercentage << " %\n";
        std::cout << "Throughput: " << throughput << " Mbps\n";
        std::cout << "Average Delay: " << avgDelay << " ms\n";

        // Save experiment results
        csv << seed << ","
            << rateMbps << ","
            << tuple.sourceAddress << ","
            << txPackets << ","
            << rxPackets << ","
            << missingPackets << ","
            << lossPercentage << ","
            << throughput << ","
            << avgDelay << "\n";
    }

    Simulator::Destroy();
}

int main(int argc, char* argv[])
{
    std::ofstream csv("wireless-results.csv");

    if (!csv.is_open())
    {
        std::cerr << "Could not create CSV file!\n";
        return 1;
    }

    // CSV headings
    csv << "Seed,Rate_Mbps,Source_IP,Tx_Packets,"
        << "Rx_Packets,Missing_Packets,"
        << "Loss_Percentage,Throughput_Mbps,"
        << "Average_Delay_ms\n";

    uint32_t rates[] =
        {1, 2, 4, 6, 8, 10, 12, 15, 20, 25, 30};

    uint32_t seeds[] =
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    uint32_t totalRuns = 0;

    for (uint32_t seed : seeds)
    {
        for (uint32_t rate : rates)
        {
            RunScenario(rate, seed, csv);
            totalRuns++;
        }
    }

    csv.close();

    std::cout << "\n====================================\n";
    std::cout << "ALL EXPERIMENTS COMPLETED!\n";
    std::cout << "Total simulation runs: " << totalRuns << "\n";
    std::cout << "Expected CSV records: " << totalRuns * 3 << "\n";
    std::cout << "Results saved to wireless-results.csv\n";
    std::cout << "====================================\n";

    return 0;
}
