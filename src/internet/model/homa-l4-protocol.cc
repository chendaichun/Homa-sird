/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2020 Stanford University
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Serhat Arslan <sarslan@stanford.edu>
 */

#include <algorithm>
#include <numeric>
#include <sstream>

#include "ns3/log.h"
#include "ns3/assert.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/object-vector.h"
#include "ns3/uinteger.h"

#include "ns3/point-to-point-net-device.h"
#include "ns3/ppp-header.h"
#include "ns3/ipv4-route.h"
#include "ipv4-end-point-demux.h"
#include "ipv4-end-point.h"
#include "ipv4-l3-protocol.h"
#include "homa-l4-protocol.h"
#include "homa-socket-factory.h"
#include "homa-socket.h"

namespace ns3 {

namespace {

enum PathRttTriggerKind : uint8_t
{
  PATH_RTT_INITIAL_CREDIT_REQUEST = 0,
  PATH_RTT_FIRST_DATA = 1
};

struct PathRttResponseTxState
{
  uint8_t flags;
  uint16_t grantOffset;
  uint16_t pktOffset;
  Time txTime;
};

struct PathRttFlowState
{
  bool haveTriggerTx = false;
  Time triggerTxTime;
  bool haveTriggerRx = false;
  Time triggerRxTime;
  uint8_t triggerKind = PATH_RTT_FIRST_DATA;
  bool sampleEmitted = false;
  std::vector<PathRttResponseTxState> responseTxStates;
};

static std::unordered_map<std::string, PathRttFlowState> g_pathRttFlowStates;

static std::string
MakePathRttKey (Ipv4Address sender,
                Ipv4Address receiver,
                uint16_t sport,
                uint16_t dport,
                uint16_t txMsgId)
{
  std::ostringstream key;
  key << sender << ":" << sport
      << " " << receiver << ":" << dport
      << " " << txMsgId;
  return key.str ();
}

static bool
IsPathRttTriggerPacket (const HomaHeader& homaHeader)
{
  return (homaHeader.GetFlags () & HomaHeader::Flags_t::DATA) != 0 &&
         homaHeader.GetPktOffset () == 0;
}

static uint8_t
GetPathRttTriggerKind (const HomaHeader& homaHeader)
{
  return (homaHeader.GetPayloadSize () == 0) ? PATH_RTT_INITIAL_CREDIT_REQUEST
                                             : PATH_RTT_FIRST_DATA;
}

static bool
IsPathRttResponsePacket (const HomaHeader& homaHeader)
{
  return (homaHeader.GetFlags () & HomaHeader::Flags_t::GRANT) != 0 ||
         (homaHeader.GetFlags () & HomaHeader::Flags_t::ACK) != 0;
}

} // namespace

NS_LOG_COMPONENT_DEFINE ("HomaL4Protocol");

NS_OBJECT_ENSURE_REGISTERED (HomaL4Protocol);

/* The protocol is not standardized yet. Using a temporary number */
const uint8_t HomaL4Protocol::PROT_NUMBER = 198;
    
TypeId 
// 注册并返回 HomaL4Protocol 的 TypeId，同时声明可配置属性与跟踪源。
HomaL4Protocol::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaL4Protocol")
    .SetParent<IpL4Protocol> ()
    .SetGroupName ("Internet")
    .AddConstructor<HomaL4Protocol> ()
    .AddAttribute ("SocketList", "The list of sockets associated to this protocol.",
                   ObjectVectorValue (),
                   MakeObjectVectorAccessor (&HomaL4Protocol::m_sockets),
                   MakeObjectVectorChecker<HomaSocket> ())
    .AddAttribute ("RttPackets", "RTT BDP in packets for Homa's in-flight window baseline.",
                   UintegerValue (10),
                   MakeUintegerAccessor (&HomaL4Protocol::m_bdp),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("NumTotalPrioBands", "Total number of priority levels used within the network",
                   UintegerValue (8),
                   MakeUintegerAccessor (&HomaL4Protocol::m_numTotalPrioBands),
                   MakeUintegerChecker<uint8_t> ())
    .AddAttribute ("NumUnschedPrioBands", "Number of priority bands dedicated for unscheduled packets",
                   UintegerValue (2),
                   MakeUintegerAccessor (&HomaL4Protocol::m_numUnschedPrioBands),
                   MakeUintegerChecker<uint8_t> ())
    .AddAttribute ("OvercommitLevel", "Minimum number of messages to Grant at the same time",
                   UintegerValue (1),
                   MakeUintegerAccessor (&HomaL4Protocol::m_overcommitLevel),
                   MakeUintegerChecker<uint8_t> ())
    .AddAttribute ("UseSrrScheduling", "If true, receiver orders active inbound messages in FIFO/SRR-like order instead of SRPT by remaining bytes.",
             BooleanValue (false),
             MakeBooleanAccessor (&HomaL4Protocol::m_useSrrScheduling),
             MakeBooleanChecker ())
    .AddAttribute ("SirdEnabled", "Enable SIRD-compatible receiver-driven control loops.",
             BooleanValue (false),
             MakeBooleanAccessor (&HomaL4Protocol::m_sirdEnabled),
             MakeBooleanChecker ())
    .AddAttribute ("SirdCreditBudgetPkts", "Baseline per-sender grant budget in packets.",
             UintegerValue (12),
             MakeUintegerAccessor (&HomaL4Protocol::m_sirdCreditBudgetPkts),
             MakeUintegerChecker<uint16_t> (1))
    .AddAttribute ("SirdUnschThresholdPkts", "Line-rate startup threshold in packets.",
             UintegerValue (12),
             MakeUintegerAccessor (&HomaL4Protocol::m_sirdUnschThresholdPkts),
             MakeUintegerChecker<uint16_t> (1))
    .AddAttribute ("SirdEcnMdFactor", "Multiplicative decrease factor for ECN loop.",
             DoubleValue (0.85),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdEcnMdFactor),
             MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("SirdEcnAiStep", "Additive increase step in packets for ECN loop.",
             DoubleValue (1.0),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdEcnAiStep),
             MakeDoubleChecker<double> (0.0))
    .AddAttribute ("SirdSenderMdFactor", "Multiplicative decrease factor for sender feedback loop.",
             DoubleValue (0.8),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdSenderMdFactor),
             MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("SirdSenderAiStep", "Additive increase step in packets for sender feedback loop.",
             DoubleValue (1.0),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdSenderAiStep),
             MakeDoubleChecker<double> (0.0))
    .AddAttribute ("SirdEcnAlphaGain", "EWMA gain for ECN CE-ratio estimation.",
             DoubleValue (0.125),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdEcnAlphaGain),
             MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("SirdSenderCsnThreshold", "Accumulated credit threshold beyond which sender sets csn (packets).",
             UintegerValue (20),
             MakeUintegerAccessor (&HomaL4Protocol::m_sirdSenderCsnThreshold),
             MakeUintegerChecker<uint16_t> (0))
    .AddAttribute ("InbndRtxTimeout", "Time value to determine the retransmission timeout of InboundMsgs",
                   TimeValue (MilliSeconds (1)),
                   MakeTimeAccessor (&HomaL4Protocol::m_inboundRtxTimeout),
                   MakeTimeChecker (MicroSeconds (0)))
    .AddAttribute ("OutbndRtxTimeout", "Time value to determine the timeout of OutboundMsgs",
                   TimeValue (MilliSeconds (10)),
                   MakeTimeAccessor (&HomaL4Protocol::m_outboundRtxTimeout),
                   MakeTimeChecker (MicroSeconds (0)))
    .AddAttribute ("MaxRtxCnt", "Maximum allowed consecutive rtx timeout count per message",
                   UintegerValue (5),
                   MakeUintegerAccessor (&HomaL4Protocol::m_maxNumRtxPerMsg),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("OptimizeMemory", 
                   "High performant mode (only packet sizes are stored to save from memory).",
                   BooleanValue (true),
                   MakeBooleanAccessor (&HomaL4Protocol::m_memIsOptimized),
                   MakeBooleanChecker ())
    .AddTraceSource ("MsgBegin",
                     "Trace source indicating a message has been delivered to "
                     "the HomaL4Protocol by the sender application layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_msgBeginTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("MsgFinish",
                     "Trace source indicating a message has been delivered to "
                     "the receiver application by the HomaL4Protocol layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_msgFinishTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("DataPktArrival",
                     "Trace source indicating a DATA packet has arrived "
                     "to the HomaL4Protocol layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_dataRecvTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("DataPktDeparture",
                     "Trace source indicating a DATA packet has departed "
                     "from the HomaL4Protocol layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_dataSendTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("CtrlPktArrival",
                     "Trace source indicating a control packet has arrived "
                     "to the HomaL4Protocol layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_ctrlRecvTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("CtrlPktArrivalTxMsg",
                     "Trace source indicating a control packet has arrived "
                     "to the HomaL4Protocol layer, including txMsgId.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_ctrlRecvTxMsgTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("PathRtt",
                     "Trace source for pure path RTT samples derived from Homa packet timing.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_pathRttTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdGrantDecision",
                     "Trace source for SIRD grant decisions.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdGrantDecisionTrace),
                     "ns3::TracedCallback")//上面 3 个 trace 已经弃用
    .AddTraceSource ("SirdSenderCreditState",
                     "Trace source for sender-side held SIRD credit.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdSenderCreditStateTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdReceiverCreditState",
                     "Trace source for receiver-side available SIRD credit.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdReceiverCreditStateTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdBucketState",
                     "Trace source for SIRD per-sender/global bucket state.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdBucketStateTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdPacketState",
                     "Trace source for per-packet SIRD state (flags/credit/CE/CSN).",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdPacketStateTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdLoopState",
                     "Trace source for per-sender SIRD control-loop state.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdLoopStateTrace),
                     "ns3::TracedCallback")
  ;
  return tid;
}
    
// 构造函数：初始化端点复用器，并创建发送/接收两个核心调度器对象。
HomaL4Protocol::HomaL4Protocol ()
  : m_endPoints (new Ipv4EndPointDemux ())
{
  NS_LOG_FUNCTION (this);
      
  m_sendScheduler = CreateObject<HomaSendScheduler> (this);
  m_recvScheduler = CreateObject<HomaRecvScheduler> (this); 
}

// 析构函数：当前仅做日志记录，资源释放由 DoDispose 统一完成。
HomaL4Protocol::~HomaL4Protocol ()
{
  NS_LOG_FUNCTION_NOARGS ();
}
    
void 
// 将协议实例绑定到节点，读取底层网卡 MTU/链路速率，并初始化发送队列空闲时间。
HomaL4Protocol::SetNode (Ptr<Node> node)
{
  m_node = node;
    
  Ptr<NetDevice> netDevice = m_node->GetDevice (0);
  m_mtu = netDevice->GetMtu ();
    
  PointToPointNetDevice* p2pNetDevice = dynamic_cast<PointToPointNetDevice*>(&(*(netDevice)));
  m_linkRate = p2pNetDevice->GetDataRate ();
    
  m_nextTimeTxQueWillBeEmpty = Simulator::Now ();
}
    
Ptr<Node> 
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetNode(void) const
{
  return m_node;
}
    
uint32_t
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetMtu (void) const
{
  return m_mtu;
}
    
uint16_t 
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetBdp(void) const
{
  return m_bdp;
}

Time
HomaL4Protocol::GetFullDataPktTxTime (void) const
{
  PppHeader ppp;
  return m_linkRate.CalculateBytesTxTime (m_mtu + ppp.GetSerializedSize ());
}
    
int 
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetProtocolNumber (void) const
{
  return PROT_NUMBER;
}
 
Time
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetInboundRtxTimeout(void) const
{
  return m_inboundRtxTimeout;
}
    
Time
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetOutboundRtxTimeout(void) const
{
  return m_outboundRtxTimeout;
}
    
uint16_t 
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetMaxNumRtxPerMsg(void) const
{
  return m_maxNumRtxPerMsg;
}
    
uint8_t
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetNumTotalPrioBands (void) const
{
  return m_numTotalPrioBands;
}
    
uint8_t
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetNumUnschedPrioBands (void) const
{
  return m_numUnschedPrioBands;
}
    
uint8_t
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetOvercommitLevel (void) const
{
  return m_overcommitLevel;
}

bool
// 根据当前状态进行条件判断，返回布尔决策供上层流程分支使用。
HomaL4Protocol::UseSrrScheduling (void) const
{
  return m_useSrrScheduling;
}

bool
// 根据当前状态进行条件判断，返回布尔决策供上层流程分支使用。
HomaL4Protocol::IsSirdEnabled (void) const
{
  return m_sirdEnabled;
}

uint16_t
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetSirdCreditBudgetPkts (void) const
{
  return m_sirdCreditBudgetPkts;
}

uint16_t
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetSirdUnschThresholdPkts (void) const
{
  return m_sirdUnschThresholdPkts;
}

double
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetSirdEcnMdFactor (void) const
{
  return m_sirdEcnMdFactor;
}

double
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetSirdEcnAiStep (void) const
{
  return m_sirdEcnAiStep;
}

double
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetSirdSenderMdFactor (void) const
{
  return m_sirdSenderMdFactor;
}

double
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetSirdSenderAiStep (void) const
{
  return m_sirdSenderAiStep;
}

double
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetSirdEcnAlphaGain (void) const
{
  return m_sirdEcnAlphaGain;
}

uint16_t
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetSirdSenderCsnThreshold (void) const
{
  return m_sirdSenderCsnThreshold;
}

void
// 封装 SIRD 授权决策的 trace 回调，向外暴露发送方预算与拥塞状态。
HomaL4Protocol::TraceSirdGrantDecision (Ipv4Address sender,
                                        uint16_t txMsgId,
                                        uint16_t grantOffset,
                                        double senderBudgetPkts,
                                        double ecnEwma,
                                        bool senderCsn)
{
  m_sirdGrantDecisionTrace (sender,
                            txMsgId,
                            grantOffset,
                            senderBudgetPkts,
                            ecnEwma,
                            senderCsn);
}

void
HomaL4Protocol::TraceSirdSenderCreditState (Ipv4Address sender,
                                            Ipv4Address receiver,
                                            uint16_t txMsgId,
                                            uint32_t senderCreditPkts,
                                            uint8_t eventType)
{
  m_sirdSenderCreditStateTrace (sender,
                                receiver,
                                txMsgId,
                                senderCreditPkts,
                                eventType);
}

void
HomaL4Protocol::TraceSirdReceiverCreditState (Ipv4Address receiver,
                                              Ipv4Address sender,
                                              uint32_t receiverAvailPkts,
                                              uint32_t receiverBudgetPkts,
                                              uint32_t senderAvailPkts,
                                              uint32_t senderBudgetPkts,
                                              uint8_t eventType)
{
  m_sirdReceiverCreditStateTrace (receiver,
                                  sender,
                                  receiverAvailPkts,
                                  receiverBudgetPkts,
                                  senderAvailPkts,
                                  senderBudgetPkts,
                                  eventType);
}

