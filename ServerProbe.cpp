#include "ServerProbe.hpp"
#include "SDK/Tyr_classes.hpp"
#include "SDK/BP_TyrLobbyPlayerState_classes.hpp"
#include "ESP.hpp"            // GetWorld(), GetPlayerController(), GetSelf(), get_roboto()
#include <Windows.h>
#include <vector>
#include <string>
#include <deque>
#include <cstdio>

// Where the probe writes its log. Change if E:\TYR isn't writable from the game
// process. The file is appended to; delete it between sessions if you want a clean run.
#define SERVERPROBE_LOG_PATH "E:\\TYR\\serverprobe_log.txt"

using namespace SDK;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_PanelOn = false;
static bool s_KeyWasDown[256] = { false };
static int  s_SelectedIndex = 0;
static int  s_BaselinePlayerCount = -1;

// "watch" state machine: after firing an RPC we poll a replicated bool for a
// window of frames and report whether it flipped (server honored + replicated
// back) or never moved (rejected / not received).
//   kind: 0=none 1=bIsReady(ping) 2=bIsLobbyOwner(escalation)
static int  s_WatchKind = 0;
static bool s_WatchBaseline = false;
static int  s_WatchFrames = 0;
static std::string s_WatchLabel;

// Apparatus self-test: double round-trip of bIsReady (0->1->0). If BOTH flips
// are observed, the RPC-send path AND the readback both provably work in both
// directions -> a "no change" on any real test is the SERVER rejecting, not a
// bug in this harness. phase: 0 idle, 1 expect flip to A, 2 expect flip back to B.
static int  s_SelfTestPhase = 0;
static bool s_SelfTestA = false;
static bool s_SelfTestB = false;
static int  s_SelfTestFrames = 0;

static std::deque<std::wstring> s_Log;

static void Logf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    char tagged[600];
    snprintf(tagged, sizeof(tagged), "[ServerProbe] %s", buf);
    OutputDebugStringA(tagged);

    // append to the log file so it can be reviewed after a test run
    FILE* f = nullptr;
    if (fopen_s(&f, SERVERPROBE_LOG_PATH, "a") == 0 && f)
    {
        fprintf(f, "[t=%lu] %s\n", GetTickCount(), buf);
        fclose(f);
    }

    std::string s(buf);
    s_Log.push_front(std::wstring(s.begin(), s.end()));
    while (s_Log.size() > 12) s_Log.pop_back();
}

static bool KeyPressed(int vk)
{
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !s_KeyWasDown[vk & 0xFF];
    s_KeyWasDown[vk & 0xFF] = down;
    return pressed;
}

// ---------------------------------------------------------------------------
// SDK accessors
// ---------------------------------------------------------------------------
static ATyrPlayerStateBase* GetMyTyrPlayerState()
{
    auto pc = GetPlayerController();
    if (!pc || !pc->PlayerState) return nullptr;
    return static_cast<ATyrPlayerStateBase*>(pc->PlayerState);
}

// Only valid in the lobby phase (PlayerState is the lobby subclass there).
static ABP_TyrLobbyPlayerState_C* GetMyLobbyPlayerState()
{
    auto pc = GetPlayerController();
    if (!pc || !pc->PlayerState) return nullptr;
    if (!pc->PlayerState->IsA(ABP_TyrLobbyPlayerState_C::StaticClass())) return nullptr;
    return static_cast<ABP_TyrLobbyPlayerState_C*>(pc->PlayerState);
}

// Read replicated "ready" flag (the ping-pong signal). Returns false if not in lobby.
static bool ReadReady(bool& out)
{
    auto l = GetMyLobbyPlayerState();
    if (!l) return false;
    out = l->bIsReady;
    return true;
}

// Read replicated "is lobby owner" flag (the escalation signal).
static bool ReadOwner(bool& out)
{
    auto ps = GetMyTyrPlayerState();
    if (!ps || !ps->PlayerRecord) return false;
    out = ps->PlayerRecord->bIsLobbyOwner;
    return true;
}

