/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Simulation 1 scaffold:
 * - 144 hosts
 * - 9 ToR switches, 16 hosts per ToR
 * - 4 Spine switches
 * - 100Gbps host-ToR
 * - 400Gbps ToR-Spine by default, or 200Gbps in core-overload mode
 *
 * Workloads are read from a file under inputs/.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/homa-header.h"
#include "ns3/internet-apps-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("HomaL4ProtocolSim1LargeLeafSpine");

struct WorkloadSpec
{
  double avgMsgSizePkts = 0.0;
  std::map<double, int> msgSizeCdf;
};

static uint64_t g_completedBytes = 0;

struct QueueSampleTarget
{
  std::string label;
  Ptr<QueueDisc> queueDisc;
};

static std::vector<QueueSampleTarget> g_torEgressQueueTargets;

static void
TraceMsgBegin (Ptr<OutputStreamWrapper> stream,
               Ptr<const Packet> msg,
               Ipv4Address saddr,
               Ipv4Address daddr,
               uint16_t sport,
               uint16_t dport,
               int txMsgId)
{
  *stream->GetStream () << "+ " << Simulator::Now ().GetNanoSeconds ()
                        << " " << msg->GetSize ()
                        << " " << saddr << ":" << sport
                        << " " << daddr << ":" << dport
                        << " " << txMsgId << std::endl;
}

static void
TraceMsgFinish (Ptr<OutputStreamWrapper> stream,
                Ptr<const Packet> msg,
                Ipv4Address saddr,
                Ipv4Address daddr,
                uint16_t sport,
                uint16_t dport,
                int txMsgId)
{
  g_completedBytes += msg->GetSize ();
  *stream->GetStream () << "- " << Simulator::Now ().GetNanoSeconds ()
                        << " " << msg->GetSize ()
                        << " " << saddr << ":" << sport
                        << " " << daddr << ":" << dport
                        << " " << txMsgId << std::endl;
}

static void
SampleAggregateGoodput (Ptr<OutputStreamWrapper> stream,
                        Time sampleInterval,
                        uint64_t* lastBytes)
{
  uint64_t deltaBytes = g_completedBytes - *lastBytes;
  *lastBytes = g_completedBytes;

  double goodputGbps = (deltaBytes * 8.0) / sampleInterval.GetSeconds () / 1e9;
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " goodputGbps=" << goodputGbps
                        << " completedBytes=" << g_completedBytes
                        << std::endl;

  Simulator::Schedule (sampleInterval,
                       &SampleAggregateGoodput,
                       stream,
                       sampleInterval,
                       lastBytes);
}

