/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Lab2 sender-congestion test:
 * one sender sends 10MB messages at line rate to three staggered receivers.
 * This isolates SIRD sender feedback (sird.csn) and receiver credit rebalancing.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
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

NS_LOG_COMPONENT_DEFINE ("HomaL4ProtocolLab2SenderCongestion");

static Ptr<OutputStreamWrapper> g_creditEventStream;
static std::vector<uint32_t> g_receiverNodeIds;

struct SwitchEgressQueueTarget
{
  std::string label;
  Ptr<QueueDisc> queueDisc;
};

static void
AppReceive (Ptr<Socket> receiverSocket)
{
  Address from;
  while (receiverSocket->RecvFrom (from))
    {
      // Drain receive queue.
    }
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

static uint32_t
ExtractNodeId (const std::string& context)
{
  std::size_t pos = context.find ("/NodeList/");
  if (pos == std::string::npos)
    {
      return std::numeric_limits<uint32_t>::max ();
    }

  pos += 10;
  std::size_t end = context.find ('/', pos);
  if (end == std::string::npos)
    {
      end = context.size ();
    }
  return static_cast<uint32_t> (std::stoul (context.substr (pos, end - pos)));
}

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
  *stream->GetStream () << "- " << Simulator::Now ().GetNanoSeconds ()
                        << " " << msg->GetSize ()
                        << " " << saddr << ":" << sport
                        << " " << daddr << ":" << dport
                        << " " << txMsgId << std::endl;
}

static void
TraceSirdCreditDecision (Ptr<OutputStreamWrapper> stream,
                         Ipv4Address sender,
                         uint16_t txMsgId,
                         uint16_t grantOffset,
                         double senderBudgetPkts,
                         double ecnEwma,
                         bool senderCsn)
{
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " sender=" << sender
                        << " txMsgId=" << txMsgId
                        << " grantOffset=" << grantOffset
                        << " senderBudgetPkts=" << senderBudgetPkts
                        << " ecnEwma=" << ecnEwma
                        << " senderCsn=" << (senderCsn ? 1 : 0)
                        << std::endl;
}

static void
TraceSirdBucketState (Ptr<OutputStreamWrapper> stream,
                      Ipv4Address receiver,
                      Ipv4Address sender,
                      double senderBudgetHostPkts,
                      uint32_t senderInUsePkts,
                      uint32_t globalInUsePkts,
                      uint32_t globalBudgetPkts,
                      uint8_t eventType)
{
  uint32_t senderBudgetPkts = static_cast<uint32_t> (std::max (1.0, senderBudgetHostPkts));
  uint32_t senderAvailPkts =
    (senderBudgetPkts > senderInUsePkts) ? (senderBudgetPkts - senderInUsePkts) : 0;
  uint32_t globalAvailPkts =
    (globalBudgetPkts > globalInUsePkts) ? (globalBudgetPkts - globalInUsePkts) : 0;

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " receiver=" << receiver
                        << " sender=" << sender
                        << " eventType=" << static_cast<uint32_t> (eventType)
                        << " senderBudgetHostPkts=" << senderBudgetHostPkts
                        << " senderInUsePkts=" << senderInUsePkts
                        << " senderAvailPkts=" << senderAvailPkts
                        << " globalInUsePkts=" << globalInUsePkts
                        << " globalBudgetPkts=" << globalBudgetPkts
                        << " globalAvailPkts=" << globalAvailPkts
                        << std::endl;
}

static void
TraceSirdSenderCreditState (Ptr<OutputStreamWrapper> stream,
                            Ipv4Address sender,
                            Ipv4Address receiver,
                            uint16_t txMsgId,
                            uint32_t senderCreditPkts,
                            uint8_t eventType)
{
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " sender=" << sender
                        << " receiver=" << receiver
                        << " txMsgId=" << txMsgId
                        << " senderCreditPkts=" << senderCreditPkts
                        << " eventType=" << static_cast<uint32_t> (eventType)
                        << std::endl;
}