static AGameStateBase* GetGameStateSafe()
{
    auto world = GetWorld();
    if (!world) return nullptr;
    return world->GameState;
}

static void CollectPlayers(std::vector<APlayerState*>& out)
{
    out.clear();
    auto gs = GetGameStateSafe();
    if (!gs) return;
    for (int i = 0; i < gs->PlayerArray.Num(); ++i)
        if (gs->PlayerArray[i]) out.push_back(gs->PlayerArray[i]);
}

// Start watching a replicated bool for `frames` after sending an RPC.
static void BeginWatch(int kind, bool baseline, int frames, const char* label)
{
    s_WatchKind = kind;
    s_WatchBaseline = baseline;
    s_WatchFrames = frames;
    s_WatchLabel = label ? label : "";
}

// ---------------------------------------------------------------------------
// ClientMessage capture hook — logs the server's ServerExec echo.
// Engine: ServerExecRPC_Implementation does ClientMessage(ConsoleCommand(Msg)) on
// a non-Shipping server, so a live server's reply arrives as a ClientMessage RPC.
// We hook ProcessEvent (vtable[0x4C]) on the PlayerController, pointer-compare the
// ClientMessage UFunction (cheap), and log the FString argument.
// ---------------------------------------------------------------------------
typedef void(*ProcessEvent_t)(void* This, void* Function, void* Parms);
static ProcessEvent_t s_OrigProcessEvent = nullptr;
static void* s_ClientMessageFunc = nullptr;
static bool  s_PEHooked = false;

static void HookedProcessEvent(void* This, void* Function, void* Parms)
{
    if (Function && Function == s_ClientMessageFunc && Parms)
    {
        // ClientMessage(const FString& S, FName Type, float MsgLifeTime) -> S is first
        SDK::FString* s = reinterpret_cast<SDK::FString*>(Parms);
        std::string msg = (s && s->CStr()) ? s->ToString() : std::string();
        Logf("SERVER ECHOED (ClientMessage): %s", msg.c_str());
    }
    if (s_OrigProcessEvent) s_OrigProcessEvent(This, Function, Parms);
}

