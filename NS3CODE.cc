#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/point-to-point-module.h"

using namespace ns3;

int main(int argc, char *argv[])
{
    uint32_t nNodes = 200;
    bool enableScaling = false;
    bool enableQos = false;
    double simTime = 10.0;
    std::string baseRate = "40kbps"; // Used when QoS OFF

    CommandLine cmd;
    cmd.AddValue("nNodes", "Number of IoT nodes", nNodes);
    cmd.AddValue("enableScaling", "Enable Multi-AP Scaling", enableScaling);
    cmd.AddValue("enableQos", "Enable Differentiated Traffic Intensity QoS", enableQos);
    cmd.Parse(argc, argv);

    uint16_t hiPort = 9000;
    uint16_t loPort = 9001;

    NodeContainer edgeNode;
    edgeNode.Create(1);

    NodeContainer apNodes;
    apNodes.Create(enableScaling ? 2 : 1);

    NodeContainer iotNodes;
    iotNodes.Create(nNodes);

    InternetStackHelper stack;
    stack.InstallAll();

    // ---------------- WIFI SETUP ----------------

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211n);

    WifiMacHelper mac;
    YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default();

    Ptr<YansWifiChannel> channel1 = channelHelper.Create();
    Ptr<YansWifiChannel> channel2 = channelHelper.Create();

    YansWifiPhyHelper phy1, phy2;
    phy1.SetChannel(channel1);
    phy2.SetChannel(channel2);

    NetDeviceContainer staDevs1, staDevs2, apDev1, apDev2;

    uint32_t half = enableScaling ? nNodes/2 : nNodes;

    // AP1
    Ssid ssid1 = Ssid("AP1");
    mac.SetType("ns3::StaWifiMac",
                "Ssid", SsidValue(ssid1),
                "ActiveProbing", BooleanValue(false));

    for(uint32_t i=0; i<half; i++)
        staDevs1.Add(wifi.Install(phy1, mac, iotNodes.Get(i)));

    mac.SetType("ns3::ApWifiMac",
                "Ssid", SsidValue(ssid1));

    apDev1 = wifi.Install(phy1, mac, apNodes.Get(0));

    // AP2 (Scaling)
    if(enableScaling)
    {
        Ssid ssid2 = Ssid("AP2");

        mac.SetType("ns3::StaWifiMac",
                    "Ssid", SsidValue(ssid2),
                    "ActiveProbing", BooleanValue(false));

        for(uint32_t i=half; i<nNodes; i++)
            staDevs2.Add(wifi.Install(phy2, mac, iotNodes.Get(i)));

        mac.SetType("ns3::ApWifiMac",
                    "Ssid", SsidValue(ssid2));

        apDev2 = wifi.Install(phy2, mac, apNodes.Get(1));
    }

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.InstallAll();

    // ---------------- IP ADDRESSING ----------------

    Ipv4AddressHelper address;

    address.SetBase("10.1.0.0","255.255.0.0");
    address.Assign(staDevs1);
    address.Assign(apDev1);

    if(enableScaling)
    {
        address.SetBase("10.2.0.0","255.255.0.0");
        address.Assign(staDevs2);
        address.Assign(apDev2);
    }

    // ---------------- BACKHAUL ----------------

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("2ms"));

    NetDeviceContainer link1 = p2p.Install(apNodes.Get(0), edgeNode.Get(0));
    address.SetBase("10.10.1.0","255.255.255.0");
    Ipv4InterfaceContainer edgeIf1 = address.Assign(link1);

    Ipv4InterfaceContainer edgeIf2;
    if(enableScaling)
    {
        NetDeviceContainer link2 = p2p.Install(apNodes.Get(1), edgeNode.Get(0));
        address.SetBase("10.10.2.0","255.255.255.0");
        edgeIf2 = address.Assign(link2);
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---------------- APPLICATIONS ----------------

    UdpServerHelper hiServer(hiPort);
    UdpServerHelper loServer(loPort);

    hiServer.Install(edgeNode.Get(0)).Start(Seconds(0.0));
    loServer.Install(edgeNode.Get(0)).Start(Seconds(0.0));

    for(uint32_t i=0; i<nNodes; i++)
    {
        uint16_t port = (i < nNodes/5) ? hiPort : loPort;

        Ipv4Address dest =
            (enableScaling && i >= half) ?
            edgeIf2.GetAddress(1) :
            edgeIf1.GetAddress(1);

        OnOffHelper client("ns3::UdpSocketFactory",
                           InetSocketAddress(dest, port));

        // Differentiated Traffic Intensity QoS
        if(enableQos)
        {
            if(port == hiPort)
                client.SetAttribute("DataRate", StringValue("20kbps")); // Protected
            else
                client.SetAttribute("DataRate", StringValue("60kbps")); // Best Effort
        }
        else
        {
            client.SetAttribute("DataRate", StringValue(baseRate));
        }

        client.SetAttribute("PacketSize", UintegerValue(256));
        client.SetAttribute("StartTime", TimeValue(Seconds(1.0 + i*0.005)));
        client.SetAttribute("StopTime", TimeValue(Seconds(simTime)));
        client.Install(iotNodes.Get(i));
    }

    // ---------------- FLOW MONITOR ----------------

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    Simulator::Stop(Seconds(simTime+1));
    Simulator::Run();

    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());

    uint64_t hiRx=0, hiTx=0, loRx=0, loTx=0;
    double hiDelay=0, loDelay=0, totalRxBytes=0;

    for(auto &flow : stats)
    {
        auto t = classifier->FindFlow(flow.first);
        totalRxBytes += flow.second.rxBytes;

        if(t.destinationPort == hiPort)
        {
            hiRx += flow.second.rxPackets;
            hiTx += flow.second.txPackets;
            hiDelay += flow.second.delaySum.GetSeconds();
        }
        else
        {
            loRx += flow.second.rxPackets;
            loTx += flow.second.txPackets;
            loDelay += flow.second.delaySum.GetSeconds();
        }
    }

    double throughput = totalRxBytes * 8.0 / simTime / 1000;

    std::cout<<"\n========== PERFORMANCE MATRIX ==========\n";
    std::cout<<"Nodes: "<<nNodes
             <<" | Scaling: "<<enableScaling
             <<" | QoS: "<<enableQos<<"\n";
    std::cout<<"----------------------------------------\n";
    std::cout<<"HIGH PRIORITY:\n";
    std::cout<<"  PDR: "<<(hiTx? (double)hiRx/hiTx*100:0)<<" %\n";
    std::cout<<"  Latency: "<<(hiRx? hiDelay/hiRx*1000:0)<<" ms\n";
    std::cout<<"LOW PRIORITY:\n";
    std::cout<<"  PDR: "<<(loTx? (double)loRx/loTx*100:0)<<" %\n";
    std::cout<<"  Latency: "<<(loRx? loDelay/loRx*1000:0)<<" ms\n";
    std::cout<<"OVERALL:\n";
    std::cout<<"  Total PDR: "<<((hiTx+loTx)? (double)(hiRx+loRx)/(hiTx+loTx)*100:0)<<" %\n";
    std::cout<<"  Throughput: "<<throughput<<" Kbps\n";
    std::cout<<"========================================\n";

    Simulator::Destroy();
    return 0;
}