static void
TraceSirdReceiverCreditState (Ptr<OutputStreamWrapper> stream,
                              Ipv4Address receiver,
                              Ipv4Address sender,
                              uint32_t receiverAvailPkts,
                              uint32_t receiverBudgetPkts,
                              uint32_t senderAvailPkts,
                              uint32_t senderBudgetPkts,
                              uint8_t eventType)
{
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " receiver=" << receiver
                        << " sender=" << sender
                        << " receiverAvailPkts=" << receiverAvailPkts
                        << " receiverBudgetPkts=" << receiverBudgetPkts
                        << " senderAvailPkts=" << senderAvailPkts
                        << " senderBudgetPkts=" << senderBudgetPkts
                        << " eventType=" << static_cast<uint32_t> (eventType)
                        << std::endl;
}

static void
TraceSirdCreditEvent (std::string context,
                      Ipv4Address sender,
                      uint16_t txMsgId,
                      uint16_t grantOffset,
                      double senderBudgetPkts,
                      double ecnEwma,
                      bool senderCsn)
{
  uint32_t recvNode = ExtractNodeId (context);
  if (g_creditEventStream == 0)
    {
      return;
    }

  *g_creditEventStream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                                     << " type=grant"
                                     << " recvNode=" << recvNode
                                     << " sender=" << sender
                                     << " txMsgId=" << txMsgId
                                     << " grantOffset=" << grantOffset
                                     << " senderBudgetPkts=" << senderBudgetPkts
                                     << " ecnEwma=" << ecnEwma
                                     << " senderCsn=" << (senderCsn ? 1 : 0)
                                     << std::endl;
}

static void
TraceDataArrivalCreditEvent (std::string context,
                             Ptr<const Packet> packet,
                             Ipv4Address saddr,
                             Ipv4Address daddr,
                             uint16_t sport,
                             uint16_t dport,
                             int txMsgId,
                             uint16_t pktOffset,
                             uint8_t prio)
{
  uint32_t recvNode = ExtractNodeId (context);
  if (g_creditEventStream == 0)
    {
      return;
    }

  *g_creditEventStream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                                     << " type=data"
                                     << " recvNode=" << recvNode
                                     << " sender=" << saddr
                                     << " receiver=" << daddr
                                     << " txMsgId=" << txMsgId
                                     << " pktOffset=" << pktOffset
                                     << " prio=" << static_cast<uint32_t> (prio)
                                     << " size=" << packet->GetSize ()
                                     << std::endl;
}

static void
TraceSwitchEgressQueueSample (Ptr<OutputStreamWrapper> stream,
                              const std::vector<SwitchEgressQueueTarget>* targets,
                              Time stopTime,
                              Time sampleInterval)
{
  for (const auto& target : *targets)
    {
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " queue=" << target.label
                            << " packets=" << target.queueDisc->GetNPackets ()
                            << " bytes=" << target.queueDisc->GetNBytes ()
                            << std::endl;
    }

  if (Simulator::Now () + sampleInterval <= stopTime)
    {
      Simulator::Schedule (sampleInterval,
                           &TraceSwitchEgressQueueSample,
                           stream,
                           targets,
                           stopTime,
                           sampleInterval);
    }
}