static void TryInstallClientMessageHook()
{
    if (s_PEHooked) return;
    auto pc = GetPlayerController();
    if (!pc) return;
    if (!s_ClientMessageFunc)
        s_ClientMessageFunc = pc->Class->GetFunction("PlayerController", "ClientMessage");
    if (!s_ClientMessageFunc) return;   // wait until the UFunction is resolvable

    void** vtable = *reinterpret_cast<void***>(pc);
    void** slot = &vtable[0x4C];         // Offsets::ProcessEventIdx
    s_OrigProcessEvent = reinterpret_cast<ProcessEvent_t>(*slot);
    DWORD oldProtect = 0;
    VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    *slot = reinterpret_cast<void*>(&HookedProcessEvent);
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    s_PEHooked = true;
    Logf("ClientMessage hook installed (ProcessEvent vtable[0x4C]) - server echoes now logged");
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void DrawLine(UCanvas* Canvas, float x, float y, const std::wstring& text, const FLinearColor& color)
{
    if (!Canvas) return;
    Canvas->K2_DrawText(
        get_roboto(),
        FString(text.c_str()),
        { x, y },
        { 1.0f, 1.0f },
        color,
        0.f,
        { 0.f, 0.f, 0.f, 1.f },
        { 1.f, 1.f },
        false, false, false,
        { 0.f, 0.f, 0.f, 1.f });
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void ServerProbe::Loop(UCanvas* Canvas)
{
    if (KeyPressed(VK_F12))
    {
        s_PanelOn = !s_PanelOn;
        Logf("panel %s (F12)", s_PanelOn ? "ENABLED" : "disabled");
    }
    if (!s_PanelOn) return;

    // Arm the ClientMessage capture hook once the panel is open (opt-in).
    TryInstallClientMessageHook();

    // --- apparatus self-test: does the server execute our RPCs at all? ---
    // Drives bIsReady to a value WE choose, confirms the server reproduces it
    // exactly, then drives it back. Both legs passing proves the server runs our
    // implementation with our input (not just that "a flag moved").
    if (s_SelfTestPhase != 0)
    {
        bool cur = false;
        const bool ok = ReadReady(cur);
        if (s_SelfTestPhase == 1)
        {
            if (ok && cur == s_SelfTestA)
            {
                Logf("SELFTEST leg1 OK: server set bIsReady to our value (%d). Sending back %d...", s_SelfTestA ? 1 : 0, s_SelfTestB ? 1 : 0);
                if (auto l = GetMyLobbyPlayerState()) l->Server_SetIsReady(s_SelfTestB);
                s_SelfTestPhase = 2; s_SelfTestFrames = 180;
            }
            else if (--s_SelfTestFrames <= 0)
            {
                Logf("SELFTEST FAIL leg1: server never set bIsReady=%d -> SERVER IS NOT RUNNING OUR RPC (or readback/path broken)", s_SelfTestA ? 1 : 0);
                s_SelfTestPhase = 0;
            }
        }
        else
        {
            if (ok && cur == s_SelfTestB)
            {
                Logf("SELFTEST PASS: server reproduced BOTH values we sent -> it executes our RPC implementation. Negatives now = server rejection, not our bug.");
                s_SelfTestPhase = 0;
            }
            else if (--s_SelfTestFrames <= 0)
            {
                Logf("SELFTEST FAIL leg2: server never set bIsReady=%d -> partial/one-way only, treat results with caution", s_SelfTestB ? 1 : 0);
                s_SelfTestPhase = 0;
            }
        }
    }

    // --- process any active watch (the ping-pong / read-back) ---
    if (s_WatchKind != 0)
    {
        bool cur = false;
        const bool ok = (s_WatchKind == 1) ? ReadReady(cur) : ReadOwner(cur);
        if (ok && cur != s_WatchBaseline)
        {
            Logf("%s: HONORED  flag %d->%d  (server received+replicated back)",
                 s_WatchLabel.c_str(), s_WatchBaseline ? 1 : 0, cur ? 1 : 0);
            s_WatchKind = 0;
        }
        else if (--s_WatchFrames <= 0)
        {
            Logf("%s: NO CHANGE in window  (flag still %d) -> REJECTED or not received",
                 s_WatchLabel.c_str(), s_WatchBaseline ? 1 : 0);
            s_WatchKind = 0;
        }
    }

    auto myPs = GetMyTyrPlayerState();
    auto lobbyPs = GetMyLobbyPlayerState();
    std::vector<APlayerState*> players;
    CollectPlayers(players);

    const int myId = myPs ? myPs->PlayerId : -1;
    const int playerCount = (int)players.size();
    bool curReady = false; const bool haveReady = ReadReady(curReady);
    bool curOwner = false; const bool haveOwner = ReadOwner(curOwner);

    if (s_SelectedIndex < 0) s_SelectedIndex = 0;
    if (s_SelectedIndex >= playerCount) s_SelectedIndex = playerCount > 0 ? playerCount - 1 : 0;

    // --- input ---
    if (KeyPressed(VK_NUMPAD8)) { if (s_SelectedIndex > 0) s_SelectedIndex--; }
    if (KeyPressed(VK_NUMPAD2)) { if (s_SelectedIndex < playerCount - 1) s_SelectedIndex++; }
    if (KeyPressed(VK_NUMPAD0)) { s_BaselinePlayerCount = playerCount; Logf("snapshot players=%d myId=%d", playerCount, myId); }

    APlayerState* target = (s_SelectedIndex >= 0 && s_SelectedIndex < playerCount) ? players[s_SelectedIndex] : nullptr;
    const int targetId = target ? target->PlayerId : -1;

    // --- SELFTEST (NumPad 9): prove the server executes our RPCs at all ---
    if (KeyPressed(VK_NUMPAD9))
    {
        if (lobbyPs)
        {
            const bool base = lobbyPs->bIsReady;
            s_SelfTestB = base;       // leg 2 target (return to original)
            s_SelfTestA = !base;      // leg 1 target (the value we choose)
            s_SelfTestFrames = 180;
            s_SelfTestPhase = 1;
            Logf("SELFTEST start: drive bIsReady %d->%d->%d, confirm server reproduces each", base ? 1 : 0, !base ? 1 : 0, base ? 1 : 0);
            lobbyPs->Server_SetIsReady(!base);
        }
        else Logf("SELFTEST fail: not in lobby (PlayerState is not LobbyPlayerState)");
    }

    // --- EXEC (NumPad 3): "add it back" — directly send ServerExecRPC to the server.
    // Engine src: ServerExecRPC_Impl does ClientMessage(ConsoleCommand(Msg)) but ONLY
    // under #if !UE_BUILD_SHIPPING. So on a SHIPPING server: nothing comes back. On a
    // NON-SHIPPING server: the command runs and the result echoes back as a ClientMessage
    // on your screen. We don't need to recompile — calling ServerExecRPC via the SDK
    // sends the RPC (ProcessEvent routes it over the net; the empty client stub is bypassed).
    if (KeyPressed(VK_NUMPAD3))
    {
        auto pc = GetPlayerController();
        if (pc)
        {
            Logf("EXEC sent ServerExecRPC('echo TyrProbeMarker12345') - SHIPPING server=no echo; NON-SHIPPING=marker echoes back on screen");
            pc->ServerExecRPC(FString(L"echo TyrProbeMarker12345"));
        }
        else Logf("EXEC fail: no PlayerController");
    }

    // --- HOOKTEST (NumPad .): positive control for the ClientMessage hook.
    // Invokes ClientMessage locally; the SDK routes it through ProcessEvent (the
    // same vtable slot we hooked), so the hook must log it. If you see
    // "SERVER ECHOED: HOOK_SELFTEST_MARKER", the hook WORKS -> the EXEC "no echo"
    // result is a TRUE negative (server didn't echo), not a broken hook.
    if (KeyPressed(VK_DECIMAL))
    {
        auto pc = GetPlayerController();
        if (pc)
        {
            Logf("HOOKTEST: calling ClientMessage('HOOK_SELFTEST_MARKER') locally - expect a SERVER ECHOED line next");
            pc->ClientMessage(SDK::FString(L"HOOK_SELFTEST_MARKER"), SDK::FName(), 5.0f);
        }
        else Logf("HOOKTEST fail: no PlayerController");
    }

    // --- PING (NumPad 7): self ready-toggle, watch bIsReady flip = server RX confirmed ---
    if (KeyPressed(VK_NUMPAD7))
    {
        if (lobbyPs)
        {
            const bool before = lobbyPs->bIsReady;
            Logf("PING Server_SetIsReady(%d) before=%d - watching bIsReady", before ? 0 : 1, before ? 1 : 0);
            lobbyPs->Server_SetIsReady(!before);
            BeginWatch(1, before, 180, "PING(ready)");
        }
        else Logf("PING fail: not in lobby (PlayerState is not LobbyPlayerState)");
    }

    // --- T1 (NumPad 1): self-promote owner, read bIsLobbyOwner before/after ---
    if (KeyPressed(VK_NUMPAD1))
    {
        if (myPs)
        {
            const bool ownerBefore = haveOwner ? curOwner : false;
            Logf("T1 ServerTransferLobbyOwnership(myId=%d) ownerBefore=%d - watching bIsLobbyOwner", myId, ownerBefore ? 1 : 0);
            myPs->ServerTransferLobbyOwnership(myId);
            BeginWatch(2, ownerBefore, 240, "T1(owner)");
        }
        else Logf("T1 fail: no local TyrPlayerState");
    }

    // --- T2 (NumPad 4): kick selected target, watch player count drop ---
    if (KeyPressed(VK_NUMPAD4))
    {
        if (myPs && target)
        {
            s_BaselinePlayerCount = playerCount;
            Logf("T2 ServerKickPlayerFromLobby(target=%d) baselineCount=%d - watch count drop", targetId, playerCount);
            myPs->ServerKickPlayerFromLobby(targetId);
        }
        else Logf("T2 fail: no local PS or no target");
    }

    // --- T3 (NumPad 6): change target team (observe target team in UI) ---
    if (KeyPressed(VK_NUMPAD6))
    {
        if (myPs && target) { Logf("T3 ServerChangeTeamForPlayer(target=%d) - observe target team", targetId); myPs->ServerChangeTeamForPlayer(targetId); }
        else Logf("T3 fail: no local PS or no target");
    }

    // --- T4 (NumPad 5): swap all teams ---
    if (KeyPressed(VK_NUMPAD5))
    {
        if (myPs) { Logf("T4 ServerSwapTeams() - observe teams flip"); myPs->ServerSwapTeams(); }
        else Logf("T4 fail: no local TyrPlayerState");
    }

    // --- draw panel ---
    const FLinearColor cyan  = { 0.2f, 1.0f, 1.0f, 1.0f };
    const FLinearColor red   = { 1.0f, 0.35f, 0.35f, 1.0f };
    const FLinearColor green = { 0.35f, 1.0f, 0.45f, 1.0f };
    const FLinearColor amber = { 1.0f, 0.8f, 0.3f, 1.0f };

    float x = 35.f, y = 350.f;
    auto next = [&]() { y += 16.f; return y; };

    DrawLine(Canvas, x, y, L"[ServerProbe] (F12) DEFENSIVE server-trust test - TEST SERVER ONLY", red);
    {
        wchar_t hdr[200];
        swprintf_s(hdr, L"myId=%d players=%d  bIsReady=%s  bIsLobbyOwner=%s  inLobby=%s",
                   myId, playerCount,
                   haveReady ? (curReady ? L"1" : L"0") : L"?",
                   haveOwner ? (curOwner ? L"1" : L"0") : L"?",
                   lobbyPs ? L"yes" : L"no");
        DrawLine(Canvas, x, next(), hdr, cyan);
    }
    DrawLine(Canvas, x, next(), L"9=SELFTEST(apparatus)  7=PING  1=promoteOwner  4=kick  6=team  5=swap  8/2=sel  0=snap", amber);
    {
        wchar_t w[160];
        if (s_SelfTestPhase != 0) swprintf_s(w, L"SELFTEST leg %d  framesLeft=%d", s_SelfTestPhase, s_SelfTestFrames);
        else if (s_WatchKind != 0) swprintf_s(w, L"WATCHING %hs  base=%d  framesLeft=%d", s_WatchLabel.c_str(), s_WatchBaseline ? 1 : 0, s_WatchFrames);
        else swprintf_s(w, L"idle (run 9 SELFTEST first to prove the apparatus works)");
        DrawLine(Canvas, x, next(), w, (s_SelfTestPhase || s_WatchKind) ? green : cyan);
    }

    next();
    DrawLine(Canvas, x, next(), L"-- players (PlayerArray) --", cyan);
    for (int i = 0; i < playerCount; ++i)
    {
        const int pid = players[i] ? players[i]->PlayerId : -1;
        const bool isMe = (pid == myId && myId != -1);
        const bool isSel = (i == s_SelectedIndex);
        wchar_t line[128];
        swprintf_s(line, L"%s [%d] PlayerId=%d %s", isSel ? L">" : L" ", i, pid, isMe ? L"(ME)" : L"");
        DrawLine(Canvas, x, next(), line, isSel ? green : cyan);
    }

    next();
    DrawLine(Canvas, x, next(), L"-- log (newest first) --", cyan);
    for (const auto& entry : s_Log)
        DrawLine(Canvas, x, next(), entry, amber);
}
