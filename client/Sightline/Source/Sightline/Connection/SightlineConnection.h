// FSightlineConnection —— 传输层（谦卑对象）：FSocket 封装 + FRunnable 接收线程
// 职责：连接 / 发帧 / 收字节切帧 / 把完整帧投回游戏线程（AsyncTask——全客户端唯一跨线程门）
// 职责边界：不碰任何游戏对象、不认识 UWorld/UE 宏对象——纯字节搬运（依赖向平台）
#pragma once
#include "CoreMinimal.h"
#include "SightlineSendPort.h"
#include <deque>
#include "HAL/Runnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Connection/SightlineProtocol.h"
#include "Sightline.h"
#include "Delegates/Delegate.h"
#include "Async/Async.h"
#include <atomic>

/** 收到的帧投递到游戏线程后的回调（在游戏线程执行） */
DECLARE_DELEGATE_TwoParams(FOnFrame, uint16 /*MsgId*/, const TArray<uint8>& /*Payload*/);
DECLARE_DELEGATE_OneParam(FOnDisconnected, const FString& /*Reason*/);

/** 接收线程：非阻塞轮询 → 攒字节 → 切帧 → 延迟队列（M5 损伤滑块）→ AsyncTask 投回游戏线程 */
class FSightlineReceiver : public FRunnable
{
public:
    FSightlineReceiver(FSocket* InSocket, FOnFrame InOnFrame)
        : Socket(InSocket), OnFrame(MoveTemp(InOnFrame)) {}

    /** M5 损伤滑块：入站延迟毫秒（原子——游戏线程写、接收线程读；0 = 直通） */
    void SetInboundDelayMs(int32 Ms) { InboundDelayMs.store(static_cast<int32>(Ms)); }

    virtual bool Init() override { return true; }
    virtual uint32 Run() override
    {
        UE_LOG(LogSightline, Verbose, TEXT("[RecvThread] 启动"));
        TArray<uint8> Buffer;
        while (!bStop)
        {
            uint32 Pending = 0;
            if (Socket->HasPendingData(Pending) && Pending > 0)
            {
                const FOnFrame FrameDelegate = OnFrame;   // 值拷贝（C++ 不能直接按值捕获成员）
                TArray<uint8> Tmp;
                Tmp.SetNumUninitialized(FMath::Min(Pending, 65536u));
                int32 Read = 0;
                if (Socket->Recv(Tmp.GetData(), Tmp.Num(), Read, ESocketReceiveFlags::None))
                {
                    Buffer.Append(Tmp.GetData(), Read);
                    SightlineProtocol::FFrame Frame;
                    while (SightlineProtocol::TryDecode(Buffer, Frame))
                    {
                        const int32 Delay = InboundDelayMs.load();
                        if (Delay > 0)   // 损伤注入：入队到期投递（队列仅接收线程触达，零锁）
                            DelayQueue.push_back({NowMs() + Delay, Frame.MsgId, MoveTemp(Frame.Payload)});
                        else
                            Dispatch(Frame.MsgId, Frame.Payload, FrameDelegate);
                    }
                }
            }
            // 到期帧出队（按投递时刻升序；10ms 轮询粒度 = 滑块延迟的抖动上界，M5 已知）
            const double Now = NowMs();
            for (auto It = DelayQueue.begin(); It != DelayQueue.end();)
            {
                if (It->DeliverAtMs <= Now)
                {
                    Dispatch(It->MsgId, MoveTemp(It->Payload), OnFrame);
                    It = DelayQueue.erase(It);
                }
                else
                {
                    ++It;
                }
            }
            FPlatformProcess::Sleep(0.01f);   // 10ms 轮询——心跳粒度足够，避免阻塞 recv 的关停竞态
        }
        UE_LOG(LogSightline, Verbose, TEXT("[RecvThread] 退出"));
        return 0;
    }
    virtual void Stop() override { bStop = true; }
    virtual void Exit() override {}

private:
    struct FDelayedFrame                     // 延迟队列元素（仅接收线程触达，零锁）
    {
        double DeliverAtMs = 0.0;
        uint16 MsgId = 0;
        TArray<uint8> Payload;
    };
    static double NowMs() { return FPlatformTime::Seconds() * 1000.0; }
    static void Dispatch(uint16 MsgId, TArray<uint8> Payload, const FOnFrame& Delegate)
    {
        // 唯一跨线程门：值拷贝投递到游戏线程（接收线程不碰任何游戏对象）
        AsyncTask(ENamedThreads::GameThread, [Delegate, MsgId, Payload]()
        {
            Delegate.ExecuteIfBound(MsgId, Payload);
        });
    }