int
main (int argc, char* argv[])
{
  std::string simTag = "default";
  std::string outputDir = "outputs/sird-scenarios/HomaL4Protocol-lab2-sender-congestion";

  double startSec = 0.2;
  double durationSec = 13.2;
  double settleTailSec = 0.1;
  uint64_t flowGapUs = 4500000;
  uint64_t sendIntervalUs = 0;
  uint32_t activeReceiverCount = 3;
  uint32_t msgSizeBytes = 10000000;

  bool enableSird = true;
  bool traceMsg = false;
  bool traceProtocolCredit = true;
  bool traceSirdCredit = false;
  bool traceSirdBucket = false;
  bool traceCreditEvents = false;
  bool traceSwitchEgressQueue = false;
  uint64_t switchQueueSampleUs = 1000;

  double bdpPkts = 33.32;
  uint8_t numTotalPrioBands = 8;
  uint8_t numUnschedPrioBands = 2;

  double sirdEcnMdFactor = 0.85;
  double sirdEcnAiStep = 1.0;
  double sirdSenderMdFactor = 0.8;
  double sirdSenderAiStep = 1.0;
  double sirdEcnAlphaGain = 0.125;
  uint16_t sirdSenderCsnThresholdPkts = 0;

  std::string deviceQueueMaxSize = "17p";
  std::string qdiscMaxSize = "1000p";

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simTag", "Suffix for output trace files", simTag);
  cmd.AddValue ("outputDir", "Directory for output trace files", outputDir);
  cmd.AddValue ("startSec", "Start time of traffic generation", startSec);
  cmd.AddValue ("durationSec", "Traffic generation duration in seconds", durationSec);
  cmd.AddValue ("settleTailSec", "Tail time after traffic generation for draining in-flight packets", settleTailSec);
  cmd.AddValue ("flowGapUs", "Start gap between receiver flows in microseconds", flowGapUs);
  cmd.AddValue ("sendIntervalUs", "Message send interval in microseconds; 0 means line-rate interval from msgSizeBytes", sendIntervalUs);
  cmd.AddValue ("activeReceiverCount", "Number of receiver flows to start from the lab2 receiver set", activeReceiverCount);
  cmd.AddValue ("msgSizeBytes", "Sender-to-receiver message size", msgSizeBytes);
  cmd.AddValue ("enableSird", "Enable SIRD control path", enableSird);
  cmd.AddValue ("traceMsg", "Whether to trace message begin/finish events", traceMsg);
  cmd.AddValue ("traceProtocolCredit", "Whether to trace protocol-level sender/receiver credit state", traceProtocolCredit);
  cmd.AddValue ("traceSirdCredit", "Whether to trace SIRD credit/GRANT decisions", traceSirdCredit);
  cmd.AddValue ("traceSirdBucket", "Whether to trace SIRD bucket states", traceSirdBucket);
  cmd.AddValue ("traceCreditEvents", "Whether to trace legacy grant/data event reconstruction", traceCreditEvents);
  cmd.AddValue ("traceSwitchEgressQueue", "Whether to sample switch egress queue occupancy", traceSwitchEgressQueue);
  cmd.AddValue ("switchQueueSampleUs", "Switch egress queue sampling interval in microseconds", switchQueueSampleUs);
  cmd.AddValue ("bdpPkts", "RTT BDP in packets; all SIRD/Homa thresholds are derived from it", bdpPkts);
  cmd.AddValue ("sirdEcnMdFactor", "SIRD ECN multiplicative decrease factor", sirdEcnMdFactor);
  cmd.AddValue ("sirdEcnAiStep", "SIRD ECN additive increase step", sirdEcnAiStep);
  cmd.AddValue ("sirdSenderMdFactor", "SIRD sender-feedback multiplicative decrease factor", sirdSenderMdFactor);
  cmd.AddValue ("sirdSenderAiStep", "SIRD sender-feedback additive increase step", sirdSenderAiStep);
  cmd.AddValue ("sirdEcnAlphaGain", "SIRD ECN EWMA gain", sirdEcnAlphaGain);
  cmd.AddValue ("sirdSenderCsnThresholdPkts", "Optional override for SIRD sender CSN threshold in packets; 0 means derive from bdpPkts", sirdSenderCsnThresholdPkts);
  cmd.AddValue ("deviceQueueMaxSize", "PointToPointNetDevice TxQueue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "SirdQueueDisc MaxSize", qdiscMaxSize);
  cmd.Parse (argc, argv);

  auto roundPackets = [] (double value) -> uint16_t {
    return static_cast<uint16_t> (std::max<long> (1, std::lround (value)));
  };
  uint32_t homaBdpPkts = roundPackets (bdpPkts);
  uint16_t sirdCreditBudgetPkts = roundPackets (1.5 * bdpPkts);
  uint16_t sirdUnschThresholdPkts = roundPackets (1.0 * bdpPkts);
  uint16_t derivedSirdSenderCsnThresholdPkts = roundPackets (0.5 * bdpPkts);
  if (sirdSenderCsnThresholdPkts == 0)
    {
      sirdSenderCsnThresholdPkts = derivedSirdSenderCsnThresholdPkts;
    }
  std::ostringstream qdiscMarkThresholdBuilder;
  qdiscMarkThresholdBuilder << roundPackets (1.25 * bdpPkts) << "p";
  std::string qdiscMarkThreshold = qdiscMarkThresholdBuilder.str ();

  Time::SetResolution (Time::NS);
  SeedManager::SetRun (1);

  Config::SetDefault ("ns3::Ipv4GlobalRouting::EcmpMode", EnumValue (Ipv4GlobalRouting::ECMP_RANDOM));
  Config::SetDefault ("ns3::HomaL4Protocol::RttPackets", UintegerValue (homaBdpPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::NumTotalPrioBands", UintegerValue (numTotalPrioBands));
  Config::SetDefault ("ns3::HomaL4Protocol::NumUnschedPrioBands", UintegerValue (numUnschedPrioBands));
  Config::SetDefault ("ns3::HomaL4Protocol::UseSrrScheduling", BooleanValue (false));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEnabled", BooleanValue (enableSird));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdCreditBudgetPkts", UintegerValue (sirdCreditBudgetPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdUnschThresholdPkts", UintegerValue (sirdUnschThresholdPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnMdFactor", DoubleValue (sirdEcnMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAiStep", DoubleValue (sirdEcnAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderMdFactor", DoubleValue (sirdSenderMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderAiStep", DoubleValue (sirdSenderAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAlphaGain", DoubleValue (sirdEcnAlphaGain));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCsnThreshold", UintegerValue (sirdSenderCsnThresholdPkts));
  const uint32_t senderIdx = 0;
  const std::vector<uint32_t> receiverIdx = {1, 2, 3};
  const uint32_t nHosts = 4;

  NodeContainer hosts;
  hosts.Create (nHosts);
  NodeContainer sw;
  sw.Create (1);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("100Gbps"));
  p2p.SetChannelAttribute ("Delay", StringValue ("1us"));
  p2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  std::vector<NetDeviceContainer> links;
  std::vector<QueueDiscContainer> linkQdiscs;
  links.reserve (nHosts);
  linkQdiscs.reserve (nHosts);
  for (uint32_t i = 0; i < nHosts; ++i)
    {
      links.push_back (p2p.Install (hosts.Get (i), sw.Get (0)));
    }

  InternetStackHelper stack;
  stack.Install (hosts);
  stack.Install (sw);

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::SirdQueueDisc",
                        "MaxSize", StringValue (qdiscMaxSize),
                        "MarkThreshold", StringValue (qdiscMarkThreshold),
                        "UseEcn", BooleanValue (true));
  for (uint32_t i = 0; i < nHosts; ++i)
    {
      linkQdiscs.push_back (tch.Install (links[i]));
    }

  Ipv4AddressHelper address;
  address.SetBase ("10.2.0.0", "255.255.255.0");
  std::vector<Ipv4Address> hostIps;
  hostIps.reserve (nHosts);
  for (uint32_t i = 0; i < nHosts; ++i)
    {
      Ipv4InterfaceContainer ifs = address.Assign (links[i]);
      hostIps.push_back (ifs.GetAddress (0));
      address.NewNetwork ();
    }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  AsciiTraceHelper ascii;
  std::string mkdirCmd = "mkdir -p " + outputDir;
  std::system (mkdirCmd.c_str ());
  std::ostringstream prefix;
  prefix << outputDir << "/lab2_" << simTag;

  if (traceMsg)
    {
      Ptr<OutputStreamWrapper> msgStream = ascii.CreateFileStream (prefix.str () + ".msg.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgBegin",
                                     MakeBoundCallback (&TraceMsgBegin, msgStream));
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgFinish",
                                     MakeBoundCallback (&TraceMsgFinish, msgStream));
    }

  if (enableSird && traceSirdCredit)
    {
      Ptr<OutputStreamWrapper> creditStream = ascii.CreateFileStream (prefix.str () + ".sird-credit.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdGrantDecision",
                                     MakeBoundCallback (&TraceSirdCreditDecision, creditStream));
    }

  if (enableSird && traceSirdBucket)
    {
      Ptr<OutputStreamWrapper> bucketStream = ascii.CreateFileStream (prefix.str () + ".sird-bucket.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdBucketState",
                                     MakeBoundCallback (&TraceSirdBucketState, bucketStream));
    }

  if (enableSird && traceProtocolCredit)
    {
      Ptr<OutputStreamWrapper> senderCreditStream = ascii.CreateFileStream (prefix.str () + ".sender-credit.tr");
      Ptr<OutputStreamWrapper> receiverCreditStream = ascii.CreateFileStream (prefix.str () + ".receiver-credit.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdSenderCreditState",
                                     MakeBoundCallback (&TraceSirdSenderCreditState, senderCreditStream));
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdReceiverCreditState",
                                     MakeBoundCallback (&TraceSirdReceiverCreditState, receiverCreditStream));
    }

  if (enableSird && traceCreditEvents)
    {
      g_creditEventStream = ascii.CreateFileStream (prefix.str () + ".credit-events.tr");
    }

  if (enableSird && traceCreditEvents)
    {
      Config::Connect ("/NodeList/*/$ns3::HomaL4Protocol/SirdGrantDecision",
                       MakeCallback (&TraceSirdCreditEvent));
      Config::Connect ("/NodeList/*/$ns3::HomaL4Protocol/DataPktArrival",
                       MakeCallback (&TraceDataArrivalCreditEvent));
    }

  std::vector<SwitchEgressQueueTarget> switchEgressQueueTargets;
  switchEgressQueueTargets.reserve (nHosts);
  for (uint32_t i = 0; i < nHosts; ++i)
    {
      std::ostringstream switchQueueLabel;
      if (i == senderIdx)
        {
          switchQueueLabel << "switch_port_to_sender";
        }
      else
        {
          switchQueueLabel << "switch_port_to_receiver" << i;
        }
      switchEgressQueueTargets.push_back ({switchQueueLabel.str (), linkQdiscs[i].Get (1)});
    }

  activeReceiverCount = std::min<uint32_t> (activeReceiverCount, receiverIdx.size ());
  std::vector<InetSocketAddress> receiverAddrs;
  for (uint32_t i = 0; i < activeReceiverCount; ++i)
    {
      uint32_t ridx = receiverIdx[i];
      g_receiverNodeIds.push_back (ridx);
      Ptr<SocketFactory> rFactory = hosts.Get (ridx)->GetObject<HomaSocketFactory> ();
      Ptr<Socket> receiverSock = rFactory->CreateSocket ();
      InetSocketAddress addr (hostIps[ridx], static_cast<uint16_t> (31000 + i));
      receiverSock->Bind (addr);
      receiverSock->SetRecvCallback (MakeCallback (&AppReceive));
      receiverAddrs.push_back (addr);
    }

  Time startTime = Seconds (startSec);
  Time stopTime = Seconds (startSec + durationSec);
  Time simStopTime = Seconds (startSec + durationSec + settleTailSec);
  Time msgInterval = sendIntervalUs == 0 ?
    Seconds ((static_cast<double> (msgSizeBytes) * 8.0) / 100e9) :
    MicroSeconds (sendIntervalUs);
  Time flowGap = MicroSeconds (flowGapUs);

  if (traceSwitchEgressQueue)
    {
      Ptr<OutputStreamWrapper> switchQueueStream =
        ascii.CreateFileStream (prefix.str () + ".switch-egress-queue.tr");
      Simulator::Schedule (startTime,
                           &TraceSwitchEgressQueueSample,
                           switchQueueStream,
                           &switchEgressQueueTargets,
                           simStopTime,
                           MicroSeconds (switchQueueSampleUs));
    }

  for (uint32_t i = 0; i < receiverAddrs.size (); ++i)
    {
      Ptr<SocketFactory> sFactory = hosts.Get (senderIdx)->GetObject<HomaSocketFactory> ();
      Ptr<Socket> senderSock = sFactory->CreateSocket ();
      senderSock->Bind (InetSocketAddress (hostIps[senderIdx], static_cast<uint16_t> (22000 + i)));
      Simulator::Schedule (startTime + i * flowGap,
                           &SendPeriodic,
                           senderSock,
                           receiverAddrs[i],
                           msgSizeBytes,
                           msgInterval,
                           stopTime);
    }

  Simulator::Stop (simStopTime);
  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}
