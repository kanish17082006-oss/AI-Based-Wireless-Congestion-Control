
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("AIAdaptiveCongestionControl");

struct SenderState
{
    Ptr<PacketSink> sink;
    Ptr<OnOffApplication> app;
    uint64_t lastRx;
    double rateMbps;
    uint32_t id;
};

std::vector<SenderState> senders;
std::ofstream results;

// Ask the trained Python model for a prediction
int PredictCongestion(double rate, double throughput)
{
    std::ostringstream command;

    command << ".venv/bin/python predict.py "
            << std::fixed << std::setprecision(4)
            << rate << " " << throughput;

    FILE* pipe = popen(command.str().c_str(), "r");

    if (pipe == nullptr)
    {
        std::cerr << "Could not execute Python predictor!\n";
        return -1;
    }

    char buffer[128];
    std::string output;

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        output += buffer;
    }

    int status = pclose(pipe);

    if (status != 0 || output.empty())
    {
        std::cerr << "Python prediction failed!\n";
        return -1;
    }

    try
    {
        int prediction = std::stoi(output);

        if (prediction != 0 && prediction != 1)
        {
            return -1;
        }

        return prediction;
    }
    catch (...)
    {
        return -1;
    }
}

// Run every second to measure and control network traffic
void ControlLoop()
{
    std::cout << "\n========== AI CONTROL: "
              << Simulator::Now().GetSeconds()
              << " seconds ==========\n";

    for (auto& sender : senders)
    {
        uint64_t currentRx = sender.sink->GetTotalRx();

        uint64_t receivedBytes =
            currentRx - sender.lastRx;

        // Throughput measured during the last 1-second window
        double throughput =
            receivedBytes * 8.0 / 1000000.0;

        sender.lastRx = currentRx;

        int prediction =
            PredictCongestion(sender.rateMbps, throughput);

        if (prediction == -1)
        {
            std::cerr << "Prediction failed for sender "
                      << sender.id
                      << ". Keeping its current rate.\n";

            results << Simulator::Now().GetSeconds() << ","
                    << sender.id << ","
                    << sender.rateMbps << ","
                    << throughput << ",ERROR\n";

            continue;
        }

        std::string status =
            prediction == 1 ? "CONGESTED" : "NORMAL";

        double oldRate = sender.rateMbps;

        // Congestion: multiplicative decrease
        if (prediction == 1)
        {
            sender.rateMbps =
                std::max(1.0, sender.rateMbps * 0.70);
        }
        // Normal: cautious additive increase
        else
        {
            sender.rateMbps =
                std::min(30.0, sender.rateMbps + 1.0);
        }

        // Update the sender's actual transmission rate
        uint64_t bitsPerSecond =
            static_cast<uint64_t>(
                sender.rateMbps * 1000000.0
            );

        sender.app->SetAttribute(
            "DataRate",
            DataRateValue(DataRate(bitsPerSecond))
        );

        std::cout << "Sender " << sender.id
                  << " | Throughput: " << throughput
                  << " Mbps"
                  << " | AI: " << status
                  << " | Rate: " << oldRate
                  << " -> " << sender.rateMbps
                  << " Mbps\n";

        results << Simulator::Now().GetSeconds() << ","
                << sender.id << ","
                << sender.rateMbps << ","
                << throughput << ","
                << status << "\n";
    }

    // Continue controlling until the experiment ends
    if (Simulator::Now() < Seconds(10.0))
    {
        Simulator::Schedule(
            Seconds(1.0),
            &ControlLoop
        );
    }
}

int main(int argc, char* argv[])
{
    results.open("ai-control-results.csv");

    if (!results.is_open())
    {
        std::cerr << "Cannot create results CSV!\n";
        return 1;
    }

    results << "Time,Sender_ID,Rate_Mbps,"
            << "Throughput_Mbps,AI_Prediction\n";

    RngSeedManager::SetSeed(12345);
    RngSeedManager::SetRun(1);

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

    // Set node positions
    MobilityHelper mobility;

    Ptr<ListPositionAllocator> positions =
        CreateObject<ListPositionAllocator>();

    positions->Add(Vector(0.0, 0.0, 0.0));
    positions->Add(Vector(5.0, 0.0, 0.0));
    positions->Add(Vector(0.0, 5.0, 0.0));
    positions->Add(Vector(5.0, 5.0, 0.0));

    mobility.SetPositionAllocator(positions);

    mobility.SetMobilityModel(
        "ns3::ConstantPositionMobilityModel"
    );

    mobility.Install(nodes);

    // Install Internet stack
    InternetStackHelper internet;
    internet.Install(nodes);

    // Assign IP addresses
    Ipv4AddressHelper address;

    address.SetBase(
        "10.1.1.0",
        "255.255.255.0"
    );

    Ipv4InterfaceContainer interfaces =
        address.Assign(devices);

    // Install one separate receiver port per sender
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

        sinkApps.Start(Seconds(0.0));
        sinkApps.Stop(Seconds(12.0));

        Ptr<PacketSink> sink =
            DynamicCast<PacketSink>(sinkApps.Get(0));

        // Configure sender
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

        double initialRate = 15.0;

        source.SetAttribute(
            "DataRate",
            DataRateValue(
                DataRate(15000000)
            )
        );

        ApplicationContainer sourceApps =
            source.Install(nodes.Get(i));

        sourceApps.Start(Seconds(1.0 + i * 0.2));
        sourceApps.Stop(Seconds(11.0 + i * 0.2));

        Ptr<OnOffApplication> app =
            DynamicCast<OnOffApplication>(
                sourceApps.Get(0)
            );

        SenderState state;

        state.sink = sink;
        state.app = app;
        state.lastRx = 0;
        state.rateMbps = initialRate;
        state.id = i + 1;

        senders.push_back(state);
    }

    // Start feedback control after traffic begins
    Simulator::Schedule(
        Seconds(2.0),
        &ControlLoop
    );

    Simulator::Stop(Seconds(14.0));

    std::cout << "\nStarting AI-based adaptive simulation...\n";

    Simulator::Run();

    Simulator::Destroy();

    results.close();

    std::cout << "\n====================================\n";
    std::cout << "AI CONTROL SIMULATION COMPLETED!\n";
    std::cout << "Results: ai-control-results.csv\n";
    std::cout << "====================================\n";

    return 0;
}
