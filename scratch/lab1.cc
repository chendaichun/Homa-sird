/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Lab1 receiver-congestion test:
 * 6 background senders send 10MB messages to one receiver, while one probe
 * sender periodically sends short messages to the same receiver.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/homa-header.h"
#include "ns3/internet-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("HomaL4ProtocolLab1ReceiverCongestion");

static std::unordered_map<std::string, uint64_t> g_linkRxBytesTotal;
static std::unordered_map<std::string, uint64_t> g_linkRxBytesLastSample;
static bool g_progressBarCompleted = false;
static const double kLinkThroughputSampleSec = 0.001;

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
TracePathRtt (Ptr<OutputStreamWrapper> stream,
              Ipv4Address sender,
              Ipv4Address receiver,
              uint16_t sport,
              uint16_t dport,
              uint16_t txMsgId,
              uint8_t triggerKind,
              uint8_t ctrlFlags,
              Time pathRtt)
{
  const char* basis = (triggerKind == 0) ? "initialCreditRequest" : "firstData";

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " sender=" << sender
                        << " receiver=" << receiver
                        << " sport=" << sport
                        << " dport=" << dport
                        << " txMsgId=" << txMsgId
                        << " ctrl=" << HomaHeader::FlagsToString (ctrlFlags)
                        << " basis=" << basis
                        << " rttNs=" << pathRtt.GetNanoSeconds ()
                        << std::endl;
}