    FSocket* Socket = nullptr;
    FOnFrame OnFrame;
    std::atomic<bool> bStop{false};
    std::atomic<int32> InboundDelayMs{0};
    std::deque<FDelayedFrame> DelayQueue;
};

/** 连接：FSocket 生命周期 + 接收线程的持有者（Disconnect 顺序 = 先停线程再关 socket） */
class FSightlineConnection : public ISendPort
{
public:
    ~FSightlineConnection()
    {
        // 析构 = 纯 RAII 收尾：只停线程关 socket，不触发 OnDisconnected
        // （析构期回调持有者是悬垂风险；正常断开请显式调 Disconnect）
        Receiver = nullptr;                   // Kill(true) 会删除 Runnable，指针先清
        if (ReceiverThread)
        {
            ReceiverThread->Kill(true);
            ReceiverThread = nullptr;
        }
        if (Socket && SocketSubsystem)
        {
            SocketSubsystem->DestroySocket(Socket);
            Socket = nullptr;
        }
    }

    bool Connect(const FString& Host, uint16 Port)
    {
        if (Socket) return false;

        SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
        bool Valid = false;
        Addr->SetIp(*Host, Valid);
        Addr->SetPort(Port);
        if (!Valid) return false;

        Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("Sightline"), Addr->GetProtocolType());
        if (!Socket) return false;
        Socket->SetNonBlocking(true);

        bConnected = Socket->Connect(*Addr);   // 非阻塞 connect：立即返回，成功与否由后续收发判定
        if (!bConnected) return false;

        Receiver = new FSightlineReceiver(Socket, OnFrame);
        ReceiverThread = FRunnableThread::Create(
            Receiver, TEXT("SightlineRecv"), 0, TPri_Normal);
        return ReceiverThread != nullptr;
    }

    void Disconnect(const FString& Reason)
    {
        Receiver = nullptr;                   // Kill(true) 会删除 Runnable，指针先清
        if (ReceiverThread)
        {
            ReceiverThread->Kill(true);       // Stop() → 等待 Run() 退出（轮询 10ms 内必退）
            ReceiverThread = nullptr;
        }
        if (Socket)
        {
            SocketSubsystem->DestroySocket(Socket);
            Socket = nullptr;
        }
        bConnected = false;
        if (OnDisconnected.IsBound()) OnDisconnected.Execute(Reason);
    }

    bool Send(uint16 MsgId, const TArray<uint8>& Payload)
    {
        if (!Socket || !bConnected) return false;
        const TArray<uint8> Frame = SightlineProtocol::Encode(MsgId, Payload);
        int32 Sent = 0;
        return Socket->Send(Frame.GetData(), Frame.Num(), Sent) && Sent == Frame.Num();
    }

    FOnFrame OnFrame;                 // 游戏线程回调（由持有者注入并绑定）
    FOnDisconnected OnDisconnected;
    bool IsConnected() const { return bConnected; }

private:
    ISocketSubsystem* SocketSubsystem = nullptr;
    FSocket* Socket = nullptr;
    FRunnableThread* ReceiverThread = nullptr;
    FSightlineReceiver* Receiver = nullptr;   // 非拥有裸指针（线程对象拥有；Kill 前置空）

public:
    /** M5 损伤滑块：入站延迟毫秒（透传接收线程的原子延迟值） */
    void SetInboundDelayMs(int32 Ms) { if (Receiver) Receiver->SetInboundDelayMs(Ms); }
    std::atomic<bool> bConnected{false};
};