void
// 封装 SIRD bucket 状态的 trace 回调，便于观测 per-sender/global credit 变化。
HomaL4Protocol::TraceSirdBucketState (Ipv4Address receiver,
                                      Ipv4Address sender,
                                      double senderBudgetHostPkts,
                                      uint32_t senderCreditsInUsePkts,
                                      uint32_t globalCreditsInUsePkts,
                                      uint32_t globalBudgetPkts,
                                      uint8_t eventType)
{
  m_sirdBucketStateTrace (receiver,
                          sender,
                          senderBudgetHostPkts,
                          senderCreditsInUsePkts,
                          globalCreditsInUsePkts,
                          globalBudgetPkts,
                          eventType);
}

void
// 封装每包状态 trace 回调，暴露 flags/credit/CE/CSN 等关键字段。
HomaL4Protocol::TraceSirdPacketState (Ipv4Address receiver,
                                      Ipv4Address sender,
                                      uint8_t flags,
                                      uint32_t msgPktState,
                                      uint16_t grantOffset,
                                      uint8_t ecn,
                                      bool csn,
                                      uint32_t creditState)
{
  m_sirdPacketStateTrace (receiver,
                          sender,
                          flags,
                          msgPktState,
                          grantOffset,
                          ecn,
                          csn,
                          creditState);
}

void
HomaL4Protocol::TraceSirdLoopState (Ipv4Address receiver,
                                    Ipv4Address sender,
                                    double netBudgetPkts,
                                    double hostBudgetPkts,
                                    double effectiveBudgetPkts,
                                    double ceEwma,
                                    uint64_t loopState,
                                    uint64_t counterState)
{
  m_sirdLoopStateTrace (receiver,
                        sender,
                        netBudgetPkts,
                        hostBudgetPkts,
                        effectiveBudgetPkts,
                        ceEwma,
                        loopState,
                        counterState);
}
    
// 根据当前状态进行条件判断，返回布尔决策供上层流程分支使用。
bool HomaL4Protocol::MemIsOptimized (void)
{
  return m_memIsOptimized;
}
    
/*
 * This method is called by AggregateObject and completes the aggregation
 * by setting the node in the homa stack and link it to the ipv4 object
 * present in the node along with the socket factory
 */
void
// 完成与 Node/IPv4 的聚合绑定，创建 SocketFactory，并注册到 IPv4 上下行路径。
HomaL4Protocol::NotifyNewAggregate ()
{
  NS_LOG_FUNCTION (this);
  Ptr<Node> node = this->GetObject<Node> ();
  Ptr<Ipv4> ipv4 = this->GetObject<Ipv4> ();
    
  NS_ASSERT_MSG(ipv4, "Homa L4 Protocol supports only IPv4.");

  if (m_node == 0)
    {
      if ((node != 0) && (ipv4 != 0))
        {
          this->SetNode (node);
          Ptr<HomaSocketFactory> homaFactory = CreateObject<HomaSocketFactory> ();
          homaFactory->SetHoma (this);
          node->AggregateObject (homaFactory);
          
          NS_ASSERT(m_mtu); // m_mtu is set inside SetNode() above.
          NS_ASSERT_MSG(m_numTotalPrioBands > m_numUnschedPrioBands,
                "Total number of priority bands should be larger than the number of bands dedicated for unscheduled packets.");
          NS_ASSERT(m_outboundRtxTimeout != Time(0));
          NS_ASSERT(m_inboundRtxTimeout != Time(0));
        }
    }
  
  if (ipv4 != 0 && m_downTarget.IsNull())
    {
      // We register this HomaL4Protocol instance as one of the upper targets of the IP layer
      ipv4->Insert (this);
      // We set our down target to the IPv4 send function.
      this->SetDownTarget (MakeCallback (&Ipv4::Send, ipv4));
    }
  IpL4Protocol::NotifyNewAggregate ();
}
    
void
// 释放协议层对象持有的 socket/endPoint 等资源，并断开下行回调。
HomaL4Protocol::DoDispose (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  for (std::vector<Ptr<HomaSocket> >::iterator i = m_sockets.begin (); i != m_sockets.end (); i++)
    {
      *i = 0;
    }
  m_sockets.clear ();

  if (m_endPoints != 0)
    {
      delete m_endPoints;
      m_endPoints = 0;
    }
  
  m_node = 0;
  m_downTarget.Nullify ();
/*
 = MakeNullCallback<void,Ptr<Packet>, Ipv4Address, Ipv4Address, uint8_t, Ptr<Ipv4Route> > ();
*/
  IpL4Protocol::DoDispose ();
}
 
/*
 * This method is called by HomaSocketFactory associated with m_node which 
 * returns a socket that is tied to this HomaL4Protocol instance.
 */
Ptr<Socket>
// 创建并初始化一个 HomaSocket，使其与当前节点和协议实例关联。
HomaL4Protocol::CreateSocket (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  Ptr<HomaSocket> socket = CreateObject<HomaSocket> ();
  socket->SetNode (m_node);
  socket->SetHoma (this);
  m_sockets.push_back (socket);
  return socket;
}
    
Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (void)
{
  NS_LOG_FUNCTION (this);
  return m_endPoints->Allocate ();
}

Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (Ipv4Address address)
{
  NS_LOG_FUNCTION (this << address);
  return m_endPoints->Allocate (address);
}

Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (Ptr<NetDevice> boundNetDevice, uint16_t port)
{
  NS_LOG_FUNCTION (this << boundNetDevice << port);
  return m_endPoints->Allocate (boundNetDevice, port);
}

Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (Ptr<NetDevice> boundNetDevice, Ipv4Address address, uint16_t port)
{
  NS_LOG_FUNCTION (this << boundNetDevice << address << port);
  return m_endPoints->Allocate (boundNetDevice, address, port);
}
Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (Ptr<NetDevice> boundNetDevice,
                         Ipv4Address localAddress, uint16_t localPort,
                         Ipv4Address peerAddress, uint16_t peerPort)
{
  NS_LOG_FUNCTION (this << boundNetDevice << localAddress << localPort << peerAddress << peerPort);
  return m_endPoints->Allocate (boundNetDevice,
                                localAddress, localPort,
                                peerAddress, peerPort);
}

void 
// 归还此前分配的 IPv4 端点，释放端口绑定状态。
HomaL4Protocol::DeAllocate (Ipv4EndPoint *endPoint)
{
  NS_LOG_FUNCTION (this << endPoint);
  m_endPoints->DeAllocate (endPoint);
}
    
void
// 应用层发送入口：构造 OutboundMsg 并交给发送调度器分配 txMsgId 与后续发送时机。
HomaL4Protocol::Send (Ptr<Packet> message, 
                     Ipv4Address saddr, Ipv4Address daddr, 
                     uint16_t sport, uint16_t dport)
{
  NS_LOG_FUNCTION (this << message << saddr << daddr << sport << dport);
    
  Send(message, saddr, daddr, sport, dport, 0);
}
    
void
// 应用层发送入口：构造 OutboundMsg 并交给发送调度器分配 txMsgId 与后续发送时机。
HomaL4Protocol::Send (Ptr<Packet> message, 
                     Ipv4Address saddr, Ipv4Address daddr, 
                     uint16_t sport, uint16_t dport, Ptr<Ipv4Route> route)
{
  NS_LOG_FUNCTION (this << message << saddr << daddr << sport << dport << route);
  
  Ptr<HomaOutboundMsg> outMsg = CreateObject<HomaOutboundMsg> (message, saddr, daddr, 
                                                               sport, dport, this);
  outMsg->SetRoute (route); // This is mostly unnecessary
    
  int txMsgId = m_sendScheduler->ScheduleNewMsg(outMsg);
    
  if (txMsgId >= 0)
    m_msgBeginTrace(message, saddr, daddr, sport, dport, txMsgId);
}

/*
 * This method is called either by the associated HomaSendScheduler after 
 * the next data packet to transmit is selected or by the associated
 * HomaRecvScheduler once a control packet is generated. The selected 
 * packet is then pushed down to the lower IP layer.
 */
void
// 统一下发出口：更新链路序列化队列时间，记录发送 trace，并调用 IPv4 下行回调。
HomaL4Protocol::SendDown (Ptr<Packet> packet, 
                          Ipv4Address saddr, Ipv4Address daddr, 
                          Ptr<Ipv4Route> route)
{
  NS_LOG_FUNCTION (this << packet << saddr << daddr << route);
    
  PppHeader pph;
  Ipv4Header iph;
  HomaHeader homaHeader;
  packet->PeekHeader(homaHeader);
    
  uint32_t headerSize = iph.GetSerializedSize () + pph.GetSerializedSize ();  
  Time timeToSerialize = m_linkRate.CalculateBytesTxTime (packet->GetSize () + headerSize);
    
  if(Simulator::Now() <= m_nextTimeTxQueWillBeEmpty)
  {
    m_nextTimeTxQueWillBeEmpty += timeToSerialize;
  }
  else
  {
    m_nextTimeTxQueWillBeEmpty = Simulator::Now() + timeToSerialize;
  }
    
  if (homaHeader.GetFlags () & HomaHeader::Flags_t::DATA)
  {
    uint32_t payloadSize = m_mtu - iph.GetSerializedSize () - homaHeader.GetSerializedSize ();
    uint32_t msgSizeBytes = homaHeader.GetMsgSize ();
    uint16_t msgSizePkts = msgSizeBytes / payloadSize + (msgSizeBytes % payloadSize != 0);
    uint16_t remainingPkts = msgSizePkts - homaHeader.GetGrantOffset () - (uint16_t)1 + m_bdp; 
    m_dataSendTrace(packet, saddr, daddr, homaHeader.GetSrcPort (), 
                    homaHeader.GetDstPort (), homaHeader.GetTxMsgId (), 
                    homaHeader.GetPktOffset (), remainingPkts);

    if (IsPathRttTriggerPacket (homaHeader))
      {
        std::string key = MakePathRttKey (saddr,
                                          daddr,
                                          homaHeader.GetSrcPort (),
                                          homaHeader.GetDstPort (),
                                          homaHeader.GetTxMsgId ());
        PathRttFlowState& state = g_pathRttFlowStates[key];
        if (!state.sampleEmitted && !state.haveTriggerTx)
          {
            state.haveTriggerTx = true;
            state.triggerTxTime = Simulator::Now ();
            state.triggerKind = GetPathRttTriggerKind (homaHeader);
          }
      }
  }
  else if (IsPathRttResponsePacket (homaHeader))
  {
    std::string key = MakePathRttKey (daddr,
                                      saddr,
                                      homaHeader.GetDstPort (),
                                      homaHeader.GetSrcPort (),
                                      homaHeader.GetTxMsgId ());
    auto it = g_pathRttFlowStates.find (key);
    if (it != g_pathRttFlowStates.end () &&
        !it->second.sampleEmitted &&
        it->second.haveTriggerRx)
      {
        it->second.responseTxStates.push_back ({homaHeader.GetFlags (),
                                                homaHeader.GetGrantOffset (),
                                                homaHeader.GetPktOffset (),
                                                Simulator::Now ()});
      }
  }
   
  m_downTarget (packet, saddr, daddr, PROT_NUMBER, route);
}
    
// 估算当前发送队列还需多久排空，供发送调度器决定下一次触发时机。
Time HomaL4Protocol::GetTimeToDrainTxQueue ()
{
  NS_LOG_FUNCTION(this);
    
  if(Simulator::Now() < m_nextTimeTxQueWillBeEmpty)
  {
    return m_nextTimeTxQueWillBeEmpty - Simulator::Now();
  }
  else
  {
    return Time(0);
  }
}

/*
 * This method is called by the lower IP layer to notify arrival of a 
 * new packet from the network. The method then classifies the packet
 * and forward it to the appropriate scheduler (send or receive) to have
 * Homa Transport logic applied on it.
 */
