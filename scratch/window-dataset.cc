#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

#include <fstream>
#include <iostream>
#include <map>

using namespace ns3;

Ptr<FlowMonitor> g_monitor;
Ptr<Ipv4FlowClassifier> g_classifier;
std::map<uint32_t, FlowMonitor::FlowStats> previousStats;
std::ofstream* g_csv;

void CollectWindow(uint32_t seed, uint32_t rate, uint32_t second)
{
    auto currentStats = g_monitor->GetFlowStats();

    for (auto it = currentStats.begin(); it != currentStats.end(); ++it)
    {
        uint32_t flowId = it->first;
        auto current = it->second;

        Ipv4FlowClassifier::FiveTuple tuple =
            g_classifier->FindFlow(flowId);

        if (tuple.destinationPort < 5000 ||
            tuple.destinationPort > 5002)
            continue;

        uint32_t senderId = tuple.destinationPort - 4999;

        uint64_t oldTx = 0;
        uint64_t oldRx = 0;
        Time oldDelay = Seconds(0);

        if (previousStats.count(flowId))
        {
            oldTx = previousStats[flowId].txPackets;
            oldRx = previousStats[flowId].rxPackets;
            oldDelay = previousStats[flowId].delaySum;
        }

        uint64_t tx = current.txPackets - oldTx;
        uint64_t rx = current.rxPackets - oldRx;
        uint64_t rxBytes = current.rxBytes -
            (previousStats.count(flowId)
                 ? previousStats[flowId].rxBytes : 0);

        Time delay = current.delaySum - oldDelay;

        double throughput = rxBytes * 8.0 / 1000000.0;

        double loss = 0.0;
        if (tx > 0 && tx > rx)
            loss = (tx - rx) * 100.0 / tx;

        double avgDelay = 0.0;
        if (rx > 0)
            avgDelay = delay.GetSeconds() * 1000.0 / rx;

        *g_csv << seed << ","
               << rate << ","
               << second << ","
               << senderId << ","
               << throughput << ","
               << loss << ","
               << avgDelay << "\n";

        previousStats[flowId] = current;
    }
}

void RunScenario(uint32_t rate, uint32_t seed, std::ofstream& csv)
{
    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(seed);

    previousStats.clear();
    g_csv = &csv;

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

    NetDeviceContainer devices = wifi.Install(phy, mac, nodes);

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> positions =
        CreateObject<ListPositionAllocator>();

    positions->Add(Vector(0, 0, 0));
    positions->Add(Vector(5, 0, 0));
    positions->Add(Vector(0, 5, 0));
    positions->Add(Vector(5, 5, 0));

    mobility.SetPositionAllocator(positions);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    InternetStackHelper internet;
    internet.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");

    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    for (uint32_t i = 0; i < 3; i++)
    {
        uint16_t port = 5000 + i;

        PacketSinkHelper sink(
            "ns3::UdpSocketFactory",
            InetSocketAddress(Ipv4Address::GetAny(), port));

        ApplicationContainer sinkApp = sink.Install(nodes.Get(3));
        sinkApp.Start(Seconds(0));
        sinkApp.Stop(Seconds(11.5));

        OnOffHelper source(
            "ns3::UdpSocketFactory",
            InetSocketAddress(interfaces.GetAddress(3), port));

        source.SetAttribute("DataRate",
            DataRateValue(DataRate(rate * 1000000)));

        source.SetAttribute("PacketSize", UintegerValue(1024));
        source.SetAttribute("OnTime",
            StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        source.SetAttribute("OffTime",
            StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        ApplicationContainer app = source.Install(nodes.Get(i));

        app.Start(Seconds(1.0 + i * 0.2));
        app.Stop(Seconds(11.0 + i * 0.2));
    }

    FlowMonitorHelper flowHelper;
    g_monitor = flowHelper.InstallAll();

    g_classifier =
        DynamicCast<Ipv4FlowClassifier>(
            flowHelper.GetClassifier());

    for (uint32_t second = 3; second <= 10; second++)
    {
        Simulator::Schedule(
            Seconds(second),
            &CollectWindow,
            seed,
            rate,
            second);
    }

    Simulator::Stop(Seconds(11.5));
    Simulator::Run();

    Simulator::Destroy();
}

int main(int argc, char* argv[])
{
    std::ofstream csv("window-results.csv");

    csv << "Seed,Rate_Mbps,Time,Sender_ID,"
        << "Throughput_Mbps,Loss_Percentage,Average_Delay_ms\n";

    uint32_t rates[] = {1, 2, 4, 6, 8, 10, 12, 15, 20, 25, 30};

    for (uint32_t seed = 1; seed <= 10; seed++)
    {
        for (uint32_t rate : rates)
        {
            std::cout << "Running seed " << seed
                      << " at " << rate << " Mbps\n";

            RunScenario(rate, seed, csv);
        }
    }

    csv.close();

    std::cout << "\nWINDOW DATASET GENERATED: window-results.csv\n";

    return 0;
}