static void
SampleTorQueues (Ptr<OutputStreamWrapper> stream, Time sampleInterval)
{
  uint64_t totalPackets = 0;
  uint64_t maxPackets = 0;

  for (const auto& target : g_torEgressQueueTargets)
    {
      uint64_t packets = target.queueDisc->GetNPackets ();
      uint64_t bytes = target.queueDisc->GetNBytes ();
      totalPackets += packets;
      maxPackets = std::max (maxPackets, packets);

      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " queue=" << target.label
                            << " packets=" << packets
                            << " bytes=" << bytes
                            << std::endl;
    }

  double meanPackets = 0.0;
  if (!g_torEgressQueueTargets.empty ())
    {
      meanPackets = static_cast<double> (totalPackets) / g_torEgressQueueTargets.size ();
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " queue=aggregate"
                        << " maxPackets=" << maxPackets
                        << " meanPackets=" << meanPackets
                        << " numQueues=" << g_torEgressQueueTargets.size ()
                        << std::endl;

  Simulator::Schedule (sampleInterval,
                       &SampleTorQueues,
                       stream,
                       sampleInterval);
}

static void
SendPeriodic (Ptr<Socket> socket,
              InetSocketAddress dst,
              uint32_t msgSizeBytes,
              Time interval,
              Time stopTime)
{
  if (Simulator::Now () >= stopTime)
    {
      return;
    }

  socket->SendTo (Create<Packet> (msgSizeBytes), 0, dst);
  Simulator::Schedule (interval,
                       &SendPeriodic,
                       socket,
                       dst,
                       msgSizeBytes,
                       interval,
                       stopTime);
}

static WorkloadSpec
ReadWorkloadFile (const std::string& path)
{
  std::ifstream in (path.c_str ());
  if (!in.is_open ())
    {
      throw std::runtime_error ("Could not open workload file: " + path);
    }

  WorkloadSpec spec;
  if (!(in >> spec.avgMsgSizePkts))
    {
      throw std::runtime_error ("Invalid workload header in: " + path);
    }

  int msgSizePkts = 0;
  double cumulativeProbability = 0.0;
  while (in >> msgSizePkts >> cumulativeProbability)
    {
      spec.msgSizeCdf[cumulativeProbability] = msgSizePkts;
    }

  if (spec.msgSizeCdf.empty ())
    {
      throw std::runtime_error ("No CDF entries found in workload file: " + path);
    }

  return spec;
}

static std::string
CanonicalPaperRawWorkload (const std::string& workload)
{
  if (workload == "google_rpc")
    {
      return "GOOGLE_ALL_RPC";
    }
  if (workload == "facebook_hadoop")
    {
      return "FACEBOOK_HADOOP_ALL";
    }
  if (workload == "web_search")
    {
      return "DCTCP";
    }
  return workload;
}

static WorkloadSpec
ReadPaperRawWorkload (const std::string& path,
                      const std::string& rawWorkload,
                      const std::string& transportType,
                      double loadFactor,
                      uint32_t bytesPerPkt)
{
  std::ifstream in (path.c_str ());
  if (!in.is_open ())
    {
      throw std::runtime_error ("Could not open paper raw data file: " + path);
    }
  if (rawWorkload.empty ())
    {
      throw std::runtime_error ("paperRawWorkload must be set when reading paper raw data.");
    }
  if (bytesPerPkt == 0)
    {
      throw std::runtime_error ("paperRawBytesPerPkt must be positive.");
    }

  const std::string targetWorkload = CanonicalPaperRawWorkload (rawWorkload);
  std::map<int, double> pctByMsgSizePkts;
  double totalPct = 0.0;
  std::string line;
  std::getline (in, line); // header
  while (std::getline (in, line))
    {
      if (line.empty ())
        {
          continue;
        }

      std::istringstream row (line);
      std::string rowTransport;
      double rowLoadFactor = 0.0;
      std::string rowWorkload;
      uint64_t msgSizeBytes = 0;
      double sizeCntPercent = 0.0;
      double bytesPercent = 0.0;
      uint64_t unschedBytes = 0;
      if (!(row >> rowTransport >> rowLoadFactor >> rowWorkload >> msgSizeBytes >>
            sizeCntPercent >> bytesPercent >> unschedBytes))
        {
          continue;
        }

      if (rowTransport != transportType || rowWorkload != targetWorkload ||
          std::fabs (rowLoadFactor - loadFactor) > 1e-9)
        {
          continue;
        }

      uint64_t msgSizePkts = (msgSizeBytes + bytesPerPkt - 1) / bytesPerPkt;
      msgSizePkts = std::max<uint64_t> (1, msgSizePkts);
      msgSizePkts = std::min<uint64_t> (0xffff, msgSizePkts);
      pctByMsgSizePkts[static_cast<int> (msgSizePkts)] += sizeCntPercent;
      totalPct += sizeCntPercent;
    }

  if (pctByMsgSizePkts.empty ())
    {
      std::ostringstream error;
      error << "No rows found in " << path
            << " for transport=" << transportType
            << " loadFactor=" << loadFactor
            << " workload=" << targetWorkload;
      throw std::runtime_error (error.str ());
    }
  if (totalPct <= 0.0)
    {
      throw std::runtime_error ("Paper raw workload has non-positive total probability.");
    }

  WorkloadSpec spec;
  double cumulativeProbability = 0.0;
  for (const auto& kv : pctByMsgSizePkts)
    {
      const int msgSizePkts = kv.first;
      const double probability = kv.second / totalPct;
      spec.avgMsgSizePkts += msgSizePkts * probability;
      cumulativeProbability += probability;
      spec.msgSizeCdf[std::min (1.0, cumulativeProbability)] = msgSizePkts;
    }

  spec.msgSizeCdf[1.0] = pctByMsgSizePkts.rbegin ()->first;
  return spec;
}

static std::string
ResolveWorkloadPath (const std::string& workloadName, const std::string& workloadFile)
{
  if (!workloadFile.empty ())
    {
      return workloadFile;
    }

  if (workloadName.empty ())
    {
      throw std::runtime_error ("You must set either workloadFile or workloadName.");
    }

  return "inputs/" + workloadName + ".txt";
}

int
main (int argc, char* argv[])
{
  std::string simTag = "default";
  std::string outputDir = "outputs/sird-scenarios/HomaL4Protocol-sim1-large-leaf-spine";
  std::string trafficConfig = "balanced";
  std::string workloadName = "";
  std::string workloadFile = "";
  std::string paperRawFile = "inputs/homa-paper-reproduction/original-raw-data-from-paper.txt";
  std::string paperRawWorkload = "";
  std::string paperRawTransport = "Homa";
  double paperRawLoadFactor = -1.0;
  uint32_t paperRawBytesPerPkt = 1472;

  bool enableSird = true;
  double offeredLoad = 0.5;
  double startSec = 0.2;
  double durationSec = 1.0;
  double settleTailSec = 0.2;

  uint32_t nHosts = 144;
  uint32_t hostsPerTor = 16;
  uint32_t nTors = 9;
  uint32_t nSpines = 4;

  double hostTorRateGbps = 100.0;
  double torSpineRateGbps = 400.0;
  std::string hostTorDelay = "1us";
  std::string torSpineDelay = "1us";

  uint32_t appPort = 30000;
  double bdpPkts = 66.67;
  uint32_t homaBdpPktsOverride = 0;
  uint16_t sirdCreditBudgetPktsOverride = 0;
  uint16_t sirdUnschThresholdPktsOverride = 0;
  double sirdEcnMdFactor = 0.85;
  double sirdEcnAiStep = 1.0;
  double sirdSenderMdFactor = 0.8;
  double sirdSenderAiStep = 1.0;
  double sirdEcnAlphaGain = 0.125;
  uint16_t sirdSenderCsnThresholdPktsOverride = 0;

  std::string deviceQueueMaxSize = "17p";
  std::string qdiscMaxSize = "1000p";
  std::string qdiscMarkThreshold = "auto";

  bool traceMsg = false;
  bool traceTorQueue = false;
  bool traceGoodput = true;
  uint64_t queueSampleUs = 100;
  uint64_t goodputSampleUs = 100;

  uint32_t incastSenders = 30;
  uint32_t incastMsgBytes = 500000;
  double incastLoadFraction = 0.07;
  int32_t incastReceiverIdx = -1;
  uint32_t incastSeed = 1;

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simTag", "Suffix for output trace files", simTag);
  cmd.AddValue ("outputDir", "Directory for output trace files", outputDir);
  cmd.AddValue ("trafficConfig", "balanced, core, or incast", trafficConfig);
  cmd.AddValue ("workloadName", "Logical workload name; default path resolves to inputs/<name>.txt", workloadName);
  cmd.AddValue ("workloadFile", "Explicit workload file path", workloadFile);
  cmd.AddValue ("paperRawFile", "Paper raw data table path", paperRawFile);
  cmd.AddValue ("paperRawWorkload", "Paper raw workload alias/name: google_rpc, facebook_hadoop, web_search, or raw WorkLoad value", paperRawWorkload);
  cmd.AddValue ("paperRawTransport", "TransportType filter for paper raw data", paperRawTransport);
  cmd.AddValue ("paperRawLoadFactor", "LoadFactor filter for paper raw data; negative derives from offeredLoad", paperRawLoadFactor);
  cmd.AddValue ("paperRawBytesPerPkt", "Bytes per packet used to convert raw MsgSizeRange bytes to packet counts", paperRawBytesPerPkt);
  cmd.AddValue ("enableSird", "Enable SIRD control path", enableSird);
  cmd.AddValue ("offeredLoad", "Per-host offered load fraction for background all-to-all traffic", offeredLoad);
  cmd.AddValue ("startSec", "Background traffic start time", startSec);
  cmd.AddValue ("durationSec", "Traffic generation duration", durationSec);
  cmd.AddValue ("settleTailSec", "Drain time after traffic stop", settleTailSec);
  cmd.AddValue ("torSpineRateGbps", "Override ToR-Spine rate in Gbps; <=0 uses trafficConfig default", torSpineRateGbps);
  cmd.AddValue ("traceMsg", "Trace message begin/finish", traceMsg);
  cmd.AddValue ("traceTorQueue", "Sample ToR egress queues over time", traceTorQueue);
  cmd.AddValue ("traceGoodput", "Sample aggregate application-level goodput", traceGoodput);
  cmd.AddValue ("queueSampleUs", "ToR queue sampling period in microseconds", queueSampleUs);
  cmd.AddValue ("goodputSampleUs", "Goodput sampling period in microseconds", goodputSampleUs);
  cmd.AddValue ("incastSenders", "Number of burst senders in incast mode", incastSenders);
  cmd.AddValue ("incastMsgBytes", "Per-message size for incast overlay", incastMsgBytes);
  cmd.AddValue ("incastLoadFraction", "Aggregate incast overlay load fraction relative to total background offered load", incastLoadFraction);
  cmd.AddValue ("incastReceiverIdx", "Receiver host index for incast; negative chooses random receiver", incastReceiverIdx);
  cmd.AddValue ("incastSeed", "Seed for deterministic incast sender/receiver selection", incastSeed);
  cmd.AddValue ("bdpPkts", "RTT BDP in packets; default 66.67 for cross-ToR leaf-spine paths", bdpPkts);
  cmd.AddValue ("rttPkts", "Optional override for HomaL4Protocol::RttPackets; 0 derives from bdpPkts", homaBdpPktsOverride);
  cmd.AddValue ("sirdCreditBudgetPkts", "Optional override for SIRD global credit budget; 0 derives 1.5*bdpPkts", sirdCreditBudgetPktsOverride);
  cmd.AddValue ("sirdUnschThresholdPkts", "Optional override for SIRD unscheduled threshold; 0 derives 1.0*bdpPkts", sirdUnschThresholdPktsOverride);
  cmd.AddValue ("sirdEcnMdFactor", "SIRD ECN multiplicative decrease factor", sirdEcnMdFactor);
  cmd.AddValue ("sirdEcnAiStep", "SIRD ECN additive increase step", sirdEcnAiStep);
  cmd.AddValue ("sirdSenderMdFactor", "SIRD sender-feedback multiplicative decrease factor", sirdSenderMdFactor);
  cmd.AddValue ("sirdSenderAiStep", "SIRD sender-feedback additive increase step", sirdSenderAiStep);
  cmd.AddValue ("sirdEcnAlphaGain", "SIRD ECN EWMA gain", sirdEcnAlphaGain);
  cmd.AddValue ("sirdSenderCsnThresholdPkts", "Optional override for SIRD sender CSN threshold; 0 derives 0.5*bdpPkts", sirdSenderCsnThresholdPktsOverride);
  cmd.AddValue ("deviceQueueMaxSize", "PointToPointNetDevice TxQueue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "SirdQueueDisc MaxSize", qdiscMaxSize);
  cmd.AddValue ("qdiscMarkThreshold", "SirdQueueDisc ECN mark threshold", qdiscMarkThreshold);
  cmd.Parse (argc, argv);

  if (nTors * hostsPerTor != nHosts)
    {
      NS_FATAL_ERROR ("nTors * hostsPerTor must equal nHosts.");
    }
  if (offeredLoad <= 0.0 || offeredLoad > 1.0)
    {
      NS_FATAL_ERROR ("offeredLoad must be in (0, 1].");
    }
  if (incastLoadFraction < 0.0 || incastLoadFraction >= 1.0)
    {
      NS_FATAL_ERROR ("incastLoadFraction must be in [0, 1).");
    }
  if (bdpPkts <= 0.0)
    {
      NS_FATAL_ERROR ("bdpPkts must be positive.");
    }

  auto roundPackets = [] (double value) -> uint16_t {
    return static_cast<uint16_t> (std::max<long> (1, std::lround (value)));
  };
  uint32_t homaBdpPkts = homaBdpPktsOverride == 0 ? roundPackets (bdpPkts) : homaBdpPktsOverride;
  uint16_t sirdCreditBudgetPkts =
    sirdCreditBudgetPktsOverride == 0 ? roundPackets (1.5 * bdpPkts) : sirdCreditBudgetPktsOverride;
  uint16_t sirdUnschThresholdPkts =
    sirdUnschThresholdPktsOverride == 0 ? roundPackets (1.0 * bdpPkts) : sirdUnschThresholdPktsOverride;
  uint16_t sirdSenderCsnThresholdPkts =
    sirdSenderCsnThresholdPktsOverride == 0 ? roundPackets (0.5 * bdpPkts) : sirdSenderCsnThresholdPktsOverride;
  if (qdiscMarkThreshold == "auto")
    {
      std::ostringstream markThreshold;
      markThreshold << roundPackets (1.25 * bdpPkts) << "p";
      qdiscMarkThreshold = markThreshold.str ();
    }

  if (trafficConfig == "balanced")
    {
      if (torSpineRateGbps <= 0.0)
        {
          torSpineRateGbps = 400.0;
        }
    }
  else if (trafficConfig == "core")
    {
      if (torSpineRateGbps <= 0.0 || torSpineRateGbps == 400.0)
        {
          torSpineRateGbps = 200.0;
        }
    }
  else if (trafficConfig == "incast")
    {
      if (torSpineRateGbps <= 0.0)
        {
          torSpineRateGbps = 400.0;
        }
    }
  else
    {
      NS_FATAL_ERROR ("trafficConfig must be one of: balanced, core, incast.");
    }

  WorkloadSpec workload;
  try
    {
      if (!paperRawWorkload.empty ())
        {
          double rawLoadFactor = paperRawLoadFactor < 0.0 ? offeredLoad : paperRawLoadFactor;
          workload = ReadPaperRawWorkload (paperRawFile,
                                           paperRawWorkload,
                                           paperRawTransport,
                                           rawLoadFactor,
                                           paperRawBytesPerPkt);
        }
      else
        {
          workload = ReadWorkloadFile (ResolveWorkloadPath (workloadName, workloadFile));
        }
    }
  catch (const std::exception& ex)
    {
      NS_FATAL_ERROR (ex.what ());
    }

  Time::SetResolution (Time::NS);
  SeedManager::SetRun (1);

  Config::SetDefault ("ns3::Ipv4GlobalRouting::EcmpMode", EnumValue (Ipv4GlobalRouting::ECMP_RANDOM));
  Config::SetDefault ("ns3::HomaL4Protocol::RttPackets", UintegerValue (homaBdpPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::NumTotalPrioBands", UintegerValue (8));
  Config::SetDefault ("ns3::HomaL4Protocol::NumUnschedPrioBands", UintegerValue (2));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEnabled", BooleanValue (enableSird));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdCreditBudgetPkts", UintegerValue (sirdCreditBudgetPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdUnschThresholdPkts", UintegerValue (sirdUnschThresholdPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnMdFactor", DoubleValue (sirdEcnMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAiStep", DoubleValue (sirdEcnAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderMdFactor", DoubleValue (sirdSenderMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderAiStep", DoubleValue (sirdSenderAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAlphaGain", DoubleValue (sirdEcnAlphaGain));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCsnThreshold", UintegerValue (sirdSenderCsnThresholdPkts));

  NodeContainer hosts;
  hosts.Create (nHosts);
  NodeContainer tors;
  tors.Create (nTors);
  NodeContainer spines;
  spines.Create (nSpines);

  PointToPointHelper hostTor;
  hostTor.SetDeviceAttribute ("DataRate", StringValue ("100Gbps"));
  hostTor.SetChannelAttribute ("Delay", StringValue (hostTorDelay));
  hostTor.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  PointToPointHelper torSpine;
  {
    std::ostringstream rate;
    rate << torSpineRateGbps << "Gbps";
    torSpine.SetDeviceAttribute ("DataRate", StringValue (rate.str ()));
  }
  torSpine.SetChannelAttribute ("Delay", StringValue (torSpineDelay));
  torSpine.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::SirdQueueDisc",
                        "MaxSize", StringValue (qdiscMaxSize),
                        "MarkThreshold", StringValue (qdiscMarkThreshold),
                        "UseEcn", BooleanValue (true));

  std::vector<NetDeviceContainer> hostTorLinks;
  std::vector<QueueDiscContainer> hostTorQdiscs;
  hostTorLinks.reserve (nHosts);
  hostTorQdiscs.reserve (nHosts);
  for (uint32_t hostIdx = 0; hostIdx < nHosts; ++hostIdx)
    {
      uint32_t torIdx = hostIdx / hostsPerTor;
      NetDeviceContainer link = hostTor.Install (hosts.Get (hostIdx), tors.Get (torIdx));
      hostTorLinks.push_back (link);
    }

  std::vector<NetDeviceContainer> torSpineLinks;
  std::vector<QueueDiscContainer> torSpineQdiscs;
  torSpineLinks.reserve (nTors * nSpines);
  torSpineQdiscs.reserve (nTors * nSpines);
  for (uint32_t torIdx = 0; torIdx < nTors; ++torIdx)
    {
      for (uint32_t spineIdx = 0; spineIdx < nSpines; ++spineIdx)
        {
          NetDeviceContainer link = torSpine.Install (tors.Get (torIdx), spines.Get (spineIdx));
          torSpineLinks.push_back (link);
        }
    }

  InternetStackHelper stack;
  stack.Install (hosts);
  stack.Install (tors);
  stack.Install (spines);

  for (uint32_t hostIdx = 0; hostIdx < hostTorLinks.size (); ++hostIdx)
    {
      QueueDiscContainer qdiscs = tch.Install (hostTorLinks[hostIdx]);
      hostTorQdiscs.push_back (qdiscs);
      uint32_t torIdx = hostIdx / hostsPerTor;
      std::ostringstream label;
      label << "tor" << torIdx << "_to_host" << hostIdx;
      g_torEgressQueueTargets.push_back ({label.str (), qdiscs.Get (1)});
    }
  for (uint32_t linkIdx = 0; linkIdx < torSpineLinks.size (); ++linkIdx)
    {
      QueueDiscContainer qdiscs = tch.Install (torSpineLinks[linkIdx]);
      torSpineQdiscs.push_back (qdiscs);
      uint32_t torIdx = linkIdx / nSpines;
      uint32_t spineIdx = linkIdx % nSpines;
      std::ostringstream label;
      label << "tor" << torIdx << "_to_spine" << spineIdx;
      g_torEgressQueueTargets.push_back ({label.str (), qdiscs.Get (0)});
    }

  Ipv4AddressHelper address;
  address.SetBase ("10.10.0.0", "255.255.255.0");
  std::vector<Ipv4Address> hostIps;
  hostIps.resize (nHosts);

  for (uint32_t hostIdx = 0; hostIdx < nHosts; ++hostIdx)
    {
      Ipv4InterfaceContainer ifs = address.Assign (hostTorLinks[hostIdx]);
      hostIps[hostIdx] = ifs.GetAddress (0);
      address.NewNetwork ();
    }
  for (const auto& link : torSpineLinks)
    {
      address.Assign (link);
      address.NewNetwork ();
    }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  AsciiTraceHelper ascii;
  std::string mkdirCmd = "mkdir -p " + outputDir;
  std::system (mkdirCmd.c_str ());
  std::ostringstream prefix;
  prefix << outputDir << "/sim1_" << simTag;

  if (traceMsg)
    {
      Ptr<OutputStreamWrapper> msgStream = ascii.CreateFileStream (prefix.str () + ".msg.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgBegin",
                                     MakeBoundCallback (&TraceMsgBegin, msgStream));
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgFinish",
                                     MakeBoundCallback (&TraceMsgFinish, msgStream));
    }

  if (traceTorQueue)
    {
      Ptr<OutputStreamWrapper> queueStream = ascii.CreateFileStream (prefix.str () + ".tor-egress-queue.tr");
      Simulator::Schedule (MicroSeconds (queueSampleUs),
                           &SampleTorQueues,
                           queueStream,
                           MicroSeconds (queueSampleUs));
    }

  if (traceGoodput)
    {
      Ptr<OutputStreamWrapper> goodputStream = ascii.CreateFileStream (prefix.str () + ".goodput.tr");
      uint64_t* lastBytes = new uint64_t (0);
      Simulator::Schedule (MicroSeconds (goodputSampleUs),
                           &SampleAggregateGoodput,
                           goodputStream,
                           MicroSeconds (goodputSampleUs),
                           lastBytes);
    }

  std::vector<InetSocketAddress> remoteClients;
  remoteClients.reserve (nHosts);
  for (uint32_t hostIdx = 0; hostIdx < nHosts; ++hostIdx)
    {
      remoteClients.emplace_back (hostIps[hostIdx], appPort);
    }

  double backgroundOfferedLoad = offeredLoad;
  if (trafficConfig == "incast")
    {
      backgroundOfferedLoad = offeredLoad * (1.0 - incastLoadFraction);
    }

  std::vector<Ptr<MsgGeneratorApp>> backgroundApps;
  backgroundApps.reserve (nHosts);
  for (uint32_t hostIdx = 0; hostIdx < nHosts; ++hostIdx)
    {
      Ptr<MsgGeneratorApp> app = CreateObject<MsgGeneratorApp> (hostIps[hostIdx], appPort);
      app->Install (hosts.Get (hostIdx), remoteClients);
      app->SetWorkload (backgroundOfferedLoad, workload.msgSizeCdf, workload.avgMsgSizePkts);
      app->Start (Seconds (startSec));
      app->Stop (Seconds (startSec + durationSec));
      backgroundApps.push_back (app);
    }

  if (trafficConfig == "incast")
    {
      std::vector<uint32_t> hostIndices (nHosts);
      std::iota (hostIndices.begin (), hostIndices.end (), 0);
      std::mt19937 rng (incastSeed);

      uint32_t receiver = 0;
      if (incastReceiverIdx >= 0)
        {
          receiver = static_cast<uint32_t> (incastReceiverIdx);
        }
      else
        {
          std::uniform_int_distribution<uint32_t> receiverDist (0, nHosts - 1);
          receiver = receiverDist (rng);
        }

      hostIndices.erase (std::remove (hostIndices.begin (), hostIndices.end (), receiver), hostIndices.end ());
      std::shuffle (hostIndices.begin (), hostIndices.end (), rng);

      if (incastSenders > hostIndices.size ())
        {
          NS_FATAL_ERROR ("incastSenders exceeds available non-receiver hosts.");
        }

      double aggregateIncastGbps = incastLoadFraction * offeredLoad * nHosts * hostTorRateGbps;
      double perSenderGbps = aggregateIncastGbps / incastSenders;
      if (perSenderGbps <= 0.0)
        {
          NS_FATAL_ERROR ("incastLoadFraction leads to non-positive per-sender rate.");
        }

      Time incastInterval = Seconds ((static_cast<double> (incastMsgBytes) * 8.0) /
                                     (perSenderGbps * 1e9));
      Time stopTime = Seconds (startSec + durationSec);

      for (uint32_t i = 0; i < incastSenders; ++i)
        {
          uint32_t senderIdx = hostIndices[i];
          Ptr<SocketFactory> sFactory = hosts.Get (senderIdx)->GetObject<HomaSocketFactory> ();
          Ptr<Socket> senderSock = sFactory->CreateSocket ();
          senderSock->Bind (InetSocketAddress (hostIps[senderIdx], static_cast<uint16_t> (40000 + i)));
          Simulator::Schedule (Seconds (startSec),
                               &SendPeriodic,
                               senderSock,
                               InetSocketAddress (hostIps[receiver], appPort),
                               incastMsgBytes,
                               incastInterval,
                               stopTime);
        }
    }

  Simulator::Stop (Seconds (startSec + durationSec + settleTailSec));
  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}
