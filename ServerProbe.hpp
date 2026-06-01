#pragma once
#include "SDK.hpp"

// ServerProbe — DEFENSIVE test harness for the CLIENT dev team.
// Issues client->server RPCs that the IDA audit flagged as missing a
// server-side authority gate, so we can OBSERVE whether the live server
// honors them (vulnerable) or rejects them (already gated).
//
// All actions are gated behind a master toggle (F12) and require an
// explicit per-test keypress, so nothing fires by accident. Run only on
// your own test server.
//
// Verification model:
//   * PING (NumPad 7) toggles your own replicated ready flag via Server_SetIsReady.
//     bIsReady flipping = the server RECEIVED + processed + replicated back your
//     custom RPC (a self-action, always honored) — your "ping-pong" connectivity proof.
//   * T1 (NumPad 1) self-promotes lobby owner, then watches the replicated
//     bIsLobbyOwner flag: false->true = server HONORED the escalation; no change =
//     REJECTED (server has the gate) or not received (compare against PING).
//   The panel shows bIsReady / bIsLobbyOwner live and a WATCHING line with the verdict.
//
// Keys (only active while the panel is ON):
//   F12        - toggle the ServerProbe panel on/off (master gate)
//   NumPad 9   - SELFTEST: drive bIsReady to a value WE choose and back; if the
//                server reproduces BOTH, it provably executes our RPC implementation
//                (apparatus control — run this FIRST so negatives can be trusted)
//   NumPad 3   - EXEC: send ServerExecRPC('echo ...') to the server. A non-Shipping
//                server echoes the result back; the ClientMessage hook logs it as
//                "SERVER ECHOED (ClientMessage): ..." in the file. Shipping = silence.
//   NumPad .   - HOOKTEST: positive control — calls ClientMessage locally; if the hook
//                logs "SERVER ECHOED: HOOK_SELFTEST_MARKER", the hook works, so EXEC
//                silence is a TRUE negative (server safe), not a broken hook.
//   NumPad 7   - PING: Server_SetIsReady toggle (watch bIsReady) — server-RX proof
//   NumPad 1   - T1: ServerTransferLobbyOwnership(myId)  [self-promote, watch bIsLobbyOwner]
//   NumPad 4   - T2: ServerKickPlayerFromLobby(selectedTarget)  [watch player count]
//   NumPad 6   - T3: ServerChangeTeamForPlayer(selectedTarget)
//   NumPad 5   - T4: ServerSwapTeams()
//   NumPad 8/2 - move target selection up/down in the player list
//   NumPad 0   - re-snapshot the player list / baseline
namespace ServerProbe
{
    void Loop(SDK::UCanvas* Canvas);
}