enum IpL4Protocol::RxStatus
// 网络接收入口：校验报文并按 DATA/BUSY 与 GRANT/RESEND/ACK 分发到接收或发送调度器。
HomaL4Protocol::Receive (Ptr<Packet> packet,
                        Ipv4Header const &header,
                        Ptr<Ipv4Interface> interface)
{
  NS_LOG_FUNCTION (this << packet << header << interface);
    
  NS_LOG_DEBUG ("HomaL4Protocol (" << this << ") received: " << packet->ToString ());
    
  NS_ASSERT(header.GetProtocol() == PROT_NUMBER);
  
  Ptr<Packet> cp = packet->Copy ();
    
  HomaHeader homaHeader;
  cp->RemoveHeader(homaHeader);//homaHeader：拿到了包里的 Homa 头字段
  //cp：头已经被移除，只剩 payload

  NS_ASSERT_MSG(cp->GetSize()==homaHeader.GetPayloadSize(),
                "HomaL4Protocol (" << this << ") received a packet "
                " whose payload size doesn't match the homa header field!");

  NS_LOG_DEBUG ("Looking up dst " << header.GetDestination () << " port " << homaHeader.GetDstPort ()); 
  Ipv4EndPointDemux::EndPoints endPoints =
    m_endPoints->Lookup (header.GetDestination (), homaHeader.GetDstPort (),
                         header.GetSource (), homaHeader.GetSrcPort (), interface);
  if (endPoints.empty ())
    {
      NS_LOG_LOGIC ("RX_ENDPOINT_UNREACH");
      return IpL4Protocol::RX_ENDPOINT_UNREACH;
    }
    
  //  The Homa protocol logic starts here!
  uint8_t rxFlag = homaHeader.GetFlags ();
  if (rxFlag & HomaHeader::Flags_t::DATA)
    {
      if (IsPathRttTriggerPacket (homaHeader))
        {
          std::string key = MakePathRttKey (header.GetSource (),
                                            header.GetDestination (),
                                            homaHeader.GetSrcPort (),
                                            homaHeader.GetDstPort (),
                                            homaHeader.GetTxMsgId ());
          PathRttFlowState& state = g_pathRttFlowStates[key];
          if (!state.sampleEmitted && !state.haveTriggerRx)
            {
              state.haveTriggerRx = true;
              state.triggerRxTime = Simulator::Now ();
              state.triggerKind = GetPathRttTriggerKind (homaHeader);
            }
        }
    }
  else if (IsPathRttResponsePacket (homaHeader))
    {
      std::string key = MakePathRttKey (header.GetDestination (),
                                        header.GetSource (),
                                        homaHeader.GetDstPort (),
                                        homaHeader.GetSrcPort (),
                                        homaHeader.GetTxMsgId ());
      auto stateIt = g_pathRttFlowStates.find (key);
      if (stateIt != g_pathRttFlowStates.end () &&
          !stateIt->second.sampleEmitted &&
          stateIt->second.haveTriggerTx &&
          stateIt->second.haveTriggerRx)
        {
          auto& responseTxStates = stateIt->second.responseTxStates;
          for (auto respIt = responseTxStates.begin (); respIt != responseTxStates.end (); ++respIt)
            {
              if (respIt->flags == homaHeader.GetFlags () &&
                  respIt->grantOffset == homaHeader.GetGrantOffset () &&
                  respIt->pktOffset == homaHeader.GetPktOffset ())
                {
                  Time receiverDelay = respIt->txTime - stateIt->second.triggerRxTime;
                  Time pathRtt = (Simulator::Now () - stateIt->second.triggerTxTime) - receiverDelay;
                  if (pathRtt.GetNanoSeconds () >= 0)
                    {
                      m_pathRttTrace (header.GetDestination (),
                                      header.GetSource (),
                                      homaHeader.GetDstPort (),
                                      homaHeader.GetSrcPort (),
                                      homaHeader.GetTxMsgId (),
                                      stateIt->second.triggerKind,
                                      homaHeader.GetFlags (),
                                      pathRtt);
                    }
                  stateIt->second.sampleEmitted = true;
                  stateIt->second.responseTxStates.clear ();
                  break;
                }
            }
        }
    }

  if (rxFlag & HomaHeader::Flags_t::DATA ||
      rxFlag & HomaHeader::Flags_t::BUSY)
  {
    m_recvScheduler->ReceivePacket(cp, header, homaHeader, interface);//是 Data 或者 Busy 包，调度器受着
  }
  else if ((rxFlag & HomaHeader::Flags_t::GRANT) ||
           (rxFlag & HomaHeader::Flags_t::RESEND) ||
           (rxFlag & HomaHeader::Flags_t::ACK))
  {
    m_sendScheduler->CtrlPktRecvdForOutboundMsg(header, homaHeader);//收到控制包
  }
  else
  {
    NS_LOG_ERROR("ERROR: HomaL4Protocol received an unknown type of a packet: " 
                 << homaHeader.FlagsToString(rxFlag));
    return IpL4Protocol::RX_ENDPOINT_UNREACH;
  }
    
  if (rxFlag & HomaHeader::Flags_t::DATA)
    m_dataRecvTrace(cp, header.GetSource (), header.GetDestination (), 
                    homaHeader.GetSrcPort (), homaHeader.GetDstPort (), 
                    homaHeader.GetTxMsgId (), homaHeader.GetPktOffset (), 
                    homaHeader.GetPrio ());//trace 用
  else
  {
    m_ctrlRecvTrace(cp, header.GetSource (), header.GetDestination (), 
                    homaHeader.GetSrcPort (), homaHeader.GetDstPort (), 
                    homaHeader.GetFlags (), homaHeader.GetGrantOffset(), 
                    homaHeader.GetPrio());
    m_ctrlRecvTxMsgTrace(header.GetSource (), header.GetDestination (),
                         homaHeader.GetSrcPort (), homaHeader.GetDstPort (),
                         homaHeader.GetTxMsgId (), homaHeader.GetFlags (),
                         homaHeader.GetGrantOffset (), homaHeader.GetPrio ());
    uint8_t ecn = static_cast<uint8_t> (header.GetEcn ());
    bool csn = (homaHeader.GetFeedbackFlags () & HomaHeader::FeedbackFlags_t::FEEDBACK_CSN) != 0;
    TraceSirdPacketState (header.GetDestination (),
                          header.GetSource (),
                          homaHeader.GetFlags (),
                          (static_cast<uint32_t> (homaHeader.GetTxMsgId ()) << 16) |
                            static_cast<uint32_t> (homaHeader.GetPktOffset ()),
                          homaHeader.GetGrantOffset (),
                          ecn,
                          csn,
                          0);
  }
    
  return IpL4Protocol::RX_OK;
}
    
enum IpL4Protocol::RxStatus
// 网络接收入口：校验报文并按 DATA/BUSY 与 GRANT/RESEND/ACK 分发到接收或发送调度器。
HomaL4Protocol::Receive (Ptr<Packet> packet,
                        Ipv6Header const &header,
                        Ptr<Ipv6Interface> interface)
{
  NS_FATAL_ERROR_CONT("HomaL4Protocol currently doesn't support IPv6. Use IPv4 instead.");
  return IpL4Protocol::RX_ENDPOINT_UNREACH;
}

/*
 * This method is called by the HomaRecvScheduler everytime a message is ready to
 * be forwarded up to the applications. 
 */
// 将完整重组后的消息按四元组查找端点并上交应用，同时触发消息完成 trace。
void HomaL4Protocol::ForwardUp (Ptr<Packet> completeMsg,
                                const Ipv4Header &header,
                                uint16_t sport, uint16_t dport, uint16_t txMsgId,
                                Ptr<Ipv4Interface> incomingInterface)
{
  NS_LOG_FUNCTION (this << completeMsg << header << sport << incomingInterface);
    
  NS_LOG_DEBUG ("Looking up dst " << header.GetDestination () << " port " << dport); 
  Ipv4EndPointDemux::EndPoints endPoints =
    m_endPoints->Lookup (header.GetDestination (), dport,
                         header.GetSource (), sport, incomingInterface);
    
  NS_ASSERT_MSG(!endPoints.empty (), 
                "HomaL4Protocol was able to find an endpoint when msg was received, but now it couldn't");
    
  for (Ipv4EndPointDemux::EndPointsI endPoint = endPoints.begin ();
         endPoint != endPoints.end (); endPoint++)
  {
    (*endPoint)->ForwardUp (completeMsg, header, sport, incomingInterface);
  }
    
  m_msgFinishTrace(completeMsg, header.GetSource(), header.GetDestination(), 
                   sport, dport, (int)txMsgId);
}

// inherited from Ipv4L4Protocol (Not used for Homa Transport Purposes)
void 
// 处理 ICMP 反馈：解析原始端口并转交对应端点，便于上层感知网络错误。
HomaL4Protocol::ReceiveIcmp (Ipv4Address icmpSource, uint8_t icmpTtl,
                            uint8_t icmpType, uint8_t icmpCode, uint32_t icmpInfo,
                            Ipv4Address payloadSource,Ipv4Address payloadDestination,
                            const uint8_t payload[8])
{
  NS_LOG_FUNCTION (this << icmpSource << icmpTtl << icmpType << icmpCode << icmpInfo 
                        << payloadSource << payloadDestination);
  uint16_t src, dst;
  src = payload[0] << 8;
  src |= payload[1];
  dst = payload[2] << 8;
  dst |= payload[3];

  Ipv4EndPoint *endPoint = m_endPoints->SimpleLookup (payloadSource, src, payloadDestination, dst);
  if (endPoint != 0)
    {
      endPoint->ForwardIcmp (icmpSource, icmpTtl, icmpType, icmpCode, icmpInfo);
    }
  else
    {
      NS_LOG_DEBUG ("no endpoint found source=" << payloadSource <<
                    ", destination="<<payloadDestination<<
                    ", src=" << src << ", dst=" << dst);
    }
}
    
void
// 设置 IPv4 下行发送回调，使 Homa 能把封装后的分组交给网络层。
HomaL4Protocol::SetDownTarget (IpL4Protocol::DownTargetCallback callback)
{
  NS_LOG_FUNCTION (this);
  m_downTarget = callback;
}
    
void
// IPv6 下行回调设置接口；当前实现明确不支持 IPv6 并直接报错。
HomaL4Protocol::SetDownTarget6 (IpL4Protocol::DownTargetCallback6 callback)
{
  NS_LOG_FUNCTION (this);
  NS_FATAL_ERROR("HomaL4Protocol currently doesn't support IPv6. Use IPv4 instead.");
  m_downTarget6 = callback;
}

IpL4Protocol::DownTargetCallback
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetDownTarget (void) const
{
  return m_downTarget;
}
    
IpL4Protocol::DownTargetCallback6
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetDownTarget6 (void) const
{
  NS_FATAL_ERROR("HomaL4Protocol currently doesn't support IPv6. Use IPv4 instead.");
  return m_downTarget6;
}

/******************************************************************************/

// 注册并返回 HomaOutboundMsg 的 TypeId。
TypeId HomaOutboundMsg::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaOutboundMsg")
    .SetParent<Object> ()
    .SetGroupName("Internet")
  ;
  return tid;
}

/*
 * This method creates a new outbound message with the given information.
 */
// 构造发送侧消息状态：分片、初始化可发送窗口、以及 SIRD 首包授权等待策略。
HomaOutboundMsg::HomaOutboundMsg (Ptr<Packet> message, 
                                  Ipv4Address saddr, Ipv4Address daddr, 
                                  uint16_t sport, uint16_t dport, 
                                  Ptr<HomaL4Protocol> homa)
    : m_route(0),
      m_senderHeldCreditPkts(0),
      m_prio(0),
      m_prioSetByReceiver(false),
  m_waitForFirstGrant(false),
  m_initialCreditRequestSent(false),
      m_isExpired(false)
{
  NS_LOG_FUNCTION (this);
      
  m_saddr = saddr;
  m_daddr = daddr;
  m_sport = sport;
  m_dport = dport;
  m_homa = homa;
    
  m_msgSizeBytes = message->GetSize ();
  // The remaining undelivered message size equals to the total message size in the beginning
  m_remainingBytes = m_msgSizeBytes;
    
  HomaHeader homah;
  Ipv4Header ipv4h;
  m_maxPayloadSize = m_homa->GetMtu () - homah.GetSerializedSize () - ipv4h.GetSerializedSize ();
    
  // Packetize the message into MTU sized packets and store the corresponding state
  uint32_t unpacketizedBytes = m_msgSizeBytes;
  uint16_t numPkts = 0;
  uint32_t nextPktSize;
  Ptr<Packet> nextPkt;
  while (unpacketizedBytes > 0)
  {
    nextPktSize = std::min(unpacketizedBytes, m_maxPayloadSize);

    if (m_homa->MemIsOptimized ())
    {
      m_pktSizes.push_back(nextPktSize);
    }
    else
    {
      nextPkt = message->CreateFragment (m_msgSizeBytes - unpacketizedBytes, nextPktSize);
      m_packets.push_back(nextPkt);
    }

    m_pktTxQ.push(numPkts);

    unpacketizedBytes -= nextPktSize;
    numPkts++;
  } 
  NS_ASSERT(numPkts == m_msgSizeBytes / m_maxPayloadSize + (m_msgSizeBytes % m_maxPayloadSize != 0));
  
  if (numPkts == 0)
  {
    m_maxGrantedIdx = 0;
  }
  else if (m_homa->IsSirdEnabled ())
  {
    uint16_t unschThresholdPkts = std::max<uint16_t> (1, m_homa->GetSirdUnschThresholdPkts ());
    if (numPkts > unschThresholdPkts)
    {
      // For long messages, require explicit GRANT before the first data packet.
      m_maxGrantedIdx = 0;
      m_waitForFirstGrant = true;
    }
    else
    {
      m_maxGrantedIdx = std::min((uint16_t)(m_homa->GetBdp () - 1), (uint16_t)(numPkts - 1));
    }
  }
  else
  {
    m_maxGrantedIdx = std::min((uint16_t)(m_homa->GetBdp () -1), numPkts);
  }

  if (!m_waitForFirstGrant && numPkts > 0)
    {
      m_senderHeldCreditPkts = std::min<uint16_t> (static_cast<uint16_t> (m_maxGrantedIdx + 1), numPkts);
    }
          
  // FIX: There is no timeout mechanism on the sender side for Homa 
  //      (even for garbage collection purposes), so removing the following
  // m_rtxEvent = Simulator::Schedule (m_homa->GetOutboundRtxTimeout (), 
  //                                   &HomaOutboundMsg::ExpireRtxTimeout, 
  //                                   this, m_maxGrantedIdx);
}

// 析构函数：当前无额外清理逻辑，保留日志便于调试生命周期。
HomaOutboundMsg::~HomaOutboundMsg ()
{
  NS_LOG_FUNCTION_NOARGS ();
}