static void
TraceLinkRxBytes (const std::string& linkLabel, Ptr<const Packet> packet)
{
  g_linkRxBytesTotal[linkLabel] += packet->GetSize ();
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
TraceSirdLoopState (Ptr<OutputStreamWrapper> stream,
                    Ipv4Address receiver,
                    Ipv4Address sender,
                    double netBudgetPkts,
                    double hostBudgetPkts,
                    double effectiveBudgetPkts,
                    double ceEwma,
                    uint64_t loopState,
                    uint64_t counterState)
{
  bool senderCe = (loopState & 0x1u) != 0;
  bool senderCsn = (loopState & 0x2u) != 0;
  uint32_t eventType = static_cast<uint32_t> ((loopState >> 2) & 0x3u);
  uint32_t senderCreditsInUsePkts = static_cast<uint32_t> ((loopState >> 8) & 0xFFFFu);
  uint32_t globalCreditsInUsePkts = static_cast<uint32_t> ((loopState >> 24) & 0xFFFFu);
  uint32_t globalBudgetPkts = static_cast<uint32_t> ((loopState >> 40) & 0xFFFFu);
  uint64_t ceMarksObserved = counterState & 0x1FFFFFu;
  uint64_t csnMarksObserved = (counterState >> 21) & 0x1FFFFFu;
  uint64_t dataPktsObserved = (counterState >> 42) & 0x1FFFFFu;

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " receiver=" << receiver
                        << " sender=" << sender
                        << " netBudgetPkts=" << netBudgetPkts
                        << " hostBudgetPkts=" << hostBudgetPkts
                        << " effectiveBudgetPkts=" << effectiveBudgetPkts
                        << " senderCe=" << (senderCe ? 1 : 0)
                        << " senderCsn=" << (senderCsn ? 1 : 0)
                        << " ceEwma=" << ceEwma
                        << " senderCreditsInUsePkts=" << senderCreditsInUsePkts
                        << " globalCreditsInUsePkts=" << globalCreditsInUsePkts
                        << " globalBudgetPkts=" << globalBudgetPkts
                        << " ceMarksObserved=" << ceMarksObserved
                        << " csnMarksObserved=" << csnMarksObserved
                        << " dataPktsObserved=" << dataPktsObserved
                        << " eventType=" << eventType
                        << std::endl;
}

static void
TraceLinkThroughput (Ptr<OutputStreamWrapper> stream, Time sampleInterval)
{
  for (const auto& kv : g_linkRxBytesTotal)
    {
      const std::string& linkLabel = kv.first;
      uint64_t totalBytes = kv.second;
      uint64_t lastBytes = g_linkRxBytesLastSample[linkLabel];
      uint64_t deltaBytes = totalBytes - lastBytes;
      g_linkRxBytesLastSample[linkLabel] = totalBytes;

      double instGbps = (deltaBytes * 8.0) / sampleInterval.GetSeconds () / 1e9;
      double avgGbps = (totalBytes * 8.0) / std::max (1e-12, Simulator::Now ().GetSeconds ()) / 1e9;

      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " link=" << linkLabel
                            << " instGbps=" << instGbps
                            << " avgGbps=" << avgGbps
                            << " totalBytes=" << totalBytes
                            << std::endl;
    }

  Simulator::Schedule (sampleInterval,
                       &TraceLinkThroughput,
                       stream,
                       sampleInterval);
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

static void
TraceSimulationProgress (Time stopTime, Time progressInterval)
{
  const double stopSec = std::max (1e-12, stopTime.GetSeconds ());
  const double nowSec = Simulator::Now ().GetSeconds ();
  const double ratio = std::min (1.0, nowSec / stopSec);
  const uint32_t barWidth = 40;
  const uint32_t filled = static_cast<uint32_t> (ratio * barWidth);

  std::cout << "\r[";
  for (uint32_t i = 0; i < barWidth; ++i)
    {
      std::cout << (i < filled ? "=" : " ");
    }
  std::cout << "] "
            << std::fixed << std::setprecision (1) << (ratio * 100.0)
            << "% (" << std::setprecision (4) << nowSec << "s/" << stopSec << "s)"
            << std::flush;

  if (ratio >= 1.0)
    {
      g_progressBarCompleted = true;
      std::cout << std::endl;
      return;
    }

  Simulator::Schedule (progressInterval,
                       &TraceSimulationProgress,
                       stopTime,
                       progressInterval);
}

static void
SendPeriodic (Ptr<Socket> socket,
              InetSocketAddress dst,
              uint32_t msgSizeBytes,
              Time interval,
              Time stopTime,
              uint32_t remainingMessages)
{
  if (Simulator::Now () >= stopTime || remainingMessages == 0)
    {
      return;
    }

  socket->SendTo (Create<Packet> (msgSizeBytes), 0, dst);
  uint32_t nextRemainingMessages = remainingMessages == 0xffffffffu ? 0xffffffffu : remainingMessages - 1;
  Simulator::Schedule (interval,
                       &SendPeriodic,
                       socket,
                       dst,
                       msgSizeBytes,
                       interval,
                       stopTime,
                       nextRemainingMessages);
}

int
main (int argc, char* argv[])
{
  // 输出 trace 文件名后缀，用来区分 Homa/SIRD 以及 8B/500KB 短流实验。
  std::string simTag = "default";

  // 输出 trace 文件目录。
  std::string outputDir = "outputs/sird-scenarios/HomaL4Protocol-lab1-receiver-congestion";

  // 主实验开关：true 启用 SIRD 接收端 credit 控制；false 使用同拓扑同流量下的 Homa baseline。
  bool enableSird = true;

  // 业务流开始注入的仿真时刻，预留时间给路由和协议初始化。
  double startSec = 0.2;

  // 业务流持续时间：6 个长流发送端和 1 个短流 probe 发送端都只在这个窗口内生成消息。
  double durationSec = 0.001;

  // 发流停止后的排空时间，用来等待在途包和消息完成事件。
  double settleTailSec = 0.001;

  // 6 个 background 长流发送端的单条消息大小；默认 10MB。
  uint32_t longMsgSizeBytes = 10000000;

  // 每个长流发送端的 offered rate；6 个发送端各 17Gbps 会略微超过接收端 100Gbps 出口。
  double longSenderRateGbps = 17.0;

  // 短流 probe 的单条消息大小；8 表示 8B 短消息，500000 表示 500KB 短消息。
  uint32_t shortMsgSizeBytes = 8;

  // 短流 probe 发送周期；值越小，短流注入压力越高。
  uint64_t shortIntervalUs = 200;

  // Probe 最多发送多少条消息；0 表示只按 durationSec 控制，不限制条数。
  uint32_t targetProbeMessages = 0;

  // 是否注入 6 个 10MB 背景长流；关闭后可生成 unloaded probe baseline。
  bool enableBackgroundTraffic = true;

  // 是否使用 SRR/FIFO-like receiver scheduling；false 表示默认 SRPT。
  bool useSrrScheduling = false;

  // 是否记录消息开始/完成事件；FCT 分析依赖这个 trace。
  bool traceMsg = true;

  // 是否记录由 Homa 内部收发事件还原的 path RTT；不额外注入 ICMP 探针包。
  bool tracePathRtt = true;

  // 是否按固定周期采样交换机侧每个出口 SirdQueueDisc 的当前排队长度。
  bool traceSwitchEgressQueue = true;

  // 交换机出口队列采样周期，单位微秒。
  uint64_t switchQueueSampleUs = 1;

  // 是否记录每条链路的接收吞吐，采样周期由 kLinkThroughputSampleSec 控制。
  bool traceLinkThroughput = true;

  // 是否记录 SIRD credit/GRANT 决策。
  bool traceSirdCredit = true;

  // 是否记录每个 sender 的 SIRD CE/CSN 控制环状态。
  bool traceSirdLoop = false;

  // 是否在终端显示仿真进度条。
  bool showProgressBar = true;

  // 进度条刷新周期，单位毫秒。
  double progressIntervalMs = 1.0;

  // RTT BDP，单位为包。100Gbps、约 2us 单向路径、约 1500B/packet 时单向约 16.66pkts，
  // 往返约 33.32pkts；Homa/SIRD 的 BDP 派生阈值统一使用这个 RTT BDP。
  double bdpPkts = 33.32;

  // Homa 可用的总优先级队列数量。
  uint8_t numTotalPrioBands = 8;

  // 其中分配给 unscheduled 数据的高优先级队列数量。
  uint8_t numUnschedPrioBands = 2;

  // ECN 拥塞反馈触发时，接收端网络侧 budget 的乘性减小系数。
  double sirdEcnMdFactor = 0.85;

  // 没有 ECN 拥塞反馈时，接收端网络侧 budget 的加性恢复步长。
  double sirdEcnAiStep = 1.0;

  // sender 通过 CSN 反馈拥塞时，host 侧 budget 的乘性减小系数。
  double sirdSenderMdFactor = 0.8;

  // 没有 CSN 反馈时，host 侧 budget 的加性恢复步长。
  double sirdSenderAiStep = 1.0;

  // ECN CE 比例的 EWMA 平滑系数。
  double sirdEcnAlphaGain = 0.125;

  // PointToPointNetDevice 的发送队列上限；这是每端口设备队列，不是交换机共享 buffer。
  std::string deviceQueueMaxSize = "17p";

  // SirdQueueDisc 队列上限；保持较浅，避免过深队列掩盖排队延迟。
  std::string qdiscMaxSize = "1000p";

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simTag", "Suffix for output trace files", simTag);
  cmd.AddValue ("outputDir", "Directory for output trace files", outputDir);
  cmd.AddValue ("enableSird", "Enable SIRD control path", enableSird);
  cmd.AddValue ("startSec", "Start time of traffic generation", startSec);
  cmd.AddValue ("durationSec", "Traffic generation duration in seconds", durationSec);
  cmd.AddValue ("settleTailSec", "Tail time after traffic generation for draining in-flight packets", settleTailSec);
  cmd.AddValue ("longMsgSizeBytes", "Background long-flow message size", longMsgSizeBytes);
  cmd.AddValue ("longSenderRateGbps", "Per-long-sender offered rate", longSenderRateGbps);
  cmd.AddValue ("shortMsgSizeBytes", "Probe short-flow message size, e.g. 8 or 500000", shortMsgSizeBytes);
  cmd.AddValue ("shortIntervalUs", "Probe send interval in microseconds", shortIntervalUs);
  cmd.AddValue ("targetProbeMessages", "Maximum number of probe messages to send; 0 means unlimited within durationSec", targetProbeMessages);
  cmd.AddValue ("enableBackgroundTraffic", "Whether to generate the 6 long background senders", enableBackgroundTraffic);
  cmd.AddValue ("useSrrScheduling", "Use FIFO/SRR-like receiver scheduling instead of SRPT", useSrrScheduling);
  cmd.AddValue ("traceMsg", "Whether to trace message begin/finish events", traceMsg);
  cmd.AddValue ("tracePathRtt", "Whether to trace Homa-derived path RTT", tracePathRtt);
  cmd.AddValue ("traceSwitchEgressQueue", "Whether to sample switch egress queue occupancy", traceSwitchEgressQueue);
  cmd.AddValue ("switchQueueSampleUs", "Switch egress queue sampling interval in microseconds", switchQueueSampleUs);
  cmd.AddValue ("traceLinkThroughput", "Whether to trace per-link throughput over time", traceLinkThroughput);
  cmd.AddValue ("traceSirdCredit", "Whether to trace SIRD credit/GRANT decisions", traceSirdCredit);
  cmd.AddValue ("traceSirdLoop", "Whether to trace per-sender SIRD loop state", traceSirdLoop);
  cmd.AddValue ("showProgressBar", "Whether to display simulation progress bar in terminal", showProgressBar);
  cmd.AddValue ("progressIntervalMs", "Progress bar refresh interval in milliseconds", progressIntervalMs);
  cmd.AddValue ("bdpPkts", "RTT BDP in packets; all SIRD/Homa thresholds are derived from it", bdpPkts);
  cmd.AddValue ("sirdEcnMdFactor", "SIRD ECN multiplicative decrease factor", sirdEcnMdFactor);
  cmd.AddValue ("sirdEcnAiStep", "SIRD ECN additive increase step", sirdEcnAiStep);
  cmd.AddValue ("sirdSenderMdFactor", "SIRD sender-feedback multiplicative decrease factor", sirdSenderMdFactor);
  cmd.AddValue ("sirdSenderAiStep", "SIRD sender-feedback additive increase step", sirdSenderAiStep);
  cmd.AddValue ("sirdEcnAlphaGain", "SIRD ECN EWMA gain", sirdEcnAlphaGain);
  cmd.AddValue ("deviceQueueMaxSize", "PointToPointNetDevice TxQueue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "SirdQueueDisc MaxSize", qdiscMaxSize);
  cmd.Parse (argc, argv);

  auto roundPackets = [] (double value) -> uint16_t {
    return static_cast<uint16_t> (std::max<long> (1, std::lround (value)));
  };
  uint32_t homaBdpPkts = roundPackets (bdpPkts);
  uint16_t sirdCreditBudgetPkts = roundPackets (1.5 * bdpPkts);
  uint16_t sirdUnschThresholdPkts = roundPackets (1.0 * bdpPkts);
  uint16_t sirdSenderCsnThresholdPkts = roundPackets (0.5 * bdpPkts);
  std::ostringstream qdiscMarkThresholdBuilder;
  qdiscMarkThresholdBuilder << roundPackets (1.25 * bdpPkts) << "p";
  std::string qdiscMarkThreshold = qdiscMarkThresholdBuilder.str ();

  Time::SetResolution (Time::NS);
  SeedManager::SetRun (1);

  Config::SetDefault ("ns3::Ipv4GlobalRouting::EcmpMode", EnumValue (Ipv4GlobalRouting::ECMP_RANDOM));
  Config::SetDefault ("ns3::HomaL4Protocol::RttPackets", UintegerValue (homaBdpPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::NumTotalPrioBands", UintegerValue (numTotalPrioBands));
  Config::SetDefault ("ns3::HomaL4Protocol::NumUnschedPrioBands", UintegerValue (numUnschedPrioBands));
  Config::SetDefault ("ns3::HomaL4Protocol::UseSrrScheduling", BooleanValue (useSrrScheduling));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEnabled", BooleanValue (enableSird));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdCreditBudgetPkts", UintegerValue (sirdCreditBudgetPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdUnschThresholdPkts", UintegerValue (sirdUnschThresholdPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnMdFactor", DoubleValue (sirdEcnMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAiStep", DoubleValue (sirdEcnAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderMdFactor", DoubleValue (sirdSenderMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderAiStep", DoubleValue (sirdSenderAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAlphaGain", DoubleValue (sirdEcnAlphaGain));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCsnThreshold", UintegerValue (sirdSenderCsnThresholdPkts));

  const uint32_t nLongSenders = 6;
  const uint32_t probeSenderIdx = 6;
  const uint32_t receiverIdx = 7;
  const uint32_t nHosts = 8;

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
  address.SetBase ("10.1.0.0", "255.255.255.0");
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
  prefix << outputDir << "/lab1_" << simTag;

  if (traceMsg)
    {
      Ptr<OutputStreamWrapper> msgStream = ascii.CreateFileStream (prefix.str () + ".msg.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgBegin",
                                     MakeBoundCallback (&TraceMsgBegin, msgStream));
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgFinish",
                                     MakeBoundCallback (&TraceMsgFinish, msgStream));
    }

  if (tracePathRtt)
    {
      Ptr<OutputStreamWrapper> pathRttStream = ascii.CreateFileStream (prefix.str () + ".path-rtt.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/PathRtt",
                                     MakeBoundCallback (&TracePathRtt, pathRttStream));
    }

  if (enableSird && traceSirdCredit)
    {
      Ptr<OutputStreamWrapper> sirdCreditStream = ascii.CreateFileStream (prefix.str () + ".sird-credit.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdGrantDecision",
                                     MakeBoundCallback (&TraceSirdCreditDecision, sirdCreditStream));
    }

  if (enableSird && traceSirdLoop)
    {
      Ptr<OutputStreamWrapper> sirdLoopStream = ascii.CreateFileStream (prefix.str () + ".sird-loop.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdLoopState",
                                     MakeBoundCallback (&TraceSirdLoopState, sirdLoopStream));
    }

  std::vector<SwitchEgressQueueTarget> switchEgressQueueTargets;
  switchEgressQueueTargets.reserve (nHosts);
  for (uint32_t i = 0; i < nHosts; ++i)
    {
      std::ostringstream switchQueueLabel;
      std::ostringstream uplinkLabel;
      std::ostringstream downlinkLabel;
      if (i < nLongSenders)
        {
          switchQueueLabel << "switch_port_to_long_sender" << i;
          uplinkLabel << "long_sender" << i << "_to_switch";
          downlinkLabel << "switch_to_long_sender" << i;
        }
      else if (i == probeSenderIdx)
        {
          switchQueueLabel << "switch_port_to_probe_sender";
          uplinkLabel << "probe_sender_to_switch";
          downlinkLabel << "switch_to_probe_sender";
        }
      else
        {
          switchQueueLabel << "switch_port_to_receiver";
          uplinkLabel << "receiver_to_switch";
          downlinkLabel << "switch_to_receiver";
        }

      DynamicCast<PointToPointNetDevice> (links[i].Get (1))
        ->TraceConnectWithoutContext ("MacRx",
                                      MakeBoundCallback (&TraceLinkRxBytes, uplinkLabel.str ()));
      DynamicCast<PointToPointNetDevice> (links[i].Get (0))
        ->TraceConnectWithoutContext ("MacRx",
                                      MakeBoundCallback (&TraceLinkRxBytes, downlinkLabel.str ()));

      Ptr<QueueDisc> switchTxQueueDisc = linkQdiscs[i].Get (1);
      switchEgressQueueTargets.push_back ({switchQueueLabel.str (), switchTxQueueDisc});
    }

  Ptr<OutputStreamWrapper> linkThrStream;
  if (traceLinkThroughput)
    {
      linkThrStream = ascii.CreateFileStream (prefix.str () + ".link-throughput.tr");
      Simulator::Schedule (Seconds (startSec + kLinkThroughputSampleSec),
                           &TraceLinkThroughput,
                           linkThrStream,
                           Seconds (kLinkThroughputSampleSec));
    }

  Time startTime = Seconds (startSec);
  Time stopTime = Seconds (startSec + durationSec);
  Time simStopTime = Seconds (startSec + durationSec + settleTailSec);

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

  Ptr<SocketFactory> rFactory = hosts.Get (receiverIdx)->GetObject<HomaSocketFactory> ();
  Ptr<Socket> receiverSock = rFactory->CreateSocket ();
  InetSocketAddress receiverAddr (hostIps[receiverIdx], 30000);
  receiverSock->Bind (receiverAddr);
  receiverSock->SetRecvCallback (MakeCallback (&AppReceive));

  Time longInterval = Seconds ((static_cast<double> (longMsgSizeBytes) * 8.0) /
                               (longSenderRateGbps * 1e9));
  Time shortInterval = MicroSeconds (shortIntervalUs);

  if (enableBackgroundTraffic)
    {
      for (uint32_t i = 0; i < nLongSenders; ++i)
        {
          Ptr<SocketFactory> sFactory = hosts.Get (i)->GetObject<HomaSocketFactory> ();
          Ptr<Socket> senderSock = sFactory->CreateSocket ();
          senderSock->Bind (InetSocketAddress (hostIps[i], static_cast<uint16_t> (20000 + i)));
          Simulator::Schedule (startTime,
                               &SendPeriodic,
                               senderSock,
                               receiverAddr,
                               longMsgSizeBytes,
                               longInterval,
                               stopTime,
                               0xffffffffu);
        }
    }

  Ptr<SocketFactory> probeFactory = hosts.Get (probeSenderIdx)->GetObject<HomaSocketFactory> ();
  Ptr<Socket> probeSock = probeFactory->CreateSocket ();
  probeSock->Bind (InetSocketAddress (hostIps[probeSenderIdx], 21000));
  Simulator::Schedule (startTime,
                       &SendPeriodic,
                       probeSock,
                       receiverAddr,
                       shortMsgSizeBytes,
                       shortInterval,
                       stopTime,
                       targetProbeMessages == 0 ? 0xffffffffu : targetProbeMessages);

  Simulator::Stop (simStopTime);
  Time progressInterval = MilliSeconds (std::max (1.0, progressIntervalMs));
  if (showProgressBar)
    {
      g_progressBarCompleted = false;
      TraceSimulationProgress (simStopTime, progressInterval);
    }
  Simulator::Run ();
  if (showProgressBar && !g_progressBarCompleted)
    {
      TraceSimulationProgress (simStopTime, progressInterval);
    }
  Simulator::Destroy ();
  return 0;
}
