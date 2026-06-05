#!/usr/bin/env python3
"""
buddy_hook.py — hook universal do Claude Code → claude_buddy_bridge

Encaminha o payload do hook (stdin) para a ponte local. Para PreToolUse,
espera a decisao (botao da placa) e devolve permissionDecision; para os
demais eventos e fire-and-forget. Se a ponte nao estiver rodando, sai
em silencio — o Claude Code segue normal.
"""
import json
import sys
import urllib.request

BRIDGE = "http://127.0.0.1:8788/hook"

try:
    payload = json.load(sys.stdin)
except ValueError:
    sys.exit(0)

event = payload.get("hook_event_name")
blocking = event in ("PreToolUse", "PermissionRequest")
timeout = 90 if blocking else 3

try:
    req = urllib.request.Request(
        BRIDGE, data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"})
    body = urllib.request.urlopen(req, timeout=timeout).read()
except Exception:
    sys.exit(0)          # ponte parada → nao interfere

try:
    out = json.loads(body or b"{}")
except ValueError:
    sys.exit(0)

decision = out.get("decision")
if decision in ("allow", "deny"):
    if event == "PermissionRequest":
        # Formato proprio do PermissionRequest: decision.behavior.
        d = {"behavior": decision}
        if decision == "allow":
            d["updatedInput"] = payload.get("tool_input") or {}
        else:
            d["reason"] = "negado no hardware buddy"
        print(json.dumps({"hookSpecificOutput": {
            "hookEventName": "PermissionRequest", "decision": d}}))
    elif event == "PreToolUse":
        print(json.dumps({"hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": decision,
            "permissionDecisionReason": "decidido no hardware buddy",
        }}))
sys.exit(0)