// 记录消息可选路由信息，供发送下发时透传给 IPv4。
void HomaOutboundMsg::SetRoute(Ptr<Ipv4Route> route)
{
  m_route = route;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ptr<Ipv4Route> HomaOutboundMsg::GetRoute ()
{
  return m_route;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint32_t HomaOutboundMsg::GetRemainingBytes()
{
  return m_remainingBytes;
}

// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint32_t HomaOutboundMsg::GetMsgSizeBytes()
{
  return m_msgSizeBytes;
}
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaOutboundMsg::GetMsgSizePkts()
{
  return m_msgSizeBytes / m_maxPayloadSize + (m_msgSizeBytes % m_maxPayloadSize != 0);
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ipv4Address HomaOutboundMsg::GetSrcAddress ()
{
  return m_saddr;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ipv4Address HomaOutboundMsg::GetDstAddress ()
{
  return m_daddr;
}

// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaOutboundMsg::GetSrcPort ()
{
  return m_sport;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaOutboundMsg::GetDstPort ()
{
  return m_dport;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaOutboundMsg::GetMaxGrantedIdx ()
{
  return m_maxGrantedIdx;
}
    
// 根据当前状态进行条件判断，返回布尔决策供上层流程分支使用。
bool HomaOutboundMsg::IsExpired ()
{
  return m_isExpired;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint8_t HomaOutboundMsg::GetPrio (uint16_t pktOffset)
{
  if (!m_prioSetByReceiver)
  {
//     if (this->GetMsgSizePkts () < m_homa->GetBdp ())
    if (this->GetMsgSizePkts () < 13) // Based on heuristics
        return 0;
    else
      return m_homa->GetNumUnschedPrioBands () - 1;
    // TODO: Determine priority of unscheduled packet (index = pktOffset)
    //       according to the distribution of message sizes.
  }
  return m_prio;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
EventId HomaOutboundMsg::GetRtxEvent ()
{
  return m_rtxEvent;
}
    
// 从发送队列挑选当前可发分片：需同时满足未发送且不超过授权上界。
bool HomaOutboundMsg::GetNextPktOffset (uint16_t &pktOffset)
{
  NS_LOG_FUNCTION (this);

  if (m_waitForFirstGrant)
  {
    NS_LOG_LOGIC("HomaOutboundMsg (" << this
                 << ") is waiting for first explicit GRANT before data transmission.");
    return false;
  }
    
  if (!m_pktTxQ.empty ())
  {
    uint16_t nextPktOffset = m_pktTxQ.top();
    
    if (nextPktOffset <= m_maxGrantedIdx && nextPktOffset < this->GetMsgSizePkts ())
    {
      // The selected packet is not delivered and not on flight
      NS_LOG_LOGIC("HomaOutboundMsg (" << this 
                   << ") can send packet " << nextPktOffset << " next.");
      pktOffset = nextPktOffset;
      return true;
    }
  }
  NS_LOG_LOGIC("HomaOutboundMsg (" << this 
               << ") doesn't have any packet to send!");
  return false;
}

// 判断长消息是否需要先发零负载 DATA 作为首轮信用请求。
bool HomaOutboundMsg::NeedsInitialCreditRequest (void) const
{
  return m_waitForFirstGrant && !m_initialCreditRequestSent;
}

// 标记首轮信用请求已发送，避免重复请求。
void HomaOutboundMsg::MarkInitialCreditRequestSent (void)
{
  m_initialCreditRequestSent = true;
}

// 构造零负载 DATA 分组，请求接收端显式发放第一笔授权。
Ptr<Packet> HomaOutboundMsg::GenerateInitialCreditRequest (uint16_t txMsgId)
{
  HomaHeader homaHeader;
  homaHeader.SetDstPort (m_dport);
  homaHeader.SetSrcPort (m_sport);
  homaHeader.SetTxMsgId (txMsgId);
  homaHeader.SetFlags (HomaHeader::Flags_t::DATA);
  homaHeader.SetMsgSize (m_msgSizeBytes);
  homaHeader.SetPktOffset (0);
  homaHeader.SetGrantOffset (0);
  homaHeader.SetPayloadSize (0);
  homaHeader.SetFeedbackFlags (HomaHeader::FeedbackFlags_t::FEEDBACK_NONE);
  homaHeader.SetPrio (0);

  Ptr<Packet> p = Create<Packet> ();
  SocketIpTosTag ipTosTag;
  // Keep priority in DSCP bits, ECN in low bits.
  ipTosTag.SetTos (static_cast<uint8_t> ((0u << 2) | static_cast<uint8_t> (Ipv4Header::ECN_ECT0)));
  p->ReplacePacketTag (ipTosTag);
  p->AddHeader (homaHeader);
  return p;
}
    
// 弹出并返回指定偏移的数据分片，同时清理重复排队项避免冗余发送。
Ptr<Packet> HomaOutboundMsg::RemoveNextPktFromTxQ (uint16_t pktOffset)
{
  NS_LOG_FUNCTION (this << pktOffset);
    
  NS_ASSERT_MSG(!m_pktTxQ.empty (), 
                "HomaOutboundMsg can't send a pkt if its TX queue is empty!");
  NS_ASSERT_MSG(m_pktTxQ.top() == pktOffset,
                "HomaOutboundMsg can only send the packet at the head of TX queue!");
  
  /*
   * In case a pktOffset was added multiple times to the tx queue,
   * we remove all of them at once to prevent redundant transmissions.
   */
  while(m_pktTxQ.top() == pktOffset && !m_pktTxQ.empty ())
  {
    m_pktTxQ.pop();
  }
   
  if (m_homa->MemIsOptimized ())
    return Create<Packet> (m_pktSizes[pktOffset]);
  else
    return m_packets[pktOffset]->Copy();
}

uint16_t
HomaOutboundMsg::GetAccumulatedCreditPkts (void) const
{
  return m_senderHeldCreditPkts;
}

void
HomaOutboundMsg::ConsumeSenderCredit (void)
{
  if (m_senderHeldCreditPkts > 0)
    {
      m_senderHeldCreditPkts--;
    }
}
    
/*
 * This method updates the state for the corresponding outbound message
 * upon receival of a Grant or RESEND. The state is updated only if the  
 * granted packet index is larger than the highest grant index received 
 * so far. This allows reordered Grants to be ignored when more recent 
 * ones are received.
 */
// 处理 GRANT/RESEND 携带的授权上界与优先级更新，并刷新剩余字节估计。
void HomaOutboundMsg::HandleGrantOffset (HomaHeader const &homaHeader)
{
  NS_LOG_FUNCTION (this << homaHeader);
    
  uint16_t grantOffset = homaHeader.GetGrantOffset();
  NS_ASSERT_MSG(grantOffset < this->GetMsgSizePkts (), 
                "HomaOutboundMsg shouldn't be granted after it is already fully granted!");
  
  bool wasWaitingForFirstGrant = m_waitForFirstGrant;
  uint16_t prevGrantedPkts = wasWaitingForFirstGrant ? 0 :
    std::min<uint16_t> (static_cast<uint16_t> (m_maxGrantedIdx + 1), this->GetMsgSizePkts ());
  bool firstGrant = m_waitForFirstGrant && grantOffset >= m_maxGrantedIdx;
  if (m_waitForFirstGrant && grantOffset >= m_maxGrantedIdx)
  {
    m_waitForFirstGrant = false;
  }

  if (grantOffset > m_maxGrantedIdx)
  {
    NS_LOG_LOGIC("HomaOutboundMsg (" << this 
                 << ") is increasing the Grant index to "
                 << grantOffset << ".");
      
    m_maxGrantedIdx = grantOffset;
      
    uint8_t prio = homaHeader.GetPrio();
    NS_LOG_LOGIC("HomaOutboundMsg (" << this << ") is setting priority to "
                 << (uint16_t) prio << ".");
    m_prio = prio;
    m_prioSetByReceiver = true;
      
    /*
     * Since Homa doesn't explicitly acknowledge the delivery of data packets,
     * one way to estimate the remaining bytes is to exploit the mechanism where
     * Homa grants messages in a way that there is always exactly 1 BDP worth of 
     * packets on flight. Then we can calculate the remaining bytes as the following.
    */
    m_remainingBytes = m_msgSizeBytes - (m_maxGrantedIdx+1 - m_homa->GetBdp ()) * m_maxPayloadSize;
  }
  else if (firstGrant)
  {
    uint8_t prio = homaHeader.GetPrio();
    NS_LOG_LOGIC("HomaOutboundMsg (" << this << ") is setting priority to "
                 << (uint16_t) prio << " for the first Grant.");
    m_prio = prio;
    m_prioSetByReceiver = true;
  }
  else
  {
    NS_LOG_LOGIC("HomaOutboundMsg (" << this 
                 << ") has received an out-of-order Grant. State is not updated!");
  }

  uint16_t newGrantedPkts = m_waitForFirstGrant ? 0 :
    std::min<uint16_t> (static_cast<uint16_t> (m_maxGrantedIdx + 1), this->GetMsgSizePkts ());
  if (newGrantedPkts > prevGrantedPkts)
    {
      m_senderHeldCreditPkts += (newGrantedPkts - prevGrantedPkts);
    }
}
    
// 处理 RESEND 请求：将缺失分片重新入队，并在必要时提升授权上界。
void HomaOutboundMsg::HandleResend (HomaHeader const &homaHeader)
{
  NS_LOG_FUNCTION (this << homaHeader);
    
  NS_ASSERT(homaHeader.GetFlags() & HomaHeader::Flags_t::RESEND);
  
  m_pktTxQ.push(homaHeader.GetPktOffset ());

  uint16_t grantOffset = homaHeader.GetGrantOffset();
  NS_ASSERT_MSG(grantOffset < this->GetMsgSizePkts (), 
                "HomaOutboundMsg shouldn't be granted after it is already fully granted!");
  
  if (grantOffset > m_maxGrantedIdx)
  {
    NS_LOG_LOGIC("HomaOutboundMsg (" << this 
                 << ") is increasing the Grant index to "
                 << grantOffset << ".");
      
    m_maxGrantedIdx = grantOffset;
      
    uint8_t prio = homaHeader.GetPrio();
    NS_LOG_LOGIC("HomaOutboundMsg (" << this << ") is setting priority to "
                 << (uint16_t) prio << ".");
    m_prio = prio;
    m_prioSetByReceiver = true;
  }
}
    
// 处理 ACK：校验完整确认号后将消息剩余字节置零。
void HomaOutboundMsg::HandleAck (HomaHeader const &homaHeader)
{
  NS_LOG_FUNCTION (this << homaHeader);
    
  NS_ASSERT(homaHeader.GetFlags() & HomaHeader::Flags_t::ACK);
    
  NS_ASSERT(homaHeader.GetPktOffset () == this->GetMsgSizePkts ());
  m_remainingBytes = 0;
  m_senderHeldCreditPkts = 0;
}
    
// 构造 BUSY 控制包，告知对端当前发送端暂不可立即服务该消息。
Ptr<Packet> HomaOutboundMsg::GenerateBusy (uint16_t targetTxMsgId)
{
  NS_LOG_FUNCTION (this << targetTxMsgId);
    
  uint16_t pktOffset;
  if (!m_pktTxQ.empty ())
    pktOffset = m_pktTxQ.top();
  else
    pktOffset = this->GetMsgSizePkts ();
  
  HomaHeader homaHeader;
  homaHeader.SetSrcPort (m_sport); 
  homaHeader.SetDstPort (m_dport);
  homaHeader.SetTxMsgId (targetTxMsgId);
  homaHeader.SetMsgSize (m_msgSizeBytes);
  homaHeader.SetPktOffset (pktOffset); // TODO: Is this correct?
  homaHeader.SetGrantOffset (m_maxGrantedIdx); // TODO: Is this correct?
  homaHeader.SetPrio (m_prio); // TODO: Is this correct?
  homaHeader.SetPayloadSize (0);
  homaHeader.SetFlags (HomaHeader::Flags_t::BUSY);
    
  Ptr<Packet> busyPacket = Create<Packet> ();
  busyPacket->AddHeader (homaHeader);
    
  SocketIpTosTag ipTosTag;
  ipTosTag.SetTos (0); // Busy packets have the highest priority
  // This packet may already have a SocketIpTosTag (see HomaSocket)
  busyPacket->ReplacePacketTag (ipTosTag);
    
  return busyPacket;
}
    
// 发送侧超时处理：若长时间无授权进展则将消息标记为过期。
void HomaOutboundMsg::ExpireRtxTimeout(uint16_t lastRtxGrntIdx)
{
  NS_LOG_FUNCTION(this << lastRtxGrntIdx);
    
  if (m_remainingBytes == 0) // Fully delivered (ACK received)
  {
    return;
  }
  
  if (lastRtxGrntIdx < m_maxGrantedIdx)
  {
    m_rtxEvent = Simulator::Schedule (m_homa->GetOutboundRtxTimeout (), 
                                      &HomaOutboundMsg::ExpireRtxTimeout, 
                                      this, m_maxGrantedIdx);
  }
  else
  {
    NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () << 
                " HomaOutboundMsg (" << this << ") has timed-out.");
    m_isExpired = true;
  }
}
    
/******************************************************************************/
    
const uint16_t HomaSendScheduler::MAX_N_MSG = 2560;

// 注册并返回 HomaSendScheduler 的 TypeId。
TypeId HomaSendScheduler::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaSendScheduler")
    .SetParent<Object> ()
    .SetGroupName("Internet")
  ;
  return tid;
}
    
// 构造发送调度器并初始化可分配 txMsgId 自由列表。
HomaSendScheduler::HomaSendScheduler (Ptr<HomaL4Protocol> homaL4Protocol)
{
  NS_LOG_FUNCTION (this);
      
  m_homa = homaL4Protocol;
  
  // Initially, all the txMsgId values between 0 and MAX_N_MSG are listed as free
  m_txMsgIdFreeList.resize(MAX_N_MSG);
  std::iota(m_txMsgIdFreeList.begin(), m_txMsgIdFreeList.end(), 0);
}

// 析构时检查是否仍有未完成消息，便于发现发送流程异常。
HomaSendScheduler::~HomaSendScheduler ()
{
  NS_LOG_FUNCTION_NOARGS ();
    
  int numIncmpltMsg = m_outboundMsgs.size();
  if (numIncmpltMsg > 0)
  {
    NS_LOG_ERROR("ERROR: HomaSendScheduler (" << this <<
                 ") couldn't completely deliver " << 
                 numIncmpltMsg << " outbound messages!");
  }
}

/*
 * This method is called upon receiving a new message from the application layer.
 * It inserts the message into the list of pending outbound messages and updates
 * the scheduler's state accordingly.
 */
// 接收新消息并分配 txMsgId；若发送器空闲则安排首次发送事件。
int HomaSendScheduler::ScheduleNewMsg (Ptr<HomaOutboundMsg> outMsg)
{
  NS_LOG_FUNCTION (this << outMsg);
    
  uint16_t txMsgId;
  if (m_txMsgIdFreeList.size() > 0)
  {
    // Assign a unique txMsgId which will persist while the message lasts
    txMsgId = m_txMsgIdFreeList.front ();
    NS_LOG_LOGIC("HomaSendScheduler allocating txMsgId: " << txMsgId);
    m_txMsgIdFreeList.pop_front ();
      
    m_outboundMsgs[txMsgId] = outMsg;
      
    /* 
     * HomaSendScheduler can send a packet if it hasn't done so
     * recently. Otwerwise a txEvent should already been scheduled
     * which iterates over m_outboundMsgs to decide which packet 
     * to send next. 
     */
    if(m_txEvent.IsExpired()) 
      m_txEvent = Simulator::Schedule (m_homa->GetTimeToDrainTxQueue(), 
                                       &HomaSendScheduler::TxDataPacket, this);
  }
  else
  {
    NS_LOG_ERROR(Simulator::Now ().GetNanoSeconds () << 
                 " Error: HomaSendScheduler ("<< this << 
                 ") could not allocate a new txMsgId for message (" << 
                 outMsg << ")" );
    return -1;
  }
  return (int)txMsgId;
}
   
/*
 * This method determines the txMsgId of the highest priority 
 * message that is ready to send some packets into the network.
 * See the nested if statements for the algorithm to choose 
 * highest priority outbound message.
 */
// 在所有活跃消息中选择下一条最应优先发送的消息，并清理已过期状态。
bool HomaSendScheduler::GetNextMsgId (uint16_t &txMsgId)
{
  NS_LOG_FUNCTION (this);
  
  Ptr<HomaOutboundMsg> currentMsg;
  Ptr<HomaOutboundMsg> candidateMsg;
  std::list<uint16_t> expiredMsgIds;
  uint32_t minRemainingBytes = std::numeric_limits<uint32_t>::max();
  uint16_t pktOffset;
  bool msgSelected = false;
  /*
   * Iterate over all pending outbound messages and select the one 
   * that has the smallest remainingBytes, granted but not transmitted 
   * packets, and a receiver that is not listed as busy.
   */
  for (auto& it: m_outboundMsgs) 
  {
    currentMsg = it.second;
      
    if (!currentMsg->IsExpired ())
    { 
      uint32_t curRemainingBytes = currentMsg->GetRemainingBytes();
      // Accept current msg if the remainig size is smaller than minRemainingBytes
      if (curRemainingBytes < minRemainingBytes)
      {
        // Accept current msg if it has a sendable data packet or needs a
        // zero-payload initial credit request for long SIRD messages.
        if (currentMsg->NeedsInitialCreditRequest () ||
            currentMsg->GetNextPktOffset(pktOffset))
        {
          candidateMsg = currentMsg;
          txMsgId = it.first;
          minRemainingBytes = curRemainingBytes;
          msgSelected = true;
        }
      }
    }
    else // Expired messages should be removed from the state
    {
      expiredMsgIds.push_back(it.first); 
    }
  }
    
  /*
   * We only record the txMsgId of the expired msgs above and only remove them
   * after the for loop above finishes because the ClearStateForMsg function
   * also iterates over m_outboundMsgs and erases an element which may cause the
   * iterator to skip elements in the for loop above.
   */
  for (const auto& expiredTxMsgId : expiredMsgIds)
  {
    this->ClearStateForMsg (expiredTxMsgId);
  }
    
  return msgSelected;
}
    
// 为指定消息构造下一待发分组（可能是信用请求或真实 DATA）。
bool HomaSendScheduler::GetNextPktOfMsg (uint16_t txMsgId, Ptr<Packet> &p)
{
  NS_LOG_FUNCTION (this << txMsgId);
    
  uint16_t pktOffset;
  Ptr<HomaOutboundMsg> candidateMsg = m_outboundMsgs[txMsgId];

  if (candidateMsg->NeedsInitialCreditRequest ())
  {
    p = candidateMsg->GenerateInitialCreditRequest (txMsgId);
    candidateMsg->MarkInitialCreditRequestSent ();
    return true;
  }
    
  uint32_t accumulatedCreditPkts = this->GetAccumulatedCreditPkts ();
  if (candidateMsg->GetNextPktOffset(pktOffset))
  {
    p = candidateMsg->RemoveNextPktFromTxQ(pktOffset);
      
    HomaHeader homaHeader;
    homaHeader.SetDstPort (candidateMsg->GetDstPort ());
    homaHeader.SetSrcPort (candidateMsg->GetSrcPort ());
    homaHeader.SetTxMsgId (txMsgId);
    homaHeader.SetFlags (HomaHeader::Flags_t::DATA); 
    homaHeader.SetMsgSize (candidateMsg->GetMsgSizeBytes ());
    homaHeader.SetPktOffset (pktOffset);
    homaHeader.SetGrantOffset (candidateMsg->GetMaxGrantedIdx ()); // For monitoring purposes
    homaHeader.SetPayloadSize (p->GetSize ());
    if (m_homa->IsSirdEnabled () && accumulatedCreditPkts > m_homa->GetSirdSenderCsnThreshold ())
    {
      homaHeader.SetFeedbackFlags (HomaHeader::FeedbackFlags_t::FEEDBACK_CSN);
    }
    else
    {
      homaHeader.SetFeedbackFlags (HomaHeader::FeedbackFlags_t::FEEDBACK_NONE);
    }
    
    // NOTE: Use the following SocketIpTosTag append strategy when 
    //       sending packets out. This allows us to set the priority
    //       of the packets correctly for the PfifoHomaQueueDisc way of 
    //       priority queueing in the network.
    SocketIpTosTag ipTosTag;
    uint8_t dataPrio = candidateMsg->GetPrio (pktOffset);
    uint8_t ecn = m_homa->IsSirdEnabled () ? static_cast<uint8_t> (Ipv4Header::ECN_ECT0)
                         : static_cast<uint8_t> (Ipv4Header::ECN_NotECT);
    ipTosTag.SetTos (static_cast<uint8_t> ((dataPrio << 2) | ecn));
    // This packet may already have a SocketIpTosTag (see HomaSocket)
    p->ReplacePacketTag (ipTosTag);
      
    /*
     * The priority of packets are actually carried on the packet tags as
     * shown above. The priority field on the homaHeader field is actually
     * used by control packets to signal the requested priority from receivers
     * to the senders, so that they can set their data packet priorities 
     * accordingly.
     *
     * Setting the priority field on a data packet is just for monitoring reasons.
     */
    homaHeader.SetPrio (candidateMsg->GetPrio (pktOffset));
      
    p->AddHeader (homaHeader);
    NS_LOG_DEBUG (Simulator::Now ().GetNanoSeconds () << 
                  " HomaL4Protocol sending: " << p->ToString ());

    if (m_homa->IsSirdEnabled ())
    {
      candidateMsg->ConsumeSenderCredit ();
      m_homa->TraceSirdSenderCreditState (candidateMsg->GetSrcAddress (),
                                          candidateMsg->GetDstAddress (),
                                          txMsgId,
                                          candidateMsg->GetAccumulatedCreditPkts (),
                                          2);
    }
    
    return true;
  }
  else
  {
    return false;
  }
}

uint32_t
HomaSendScheduler::GetAccumulatedCreditPkts (void) const
{
  uint32_t accumulatedCreditPkts = 0;
  for (const auto& kv : m_outboundMsgs)
    {
      accumulatedCreditPkts += kv.second->GetAccumulatedCreditPkts ();
    }
  return accumulatedCreditPkts;
}
 
/*
 * This method is called either when a new packet to send is found after
 * an idle time period or when the serialization of the previous packets 
 * finish. This allows HomaSendScheduler to choose the most recent highest 
 * priority packet just before sending it.
 */
void
// 发送调度主循环：按链路空闲时刻触发，选包并下发后继续自调度。
HomaSendScheduler::TxDataPacket ()
{
  NS_LOG_FUNCTION (this);
    
  NS_ASSERT(m_txEvent.IsExpired());
    
  Time timeToDrainTxQ = m_homa->GetTimeToDrainTxQueue();
  if (timeToDrainTxQ != Time(0))
  {
    m_txEvent = Simulator::Schedule (timeToDrainTxQ, 
                                     &HomaSendScheduler::TxDataPacket, this);
    return;
  }
    
  uint16_t nextTxMsgID;
  Ptr<Packet> p;
  if (this->GetNextMsgId (nextTxMsgID))
  {   
    bool gotPkt = this->GetNextPktOfMsg(nextTxMsgID, p);
    NS_ASSERT(gotPkt);
    if (!gotPkt)
    {
      NS_LOG_LOGIC("HomaSendScheduler selected msg " << nextTxMsgID
                   << " but no packet was available.");
      return;
    }
      
    NS_LOG_LOGIC("HomaSendScheduler (" << this <<
                  ") will transmit a packet from msg " << nextTxMsgID);
    
    m_homa->SendDown(p, 
                     m_outboundMsgs[nextTxMsgID]->GetSrcAddress (), 
                     m_outboundMsgs[nextTxMsgID]->GetDstAddress (), 
                     m_outboundMsgs[nextTxMsgID]->GetRoute ());
    
    m_txEvent = Simulator::Schedule (m_homa->GetTimeToDrainTxQueue(), 
                                     &HomaSendScheduler::TxDataPacket, this);
  }
  else
  {
    NS_LOG_LOGIC("HomaSendScheduler doesn't have any packet to send!");
  }
}
   
/*
 * This method is called when a control packet is received that interests
 * an outbound message.
 */
// 处理发送侧关注的控制包（GRANT/RESEND/ACK），并驱动状态机前进。
void HomaSendScheduler::CtrlPktRecvdForOutboundMsg(Ipv4Header const &ipv4Header, 
                                                   HomaHeader const &homaHeader)
{
  NS_LOG_FUNCTION (this << ipv4Header << homaHeader);
    
  uint16_t targetTxMsgId = homaHeader.GetTxMsgId();
  if(m_outboundMsgs.find(targetTxMsgId) == m_outboundMsgs.end())
  {
    NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () <<
                " HomaSendScheduler (" << this <<
                ") received a " << homaHeader.FlagsToString(homaHeader.GetFlags()) << 
                " packet for an unknown txMsgId (" << 
                targetTxMsgId << ").");
    return;
  }
    
  if (m_outboundMsgs[targetTxMsgId]->IsExpired ())
  {
    NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () <<
                " HomaSendScheduler (" << this <<
                ") received a " << homaHeader.FlagsToString(homaHeader.GetFlags()) << 
                " packet for an expired txMsgId (" << 
                targetTxMsgId << ").");
    this->ClearStateForMsg (targetTxMsgId);
    return;
  }
    
  Ptr<HomaOutboundMsg> targetMsg = m_outboundMsgs[targetTxMsgId];
  // Verify that the TxMsgId indeed matches the 4 tuple
  NS_ASSERT( (targetMsg->GetSrcAddress() == ipv4Header.GetDestination ()) &&
             (targetMsg->GetDstAddress() == ipv4Header.GetSource ()) && 
             (targetMsg->GetSrcPort() == homaHeader.GetDstPort ()) &&
             (targetMsg->GetDstPort() == homaHeader.GetSrcPort ()) );
  
  uint8_t ctrlFlag = homaHeader.GetFlags();
  if (ctrlFlag & HomaHeader::Flags_t::GRANT)
  {
    targetMsg->HandleGrantOffset (homaHeader);
    m_homa->TraceSirdSenderCreditState (targetMsg->GetSrcAddress (),
                                        targetMsg->GetDstAddress (),
                                        targetTxMsgId,
                                        targetMsg->GetAccumulatedCreditPkts (),
                                        1);
  }
  else if (ctrlFlag & HomaHeader::Flags_t::RESEND)
  {
    targetMsg->HandleGrantOffset (homaHeader);
    m_homa->TraceSirdSenderCreditState (targetMsg->GetSrcAddress (),
                                        targetMsg->GetDstAddress (),
                                        targetTxMsgId,
                                        targetMsg->GetAccumulatedCreditPkts (),
                                        1);
    targetMsg->HandleResend (homaHeader);
      
    uint16_t nextTxMsgID;
    this->GetNextMsgId (nextTxMsgID);
    if (nextTxMsgID != targetTxMsgId) 
    {
      // Incoming packet doesn't belong to the highest priority outboung message.
      NS_LOG_LOGIC("HomaSendScheduler (" << this 
                   << ") needs to send a BUSY packet for " << targetTxMsgId);
      
      m_homa->SendDown(targetMsg->GenerateBusy (nextTxMsgID), 
                       targetMsg->GetSrcAddress (), 
                       targetMsg->GetDstAddress (), 
                       targetMsg->GetRoute ());
    }
  }
  else if (ctrlFlag & HomaHeader::Flags_t::ACK)
  {
    NS_LOG_LOGIC("The HomaOutboundMsg (" << targetMsg << ") is fully delivered!");
    
    targetMsg->HandleAck (homaHeader); // Asserts some sanity checks.
    m_homa->TraceSirdSenderCreditState (targetMsg->GetSrcAddress (),
                                        targetMsg->GetDstAddress (),
                                        targetTxMsgId,
                                        0,
                                        3);
    this->ClearStateForMsg (targetTxMsgId);
  }
  else
  {
    NS_LOG_ERROR("ERROR: HomaSendScheduler (" << this 
                 << ") has received an unexpected control packet ("
                 << homaHeader.FlagsToString(ctrlFlag) << ")");
      
    return;
  }
    
  /* 
   * Since control packets may allow new packets to be sent, we should try 
   * to transmit those packets.
   */
  if(m_txEvent.IsExpired()) 
    m_txEvent = Simulator::Schedule (m_homa->GetTimeToDrainTxQueue(), 
                                     &HomaSendScheduler::TxDataPacket, this);
}
    
// 清理完成或失效消息的发送状态，回收 txMsgId。
void HomaSendScheduler::ClearStateForMsg (uint16_t txMsgId)
{
  NS_LOG_FUNCTION(this << txMsgId);

  Ptr<HomaOutboundMsg> outMsg = m_outboundMsgs[txMsgId];
  std::string key = MakePathRttKey (outMsg->GetSrcAddress (),
                                    outMsg->GetDstAddress (),
                                    outMsg->GetSrcPort (),
                                    outMsg->GetDstPort (),
                                    txMsgId);
  g_pathRttFlowStates.erase (key);

  Simulator::Cancel (outMsg->GetRtxEvent ());
  m_outboundMsgs.erase(m_outboundMsgs.find(txMsgId));
  m_txMsgIdFreeList.push_back(txMsgId);
}
    
/******************************************************************************/

// 注册并返回 HomaInboundMsg 的 TypeId。
TypeId HomaInboundMsg::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaInboundMsg")
    .SetParent<Object> ()
    .SetGroupName("Internet")
  ;
  return tid;
}

/*
 * This method creates a new inbound message with the given information.
 */
// 构造接收侧消息状态：初始化分片缓冲、授权窗口和重传相关计数。
HomaInboundMsg::HomaInboundMsg (Ptr<Packet> p,
                                Ipv4Header const &ipv4Header, HomaHeader const &homaHeader, 
                                Ptr<Ipv4Interface> iface, uint32_t mtuBytes, 
                                uint16_t rttPackets, bool memIsOptimized,
                                bool sirdEnabled, uint16_t sirdUnschThresholdPkts)
    : m_prio(0),
      m_hasGrantedData(false),
      m_creditDrivenGrantWindow(false),
      m_currentlyScheduled(false),
      m_numRtxWithoutProgress (0)
{
  NS_LOG_FUNCTION (this);

  m_ipv4Header = ipv4Header;
  m_iface = iface;
    
  m_sport = homaHeader.GetSrcPort ();
  m_dport = homaHeader.GetDstPort ();
  m_txMsgId = homaHeader.GetTxMsgId ();
   
  m_msgSizeBytes = homaHeader.GetMsgSize ();
  uint32_t maxPayloadSize = mtuBytes - homaHeader.GetSerializedSize () - ipv4Header.GetSerializedSize ();
  m_msgSizePkts = m_msgSizeBytes / maxPayloadSize + (m_msgSizeBytes % maxPayloadSize != 0);
          
  // The remaining undelivered message size equals to total minus actually received payload.
  m_remainingBytes = m_msgSizeBytes;
  
  // Fill in the packet buffer with place holder (empty) packets and set the received info as false
  for (uint16_t i = 0; i < m_msgSizePkts; i++)
  {
    if (memIsOptimized)
      m_pktSizes.push_back(0);
    else
      m_packets.push_back(Create<Packet> ());
      
    m_receivedPackets.push_back(false);
  } 
          
  uint16_t pktOffset = homaHeader.GetPktOffset ();
  bool hasPayload = p->GetSize () > 0;
  if (hasPayload)
  {
    if (memIsOptimized)
      m_pktSizes[pktOffset] = p->GetSize ();
    else
      m_packets[pktOffset] = p;
    m_receivedPackets[pktOffset] = true;
    m_remainingBytes -= p->GetSize ();
  }

  bool longSirdMsg = sirdEnabled && m_msgSizePkts > std::max<uint16_t> (1, sirdUnschThresholdPkts);
  if (longSirdMsg)
  {
    m_maxGrantedIdx = 0;
    m_maxGrantableIdx = 0;
    m_hasGrantedData = false;
    m_creditDrivenGrantWindow = true;
  }
  else
  {
    m_maxGrantedIdx = std::min<uint16_t> (rttPackets - 1, m_msgSizePkts - 1); // Unscheduled pkts are already granted
    m_maxGrantableIdx = m_maxGrantedIdx + 1; // 1 Data pkt is already received
    m_hasGrantedData = true;
  }
  m_lastRtxGrntIdx = m_maxGrantableIdx;
}

// 析构函数：当前无额外资源回收逻辑。
HomaInboundMsg::~HomaInboundMsg ()
{
  NS_LOG_FUNCTION_NOARGS ();
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint32_t HomaInboundMsg::GetRemainingBytes()
{
  return m_remainingBytes;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ipv4Address HomaInboundMsg::GetSrcAddress ()
{
  return m_ipv4Header.GetSource ();
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ipv4Address HomaInboundMsg::GetDstAddress ()
{
  return m_ipv4Header.GetDestination ();
}

// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetSrcPort ()
{
  return m_sport;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetDstPort ()
{
  return m_dport;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetTxMsgId ()
{
  return m_txMsgId;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ipv4Header HomaInboundMsg::GetIpv4Header ()
{
  return m_ipv4Header;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ptr<Ipv4Interface> HomaInboundMsg::GetIpv4Interface ()
{
  return m_iface;
}

/*
 * Although the retransmission events are handled by the HomaRecvScheduler
 * the corresponding EventId of messages are kept within the messages 
 * themselves for the sake of being tidy.
 */
// 把重传定时器事件句柄挂在消息对象上，便于调度器统一管理。
void HomaInboundMsg::SetRtxEvent (EventId rtxEvent)
{
  m_rtxEvent = rtxEvent;
}
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
EventId HomaInboundMsg::GetRtxEvent ()
{
  return m_rtxEvent;
}

// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetMaxGrantableIdx ()
{
  return m_maxGrantableIdx;
}

// 限制本轮最多可继续授权的窗口大小，用于精确控制 SIRD 授权额度。
void HomaInboundMsg::CapGrantableWindow (uint16_t grantWindowPkts)
{
  if (m_msgSizePkts == 0)
  {
    return;
  }

  uint16_t cappedWindow = std::max<uint16_t> (1, grantWindowPkts);
  uint32_t baseGrantable = m_hasGrantedData ? m_maxGrantedIdx + cappedWindow : cappedWindow - 1;
  uint32_t cappedMaxGrantable = std::min<uint32_t> (m_msgSizePkts - 1, baseGrantable);
  m_maxGrantableIdx = std::min<uint16_t> (m_maxGrantableIdx, static_cast<uint16_t> (cappedMaxGrantable));
}

bool HomaInboundMsg::AdvanceGrantableWindow (uint16_t grantPkts)
{
  if (m_msgSizePkts == 0 || this->IsFullyGranted ())
  {
    return false;
  }

  uint16_t creditPkts = std::max<uint16_t> (1, grantPkts);
  uint32_t baseGrantable = m_hasGrantedData ? m_maxGrantedIdx + creditPkts : creditPkts - 1;
  uint32_t advancedMaxGrantable = std::min<uint32_t> (m_msgSizePkts - 1, baseGrantable);
  if (advancedMaxGrantable <= m_maxGrantableIdx)
  {
    return false;
  }

  m_maxGrantableIdx = static_cast<uint16_t> (advancedMaxGrantable);
  return true;
}

// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetMaxGrantedIdx ()
{
  return m_maxGrantedIdx;
}
 
// 写入并更新当前对象的配置或运行时状态，为后续流程提供输入。
void HomaInboundMsg::SetLastRtxGrntIdx (uint16_t lastRtxGrntIdx)
{
  m_lastRtxGrntIdx = lastRtxGrntIdx;
}
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetLastRtxGrntIdx ()
{
  return m_lastRtxGrntIdx;
}
    
// 判断该消息是否已经被授权到最后一个分片。
bool HomaInboundMsg::IsFullyGranted ()
{
  if (!m_hasGrantedData)
  {
    return false;
  }
  return m_maxGrantedIdx >= m_msgSizePkts-1;
}

// 判断当前是否还能继续发送 GRANT（已授权上界尚未追上可授权上界）。
bool HomaInboundMsg::IsGrantable ()
{
  if (!m_hasGrantedData)
  {
    return m_msgSizePkts > 0 && m_maxGrantableIdx < m_msgSizePkts;
  }
  return m_maxGrantedIdx < m_maxGrantableIdx;
}
    
// 判断消息所有分片是否都已收到。
bool HomaInboundMsg::IsFullyReceived ()
{
  // 该函数实现当前类的一个核心步骤，结合调用链可理解其在状态机中的角色。
  return std::none_of(m_receivedPackets.begin(), 
                      m_receivedPackets.end(), 
                      std::logical_not<bool>());
}

// 根据当前状态进行条件判断，返回布尔决策供上层流程分支使用。
bool HomaInboundMsg::IsCurrentlyScheduled (void) 
{
  return m_currentlyScheduled;
}

// 写入并更新当前对象的配置或运行时状态，为后续流程提供输入。
void HomaInboundMsg::SetCurrentlyScheduled (bool currentlyScheduled) 
{
  m_currentlyScheduled = currentlyScheduled;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetNumRtxWithoutProgress ()
{
  return m_numRtxWithoutProgress;
}
// 该函数实现当前类的一个核心步骤，结合调用链可理解其在状态机中的角色。
void HomaInboundMsg::IncrNumRtxWithoutProgress ()
{
  m_numRtxWithoutProgress++;
}
// 该函数实现当前类的一个核心步骤，结合调用链可理解其在状态机中的角色。
void HomaInboundMsg::ResetNumRtxWithoutProgress ()
{
  m_numRtxWithoutProgress = 0;
}
    
/*
 * This method updates the state for an inbound message upon receival of a data packet.
 */
// 接收并登记一个数据分片，推进可授权窗口，并更新剩余字节。
void HomaInboundMsg::ReceiveDataPacket (Ptr<Packet> p, uint16_t pktOffset)
{
  NS_LOG_FUNCTION (this << p << pktOffset);

  if (p->GetSize () == 0)
  {
    // Zero-payload DATA is a credit request for long scheduled messages.
    return;
  }
    
  if (!m_receivedPackets[pktOffset])
  {
    if (m_pktSizes.size())
      m_pktSizes[pktOffset] = p->GetSize ();
    else
      m_packets[pktOffset] = p;
    m_receivedPackets[pktOffset] = true;
      
    m_remainingBytes -= p->GetSize ();
    /*
     * Since a packet has arrived, we can allow a new packet to be on flight
     * for this message, so that bytes in flight stay the same. However it 
     * is upto the HomaRecvScheduler to decide whether to send a Grant packet 
     * to the sender of this message or not.
     */
    if (m_creditDrivenGrantWindow || m_maxGrantableIdx >= m_msgSizePkts - 1)
    {
      return;
    }

    m_maxGrantableIdx++;
  }
  else
  {
    NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () <<
                " HomaInboundMsg (" << this << ") has received a packet for offset "
                << pktOffset << " which was already received.");
    // TODO: Insert a trace source to keep track of spurious retransmissions.
  }
}

// 在消息完整后执行重组，返回可直接上交应用层的完整 Packet。
Ptr<Packet> HomaInboundMsg::GetReassembledMsg ()
{
  NS_LOG_FUNCTION (this);
    
  if (m_pktSizes.size())
  {
    uint32_t msgSize = 0;
    for (std::size_t i = 0; i < m_msgSizePkts; i++)
    {
      NS_ASSERT_MSG(m_receivedPackets[i],
                    "ERROR: HomaRecvScheduler is trying to reassemble an incomplete msg!");
      msgSize += m_pktSizes[i];
    }
      
    return Create<Packet> (msgSize);
  }
  else
  {
    Ptr<Packet> completeMsg = Create<Packet> ();
    for (std::size_t i = 0; i < m_msgSizePkts; i++)
    {
      NS_ASSERT_MSG(m_receivedPackets[i],
                    "ERROR: HomaRecvScheduler is trying to reassemble an incomplete msg!");
      completeMsg->AddAtEnd (m_packets[i]);
    }
  
    return completeMsg;
  }
}
    
// 根据当前接收进度生成 GRANT 或 ACK 控制包，并同步授权边界。
Ptr<Packet> HomaInboundMsg::GenerateGrantOrAck(uint8_t grantedPrio,
                                               uint8_t pktTypeFlag)
{
  NS_LOG_FUNCTION (this << grantedPrio);
  NS_ASSERT_MSG((pktTypeFlag & HomaHeader::Flags_t::GRANT) || 
                (pktTypeFlag & HomaHeader::Flags_t::ACK),
                "GenerateGrantOrAck() can only be called to generate GRANT or ACK packets!");
  bool isAck = (pktTypeFlag & HomaHeader::Flags_t::ACK) != 0;
  NS_ASSERT_MSG(isAck || this->IsGrantable(),
                "GenerateGrantOrAck() can only generate GRANT for a grantable message!");
    
  m_prio = grantedPrio; // Updated with the most recent granted priority value
    
  uint16_t ackNo = m_msgSizePkts;
  for (std::size_t i = 0; i < m_msgSizePkts; i++)
  {
    if (!m_receivedPackets[i])
    {
      ackNo = i; // The earliest un-received packet
      break;
    }
  }
    
  HomaHeader homaHeader;
  // Note we swap the src and dst port numbers for reverse direction
  homaHeader.SetSrcPort (m_dport); 
  homaHeader.SetDstPort (m_sport);
  homaHeader.SetTxMsgId (m_txMsgId);
  homaHeader.SetMsgSize (m_msgSizeBytes);
  homaHeader.SetPktOffset (ackNo);
  uint16_t grantOffset = m_msgSizePkts == 0 ? 0 : std::min<uint16_t> (m_maxGrantableIdx, m_msgSizePkts - 1);
  homaHeader.SetGrantOffset (grantOffset);
  homaHeader.SetPrio (m_prio);
  homaHeader.SetPayloadSize (0);
  homaHeader.SetFlags (pktTypeFlag);
  
  Ptr<Packet> p = Create<Packet> ();
  p->AddHeader (homaHeader);
    
  SocketIpTosTag ipTosTag;
  ipTosTag.SetTos (0); // Grant packets have the highest priority
  // This packet may already have a SocketIpTosTag (see HomaSocket)
  p->ReplacePacketTag (ipTosTag);
    
  if (!isAck)
  {
    m_maxGrantedIdx = grantOffset;
    m_hasGrantedData = true;
  }
    
  return p;
}
    
// 扫描缺失分片并批量生成 RESEND 控制包列表。
std::list<Ptr<Packet>> HomaInboundMsg::GenerateResends (uint16_t maxRsndPktOffset)
{
  NS_LOG_FUNCTION (this << maxRsndPktOffset);
    
  std::list<Ptr<Packet>> rsndPkts;
    
  maxRsndPktOffset = std::min(maxRsndPktOffset, m_msgSizePkts);
  if (!m_hasGrantedData)
  {
    return rsndPkts;
  }
  maxRsndPktOffset = std::min(maxRsndPktOffset, m_maxGrantedIdx);
  for (uint16_t i = 0; i < maxRsndPktOffset; ++i)
  {
    if(!m_receivedPackets[i])
    {
      HomaHeader homaHeader;
      // Note we swap the src and dst port numbers for reverse direction
      homaHeader.SetSrcPort (m_dport); 
      homaHeader.SetDstPort (m_sport);
      homaHeader.SetTxMsgId (m_txMsgId);
      homaHeader.SetMsgSize (m_msgSizeBytes);
      homaHeader.SetPktOffset (i); 
      homaHeader.SetGrantOffset (m_maxGrantedIdx);
      homaHeader.SetPrio (m_prio);
      homaHeader.SetPayloadSize (0);
      homaHeader.SetFlags (HomaHeader::Flags_t::RESEND);
  
      Ptr<Packet> p = Create<Packet> ();
      p->AddHeader (homaHeader);
    
      SocketIpTosTag ipTosTag;
      ipTosTag.SetTos (0); // Resend packets have the highest priority
      // This packet may already have a SocketIpTosTag (see HomaSocket)
      p->ReplacePacketTag (ipTosTag);
        
      rsndPkts.push_back(p);
    }
  }
  
    return rsndPkts;
}
    
/******************************************************************************/

// 注册并返回 HomaRecvScheduler 的 TypeId。
TypeId HomaRecvScheduler::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaRecvScheduler")
    .SetParent<Object> ()
    .SetGroupName("Internet")
  ;
  return tid;
}
    
// 构造接收调度器并初始化 SIRD 相关全局/每发送方统计状态。
HomaRecvScheduler::HomaRecvScheduler (Ptr<HomaL4Protocol> homaL4Protocol)
{
  NS_LOG_FUNCTION (this);
      
  m_homa = homaL4Protocol;
  m_sirdGlobalCreditsInUsePkts = 0;
  m_sirdCeRatioEwma = 0.0;
  m_sirdDataPktsObserved = 0;
  m_sirdCeMarksObserved = 0;
  m_creditTickEvent = EventId ();
}

// 析构时检查是否仍有未完成入站消息，辅助排查接收流程问题。
HomaRecvScheduler::~HomaRecvScheduler ()
{
  NS_LOG_FUNCTION_NOARGS ();
    
  int numIncmpltMsg = m_inboundMsgs.size();
  if (numIncmpltMsg > 0)
  {
    NS_LOG_ERROR("ERROR: HomaRecvScheduler (" << this <<
                 ") couldn't completely deliver " << 
                 numIncmpltMsg << " active inbound messages!");
  }
}
    
// 接收侧入口：处理 DATA/BUSY，并在每次到包后重新评估是否应发放授权。
void HomaRecvScheduler::ReceivePacket (Ptr<Packet> packet, 
                                       Ipv4Header const &ipv4Header,
                                       HomaHeader const &homaHeader,
                                       Ptr<Ipv4Interface> interface)
{
  NS_LOG_FUNCTION (this << packet << ipv4Header << homaHeader);
    
  uint8_t rxFlag = homaHeader.GetFlags ();//是 Data 还是 busy
  if (rxFlag & HomaHeader::Flags_t::DATA )
  {
    if (m_homa->IsSirdEnabled ())
    {
      const uint32_t senderKey = ipv4Header.GetSource ().Get ();//用 sender 的 ip 做 key
      const double baseBudget = static_cast<double> (std::max<uint16_t> (1, m_homa->GetBdp ()));
      auto clampBudget = [baseBudget](double budget) {
        return std::max (1.0, std::min (baseBudget, budget));
      };
      if (m_sirdSenderBudgetNetPkts.find (senderKey) == m_sirdSenderBudgetNetPkts.end ())
      {
        m_sirdSenderBudgetNetPkts[senderKey] = baseBudget;
      }
      if (m_sirdSenderBudgetHostPkts.find (senderKey) == m_sirdSenderBudgetHostPkts.end ())
      {
        m_sirdSenderBudgetHostPkts[senderKey] = baseBudget;
      }
      if (m_sirdSenderNetAlpha.find (senderKey) == m_sirdSenderNetAlpha.end ())
      {
        m_sirdSenderNetAlpha[senderKey] = 0.0;
      }
      if (m_sirdSenderHostAlpha.find (senderKey) == m_sirdSenderHostAlpha.end ())
      {
        m_sirdSenderHostAlpha[senderKey] = 0.0;
      }

      bool senderCsn = (homaHeader.GetFeedbackFlags () & HomaHeader::FeedbackFlags_t::FEEDBACK_CSN) != 0;
      m_sirdSenderCsnState[senderKey] = senderCsn;//从 Homa 头里的 feedbackFlags 读取 CSN 位，并更新这个 sender 最近一次的 CSN 状态
      bool ceMarked = ipv4Header.GetEcn () == Ipv4Header::ECN_CE;
      m_sirdSenderCeState[senderKey] = ceMarked;

      m_sirdDataPktsObserved++;//用于统计或平滑估计
      m_sirdSenderDataPktsObserved[senderKey]++;
      m_sirdSenderEpochDataPkts[senderKey]++;
      if (ceMarked)
      {
        m_sirdCeMarksObserved++;
        m_sirdSenderCeMarksObserved[senderKey]++;
        m_sirdSenderEpochCeMarks[senderKey]++;
      }
      if (senderCsn)
      {
        m_sirdSenderCsnMarksObserved[senderKey]++;
        m_sirdSenderEpochCsnMarks[senderKey]++;
      }
      double sampleCeRatio = ceMarked ? 1.0 : 0.0;
      double alpha = m_homa->GetSirdEcnAlphaGain ();
      m_sirdCeRatioEwma = (1.0 - alpha) * m_sirdCeRatioEwma + alpha * sampleCeRatio;
      double netBudgetPkts = m_sirdSenderBudgetNetPkts[senderKey];
      double hostBudgetPkts = m_sirdSenderBudgetHostPkts[senderKey];

      // Reclaim outstanding scheduled credits as data arrives.
      if (packet->GetSize () > 0)//是 Data 包，负载大于 0
      {
        auto inUseIt = m_sirdSenderCreditsInUsePkts.find (senderKey);
        if (inUseIt != m_sirdSenderCreditsInUsePkts.end () && inUseIt->second > 0)
        {
          inUseIt->second--;
          if (m_sirdGlobalCreditsInUsePkts > 0)
          {
            m_sirdGlobalCreditsInUsePkts--;
          }

          const uint32_t globalBudgetPkts = std::max<uint32_t> (1, m_homa->GetSirdCreditBudgetPkts ());
          uint32_t senderBudgetPkts = static_cast<uint32_t> (std::max (1.0, hostBudgetPkts));
          uint32_t senderAvailPkts =
            (senderBudgetPkts > inUseIt->second) ? (senderBudgetPkts - inUseIt->second) : 0;
          uint32_t globalAvailPkts =
            (globalBudgetPkts > m_sirdGlobalCreditsInUsePkts) ?
            (globalBudgetPkts - m_sirdGlobalCreditsInUsePkts) : 0;
          m_homa->TraceSirdReceiverCreditState (ipv4Header.GetDestination (),
                                                Ipv4Address (senderKey),
                                                globalAvailPkts,
                                                globalBudgetPkts,
                                                senderAvailPkts,
                                                senderBudgetPkts,
                                                2);
          m_homa->TraceSirdBucketState (ipv4Header.GetDestination (),
                                        Ipv4Address (senderKey),
                                        hostBudgetPkts,
                                        inUseIt->second,
                                        m_sirdGlobalCreditsInUsePkts,
                                        globalBudgetPkts,
                                        2);
        }
      }//credit 归还

      const uint64_t epochPkts = std::max<uint16_t> (1, m_homa->GetBdp ());
      uint64_t epochDataPkts = m_sirdSenderEpochDataPkts[senderKey];
      if (epochDataPkts >= epochPkts)
      {
        double ceFraction = static_cast<double> (m_sirdSenderEpochCeMarks[senderKey]) /
                            static_cast<double> (epochDataPkts);
        double csnFraction = static_cast<double> (m_sirdSenderEpochCsnMarks[senderKey]) /
                             static_cast<double> (epochDataPkts);

        double netAlpha = m_sirdSenderNetAlpha[senderKey];
        netAlpha = (1.0 - alpha) * netAlpha + alpha * ceFraction;
        m_sirdSenderNetAlpha[senderKey] = netAlpha;

        double hostAlpha = m_sirdSenderHostAlpha[senderKey];
        hostAlpha = (1.0 - alpha) * hostAlpha + alpha * csnFraction;
        m_sirdSenderHostAlpha[senderKey] = hostAlpha;

        if (m_sirdSenderEpochCeMarks[senderKey] > 0)
        {
          netBudgetPkts = clampBudget (netBudgetPkts * (1.0 - netAlpha / 2.0));
        }
        else
        {
          netBudgetPkts = clampBudget (netBudgetPkts + 1.0);
        }
        m_sirdSenderBudgetNetPkts[senderKey] = netBudgetPkts;

        if (m_sirdSenderEpochCsnMarks[senderKey] > 0)
        {
          hostBudgetPkts = clampBudget (hostBudgetPkts * (1.0 - hostAlpha / 2.0));
        }
        else
        {
          hostBudgetPkts = clampBudget (hostBudgetPkts + 1.0);
        }
        m_sirdSenderBudgetHostPkts[senderKey] = hostBudgetPkts;

        const uint32_t globalBudgetPkts = std::max<uint32_t> (1, m_homa->GetSirdCreditBudgetPkts ());
        double effectiveBudgetPkts = std::min (netBudgetPkts, hostBudgetPkts);
        uint32_t senderCreditsInUsePkts = 0;
        auto inUseIt = m_sirdSenderCreditsInUsePkts.find (senderKey);
        if (inUseIt != m_sirdSenderCreditsInUsePkts.end ())
        {
          senderCreditsInUsePkts = inUseIt->second;
        }
        uint64_t loopState =
          (static_cast<uint64_t> (ceMarked ? 1 : 0)) |
          (static_cast<uint64_t> (senderCsn ? 1 : 0) << 1) |
          (static_cast<uint64_t> (3u) << 2) |
          (static_cast<uint64_t> (std::min<uint32_t> (senderCreditsInUsePkts, 0xFFFFu)) << 8) |
          (static_cast<uint64_t> (std::min<uint32_t> (m_sirdGlobalCreditsInUsePkts, 0xFFFFu)) << 24) |
          (static_cast<uint64_t> (std::min<uint32_t> (globalBudgetPkts, 0xFFFFu)) << 40);
        uint64_t counterState =
          (static_cast<uint64_t> (std::min<uint64_t> (m_sirdSenderCeMarksObserved[senderKey], 0x1FFFFFu))) |
          (static_cast<uint64_t> (std::min<uint64_t> (m_sirdSenderCsnMarksObserved[senderKey], 0x1FFFFFu)) << 21) |
          (static_cast<uint64_t> (std::min<uint64_t> (m_sirdSenderDataPktsObserved[senderKey], 0x1FFFFFu)) << 42);
        m_homa->TraceSirdLoopState (ipv4Header.GetDestination (),
                                    Ipv4Address (senderKey),
                                    netBudgetPkts,
                                    hostBudgetPkts,
                                    effectiveBudgetPkts,
                                    netAlpha,
                                    loopState,
                                    counterState);

        m_sirdSenderEpochDataPkts[senderKey] = 0;
        m_sirdSenderEpochCeMarks[senderKey] = 0;
        m_sirdSenderEpochCsnMarks[senderKey] = 0;
      }
    }

    this->ReceiveDataPacket (packet, ipv4Header, homaHeader, interface);
    // Sender is not busy since it is able to send data packets
    m_busySenders.erase(ipv4Header.GetSource().Get ());
  }
  else if (rxFlag & HomaHeader::Flags_t::BUSY)
  {
    m_busySenders.insert(ipv4Header.GetSource().Get ());
    // TODO: Is there anything else to do with a BUSY packet?
  }

  const uint32_t senderKey = ipv4Header.GetSource ().Get ();
  uint32_t senderCreditsInUsePkts = 0;
  auto senderInUseIt = m_sirdSenderCreditsInUsePkts.find (senderKey);
  if (senderInUseIt != m_sirdSenderCreditsInUsePkts.end ())
  {
    senderCreditsInUsePkts = senderInUseIt->second;
  }
  uint8_t ecn = static_cast<uint8_t> (ipv4Header.GetEcn ());
  bool csn = (homaHeader.GetFeedbackFlags () & HomaHeader::FeedbackFlags_t::FEEDBACK_CSN) != 0;
  uint32_t senderCreditClamped = std::min<uint32_t> (senderCreditsInUsePkts, 0xFFFFu);
  uint32_t globalCreditClamped = std::min<uint32_t> (m_sirdGlobalCreditsInUsePkts, 0xFFFFu);
  uint32_t creditState = (globalCreditClamped << 16) | senderCreditClamped;
  m_homa->TraceSirdPacketState (ipv4Header.GetDestination (),
                                ipv4Header.GetSource (),
                                homaHeader.GetFlags (),
                                (static_cast<uint32_t> (homaHeader.GetTxMsgId ()) << 16) |
                                  static_cast<uint32_t> (homaHeader.GetPktOffset ()),
                                homaHeader.GetGrantOffset (),
                                ecn,
                                csn,
                                creditState);

  this->EnsureCreditTickScheduled (true);
}
    
// 把 DATA 分组归并到对应消息；若消息完成则上交，否则重排调度队列。
void HomaRecvScheduler::ReceiveDataPacket (Ptr<Packet> packet, 
                                           Ipv4Header const &ipv4Header,
                                           HomaHeader const &homaHeader,
                                           Ptr<Ipv4Interface> interface)
{
  NS_LOG_FUNCTION (this << ipv4Header << homaHeader);
    
  Ptr<Packet> cp = packet->Copy();
  Ptr<HomaInboundMsg> inboundMsg;
    
  int msgIdx = -1;
  if (this->GetInboundMsg(ipv4Header, homaHeader, msgIdx))
  {
    NS_ASSERT(msgIdx >= 0);
    inboundMsg = m_inboundMsgs[msgIdx];
    inboundMsg->ReceiveDataPacket (cp, homaHeader.GetPktOffset());
  }
  else
  {
    inboundMsg = CreateObject<HomaInboundMsg> (cp, ipv4Header, homaHeader, interface, 
                                               m_homa->GetMtu (), m_homa->GetBdp (), 
                                               m_homa->MemIsOptimized (), m_homa->IsSirdEnabled (),
                                               m_homa->GetSirdUnschThresholdPkts ());
    inboundMsg-> SetRtxEvent (Simulator::Schedule (m_homa->GetInboundRtxTimeout(), 
                                                   &HomaRecvScheduler::ExpireRtxTimeout, this, 
                                                   inboundMsg, inboundMsg->GetMaxGrantedIdx ()));
  }
    
  if (inboundMsg->IsFullyReceived ())
  {
    NS_LOG_LOGIC("HomaInboundMsg (" << inboundMsg <<
                 ") is fully received. Forwarding the message to applications.");
    this->ForwardUp (inboundMsg, msgIdx);
  }
  else
  {
    this->ScheduleMsgAtIdx(inboundMsg, msgIdx);
  }
}
    
// 按五元组+txMsgId 在活跃入站消息列表中查找目标消息。
bool HomaRecvScheduler::GetInboundMsg(Ipv4Header const &ipv4Header, 
                                      HomaHeader const &homaHeader,
                                      int &msgIdx)
{
  NS_LOG_FUNCTION (this << ipv4Header << homaHeader);
    
  for (std::size_t i = 0; i < m_inboundMsgs.size(); ++i) 
  {
    if (m_inboundMsgs[i]->GetSrcAddress() == ipv4Header.GetSource() &&
        m_inboundMsgs[i]->GetDstAddress() == ipv4Header.GetDestination() &&
        m_inboundMsgs[i]->GetSrcPort() == homaHeader.GetSrcPort() &&
        m_inboundMsgs[i]->GetDstPort() == homaHeader.GetDstPort() &&
        m_inboundMsgs[i]->GetTxMsgId() == homaHeader.GetTxMsgId())
    {
      NS_LOG_LOGIC("The HomaInboundMsg (" << m_inboundMsgs[i]
                   << ") is found among the pending messages.");
      msgIdx = i;
      return true;
    }
  }
  
  NS_LOG_LOGIC("Incoming packet doesn't belong to a pending inbound message.");
  return false;
}
    
// 将消息按策略（SRPT 或 SRR）插回活跃队列，维护接收调度顺序。
void HomaRecvScheduler::ScheduleMsgAtIdx(Ptr<HomaInboundMsg> inboundMsg,
                                         int msgIdx)
{
  NS_LOG_FUNCTION (this << inboundMsg << msgIdx);
    
  if (msgIdx >= 0)
  {
    NS_LOG_LOGIC("The HomaInboundMsg (" << inboundMsg 
                 << ") is already an active message. Reordering it.");
    
    // Make sure the activeMsgIdx matches inboundMsg
    Ptr<HomaInboundMsg> msgToReschedule = m_inboundMsgs[msgIdx];
    NS_ASSERT(msgToReschedule->GetSrcAddress() == inboundMsg->GetSrcAddress() &&
              msgToReschedule->GetDstAddress() == inboundMsg->GetDstAddress() &&
              msgToReschedule->GetSrcPort() == inboundMsg->GetSrcPort() &&
              msgToReschedule->GetDstPort() == inboundMsg->GetDstPort() &&
              msgToReschedule->GetTxMsgId() == inboundMsg->GetTxMsgId());
    
    m_inboundMsgs.erase(m_inboundMsgs.begin() + msgIdx);
  }

  // SRR-like mode: keep messages in FIFO order among active flows.
  if (m_homa->UseSrrScheduling ())
  {
    m_inboundMsgs.push_back (inboundMsg);
    return;
  }
  
  for(std::size_t i = 0; i < m_inboundMsgs.size(); ++i) 
  {
    if(inboundMsg->GetRemainingBytes () < m_inboundMsgs[i]->GetRemainingBytes())
    {
      m_inboundMsgs.insert(m_inboundMsgs.begin()+i, inboundMsg);
      return;
    }
  }
  // The remaining size of the inboundMsg is larger than all the active messages
  m_inboundMsgs.push_back(inboundMsg);
}
    
// 将完整消息上交 L4，并立即回送 ACK，然后清理该消息状态。
void HomaRecvScheduler::ForwardUp(Ptr<HomaInboundMsg> inboundMsg, int msgIdx)
{
  NS_LOG_FUNCTION (this << inboundMsg);
    
  m_homa->ForwardUp (inboundMsg->GetReassembledMsg(), 
                     inboundMsg->GetIpv4Header (),
                     inboundMsg->GetSrcPort(),
                     inboundMsg->GetDstPort(),
                     inboundMsg->GetTxMsgId(),
                     inboundMsg->GetIpv4Interface());
        
  m_homa->SendDown(inboundMsg->GenerateGrantOrAck(m_homa->GetNumUnschedPrioBands(), 
                                                  HomaHeader::Flags_t::ACK),
                   inboundMsg->GetDstAddress (),
                   inboundMsg->GetSrcAddress ());
    
  this->ClearStateForMsg (inboundMsg, msgIdx);
}
    
// 取消定时器并从活跃列表移除入站消息。
void HomaRecvScheduler::ClearStateForMsg(Ptr<HomaInboundMsg> inboundMsg, int msgIdx)
{
  NS_LOG_FUNCTION (this << inboundMsg << msgIdx);
  
  Simulator::Cancel (inboundMsg->GetRtxEvent ());
    
  if (msgIdx >= 0)
  {
    NS_ASSERT_MSG(m_inboundMsgs[msgIdx]->GetSrcAddress() == inboundMsg->GetSrcAddress() &&
                  m_inboundMsgs[msgIdx]->GetDstAddress() == inboundMsg->GetDstAddress() &&
                  m_inboundMsgs[msgIdx]->GetSrcPort() == inboundMsg->GetSrcPort() &&
                  m_inboundMsgs[msgIdx]->GetDstPort() == inboundMsg->GetDstPort() &&
                  m_inboundMsgs[msgIdx]->GetTxMsgId() == inboundMsg->GetTxMsgId(),
                  "State can not be cleared for HomaInboundMsg because the given msgIdx "
                  "is not consistent with the message itself!");
      
    NS_LOG_DEBUG("Erasing HomaInboundMsg (" << inboundMsg << 
                 ") from the pending messages list of HomaRecvScheduler (" << 
                 this << ").");
      
    m_inboundMsgs.erase(m_inboundMsgs.begin() + msgIdx);
  }
}

/*
 * This method is called everytime a packet (DATA or BUSY) is received because
 * both types of incoming packets may cause the reordering of active messages 
 * list which implies that we might need to send out a Grant.
 * The method loops over all the pending messages in the list of active messages
 * and checks whether they are grantable. If yes, an appropriate grant packet is 
 * generated and send down the networking stack. Although the method loops over
 * all the messages, ideally at most one message should be granted because other
 * messages should already be granted when they received packets for themselves.
 */
// 根据 overcommit、busy 状态与 SIRD 预算，选择消息并发送合适的 GRANT。
bool HomaRecvScheduler::SendAppropriateGrants()
{
  NS_LOG_FUNCTION (this);
  bool grantIssued = false;

  if (!m_homa->IsSirdEnabled ())
  {
    std::unordered_set<uint32_t> grantedSenders; // Same sender can't be granted for multiple msgs at once
    uint8_t grantingPrio = m_homa->GetNumUnschedPrioBands (); // Scheduled priorities start here
    uint8_t overcommitDue = m_homa->GetOvercommitLevel ();

    Ptr<HomaInboundMsg> currentMsg;
    for (std::size_t i = 0; i < m_inboundMsgs.size(); ++i)
    {
      currentMsg = m_inboundMsgs[i];
      currentMsg->SetCurrentlyScheduled(false);

	    if (overcommitDue > 0)
	      {
	        grantingPrio = std::min(grantingPrio, (uint8_t)(m_homa->GetNumTotalPrioBands()-1));


	        Ipv4Address senderAddress = currentMsg->GetSrcAddress ();
	        bool issuedThisRound = false;
	        if (!currentMsg->IsFullyGranted () &&
	            grantedSenders.find(senderAddress.Get ()) == grantedSenders.end())
	        {
	          if (m_busySenders.find(senderAddress.Get ()) == m_busySenders.end())
	          {
            if (currentMsg->IsGrantable ())
            {
	              m_homa->SendDown(currentMsg->GenerateGrantOrAck(grantingPrio,
	                                                              HomaHeader::Flags_t::GRANT),
	                              currentMsg->GetDstAddress (),
	                              senderAddress);
	              currentMsg->SetCurrentlyScheduled(true);
	              issuedThisRound = true;
	              grantIssued = true;
	            }
	          }
	          if (issuedThisRound)
	          {
	            grantedSenders.insert(senderAddress.Get ());
	            overcommitDue--;
	            grantingPrio++;
	          }
	        }
	      }
    }
    return grantIssued;
  }
    
  std::unordered_set<uint32_t> grantedSenders;
  uint8_t grantingPrio = m_homa->GetNumUnschedPrioBands ();
  uint8_t overcommitDue = m_homa->GetOvercommitLevel ();
  const uint32_t globalBudgetPkts = std::max<uint32_t> (1, m_homa->GetSirdCreditBudgetPkts ());
    
  Ptr<HomaInboundMsg> currentMsg;
  for (std::size_t i = 0; i < m_inboundMsgs.size(); ++i)
  {
    currentMsg = m_inboundMsgs[i];
    currentMsg->SetCurrentlyScheduled (false);

    if (overcommitDue > 0)
    {
      grantingPrio = std::min(grantingPrio, (uint8_t)(m_homa->GetNumTotalPrioBands() - 1));

      Ipv4Address senderAddress = currentMsg->GetSrcAddress ();
      uint32_t senderKey = senderAddress.Get ();
      if (!currentMsg->IsFullyGranted () &&
          grantedSenders.find(senderKey) == grantedSenders.end())
      {
        if (m_busySenders.find(senderKey) == m_busySenders.end())
        {
          double netBudget = static_cast<double> (std::max<uint16_t> (1, m_homa->GetBdp ()));
          auto netBudgetIt = m_sirdSenderBudgetNetPkts.find (senderKey);
          if (netBudgetIt != m_sirdSenderBudgetNetPkts.end ())
          {
            netBudget = netBudgetIt->second;
          }
          bool senderCsn = false;
          auto csnIt = m_sirdSenderCsnState.find (senderKey);
          if (csnIt != m_sirdSenderCsnState.end ())
          {
            senderCsn = csnIt->second;
          }

          double hostBudget = static_cast<double> (std::max<uint16_t> (1, m_homa->GetBdp ()));
          auto hostBudgetIt = m_sirdSenderBudgetHostPkts.find (senderKey);
          if (hostBudgetIt != m_sirdSenderBudgetHostPkts.end ())
          {
            hostBudget = hostBudgetIt->second;
          }

          double budget = std::min (netBudget, hostBudget);

          if (m_sirdSenderCreditsInUsePkts.find (senderKey) == m_sirdSenderCreditsInUsePkts.end ())
          {
            m_sirdSenderCreditsInUsePkts[senderKey] = 0;
          }

          uint32_t senderBudgetPkts = static_cast<uint32_t> (std::max (1.0, budget));
          uint32_t senderInUsePkts = m_sirdSenderCreditsInUsePkts[senderKey];
          uint32_t senderAvailPkts = (senderBudgetPkts > senderInUsePkts) ? (senderBudgetPkts - senderInUsePkts) : 0;
          uint32_t globalAvailPkts = (globalBudgetPkts > m_sirdGlobalCreditsInUsePkts) ?
                                     (globalBudgetPkts - m_sirdGlobalCreditsInUsePkts) : 0;
          bool issuedThisRound = false;

          // SIRD issues one MSS-sized credit per tick. The credit bucket, not
          // data arrival, should decide whether the next grant can be exposed.
          if (senderAvailPkts > 0 && globalAvailPkts > 0 && !currentMsg->IsGrantable ())
          {
            currentMsg->AdvanceGrantableWindow (1);
          }

          if (senderAvailPkts > 0 && globalAvailPkts > 0 && currentMsg->IsGrantable ())
          {
            Ptr<Packet> grantPkt = currentMsg->GenerateGrantOrAck(grantingPrio,
                                                                   HomaHeader::Flags_t::GRANT);
            HomaHeader traceHeader;
            grantPkt->PeekHeader (traceHeader);

            m_homa->TraceSirdGrantDecision (senderAddress,
                                            currentMsg->GetTxMsgId (),
                                            traceHeader.GetGrantOffset (),
                                            budget,
                                            m_sirdCeRatioEwma,
                                            senderCsn);

            m_homa->SendDown(grantPkt,
                            currentMsg->GetDstAddress (),
                            senderAddress);
            currentMsg->SetCurrentlyScheduled(true);
            m_sirdSenderCreditsInUsePkts[senderKey]++;
            m_sirdGlobalCreditsInUsePkts++;
            senderInUsePkts = m_sirdSenderCreditsInUsePkts[senderKey];
            issuedThisRound = true;
            grantIssued = true;
          }

	          senderAvailPkts = (senderBudgetPkts > senderInUsePkts) ? (senderBudgetPkts - senderInUsePkts) : 0;
	          globalAvailPkts = (globalBudgetPkts > m_sirdGlobalCreditsInUsePkts) ?
	                            (globalBudgetPkts - m_sirdGlobalCreditsInUsePkts) : 0;
	          m_homa->TraceSirdReceiverCreditState (currentMsg->GetDstAddress (),
	                                                senderAddress,
	                                                globalAvailPkts,
	                                                globalBudgetPkts,
	                                                senderAvailPkts,
	                                                senderBudgetPkts,
	                                                issuedThisRound ? 1 : 0);
	          m_homa->TraceSirdBucketState (currentMsg->GetDstAddress (),
	                                        senderAddress,
	                                        hostBudget,
	                                        m_sirdSenderCreditsInUsePkts[senderKey],
	                                        m_sirdGlobalCreditsInUsePkts,
	                                        globalBudgetPkts,
	                                        issuedThisRound ? 1 : 0);

	          if (issuedThisRound)
	          {
	            grantedSenders.insert(senderKey);
	            overcommitDue--;
	            grantingPrio++;
	          }
	        }
	      }
	    }
  }
  return grantIssued;
}

void
HomaRecvScheduler::EnsureCreditTickScheduled (bool immediate)
{
  if (m_creditTickEvent.IsRunning ())
    {
      return;
    }

  if (!HasGrantOpportunity ())
    {
      return;
    }

  Time delay = immediate ? Time (0) : GetCreditTickInterval ();
  m_creditTickEvent = Simulator::Schedule (delay, &HomaRecvScheduler::CreditTick, this);
}

void
HomaRecvScheduler::CreditTick (void)
{
  m_creditTickEvent = EventId ();

  bool issued = SendAppropriateGrants ();
  if (!issued)
    {
      return;
    }

  if (HasGrantOpportunity ())
    {
      m_creditTickEvent = Simulator::Schedule (GetCreditTickInterval (),
                                               &HomaRecvScheduler::CreditTick,
                                               this);
    }
}

bool
HomaRecvScheduler::HasGrantOpportunity (void) const
{
  const uint32_t globalBudgetPkts = std::max<uint32_t> (1, m_homa->GetSirdCreditBudgetPkts ());
  if (m_sirdGlobalCreditsInUsePkts >= globalBudgetPkts)
    {
      return false;
    }

  const double baseBudget = static_cast<double> (std::max<uint16_t> (1, m_homa->GetBdp ()));

  for (const auto& currentMsg : m_inboundMsgs)
    {
      uint32_t senderKey = currentMsg->GetSrcAddress ().Get ();
      if (currentMsg->IsFullyGranted ())
        {
          continue;
        }
      if (m_busySenders.find (senderKey) != m_busySenders.end ())
        {
          continue;
        }

      double netBudget = baseBudget;
      auto netIt = m_sirdSenderBudgetNetPkts.find (senderKey);
      if (netIt != m_sirdSenderBudgetNetPkts.end ())
        {
          netBudget = netIt->second;
        }

      double hostBudget = baseBudget;
      auto hostIt = m_sirdSenderBudgetHostPkts.find (senderKey);
      if (hostIt != m_sirdSenderBudgetHostPkts.end ())
        {
          hostBudget = hostIt->second;
        }

      double budget = std::min (netBudget, hostBudget);
      uint32_t senderBudgetPkts = static_cast<uint32_t> (std::max (1.0, budget));
      uint32_t senderInUsePkts = 0;
      auto inUseIt = m_sirdSenderCreditsInUsePkts.find (senderKey);
      if (inUseIt != m_sirdSenderCreditsInUsePkts.end ())
        {
          senderInUsePkts = inUseIt->second;
        }

      if (senderBudgetPkts > senderInUsePkts)
        {
          return true;
        }
    }
  return false;
}

Time
HomaRecvScheduler::GetCreditTickInterval (void) const
{
  return m_homa->GetFullDataPktTxTime ();
}
    
// 接收侧超时处理：必要时发 RESEND，重置定时器并更新无进展重传计数。
void HomaRecvScheduler::ExpireRtxTimeout(Ptr<HomaInboundMsg> inboundMsg,
                                         uint16_t maxRsndPktOffset)
{
  NS_LOG_FUNCTION (this << inboundMsg << maxRsndPktOffset);
    
  if (inboundMsg->IsFullyReceived ())
    return;
    
  // Create a dummy Homa Header for msg lookup
  HomaHeader homaHeader;
  homaHeader.SetSrcPort (inboundMsg->GetSrcPort ());
  homaHeader.SetDstPort (inboundMsg->GetDstPort ());
  homaHeader.SetTxMsgId (inboundMsg->GetTxMsgId ());
  int msgIdx = -1;
  if (this->GetInboundMsg(inboundMsg->GetIpv4Header (), homaHeader, msgIdx))
  {
    NS_ASSERT(msgIdx >= 0);
    if (inboundMsg->GetNumRtxWithoutProgress () >= m_homa->GetMaxNumRtxPerMsg())
    {
      NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () <<
                  " Rtx Limit has been reached for the inbound Msg (" 
                  << inboundMsg << ").");
      this->ClearStateForMsg (inboundMsg, msgIdx);
      return;
    }
      
    if (m_busySenders.find(inboundMsg->GetSrcAddress ().Get ()) == m_busySenders.end() &&
        inboundMsg->IsCurrentlyScheduled())
    {
      // We send RESEND packets only to non-busy senders with scheduled messages
      NS_LOG_LOGIC(Simulator::Now().GetNanoSeconds () << 
                   " Rtx Timer for an inbound Msg (" << inboundMsg << 
                   ") expired, which is scheduled. RESEND packets will be sent");
        
      std::list<Ptr<Packet>> rsndPkts = inboundMsg->GenerateResends (maxRsndPktOffset);
      while (!rsndPkts.empty())
      {
        m_homa->SendDown(rsndPkts.front(),
                         inboundMsg->GetDstAddress (),
                         inboundMsg->GetSrcAddress ());
        rsndPkts.pop_front();
      }
    }
      
    // Rechedule the next retransmission event for this message
    inboundMsg-> SetRtxEvent (Simulator::Schedule (m_homa->GetInboundRtxTimeout(), 
                                                   &HomaRecvScheduler::ExpireRtxTimeout, this, 
                                                   inboundMsg, inboundMsg->GetMaxGrantedIdx ()));
      
    // Update the LastRtxGrntIdx value of this message for the next timeout event
    uint16_t maxGrantableIdx = inboundMsg->GetMaxGrantableIdx ();
    if (inboundMsg->GetLastRtxGrntIdx () < maxGrantableIdx)
    {
      inboundMsg->ResetNumRtxWithoutProgress ();
    }
    else if (inboundMsg->IsCurrentlyScheduled())
    {
      inboundMsg->IncrNumRtxWithoutProgress ();
    }
    inboundMsg->SetLastRtxGrntIdx (maxGrantableIdx);
      
  }
  else
  {
    NS_LOG_DEBUG(Simulator::Now().GetNanoSeconds () << 
                 " Rtx Timer for an inbound Msg (" << inboundMsg << 
                 ") expired, which doesn't exist any more.");
  }  
  return;
}
    
} // namespace ns3
