#!/usr/bin/env python3
#
# Static checks on the dwcmshc sdport miniport. Fast, no toolchain.
#
# 1. IRQL. sdport calls some miniport callbacks at DIRQL: Interrupt from its
#    ISR, and ToggleEvents both from its ISR (to mask the SDIO card interrupt)
#    and through KeSynchronizeExecution (SdhostDisableEvents/EnableEvents in
#    sdport.sys 10.0.22621.608). Nothing reachable from those callbacks in
#    this driver may call a routine that is DISPATCH_LEVEL at most (work
#    items, the registry, pool, waits). ClearEvents is checked too: it is
#    cheap to keep it clean, and sdport's error recovery calls it next to
#    the synchronized ones.
#
# 2. Request ownership. sdport finds the slot of a request through a field
#    past the public SDPORT_REQUEST, so SdPortCompleteRequest must only ever
#    be given a request sdport issued. The driver hands requests back in
#    exactly one place, MshcCompleteRequest, behind a compare-exchange on
#    SdportRequest. Fail if a second call appears or the guard goes away.
#
# The parser is crude on purpose: the driver's style puts a function's name
# on its own line followed by "(", and the body's braces in column 0.
#
import re
import sys
from pathlib import Path

DRIVER = Path("drivers/sd/dwcmshc")
SOURCES = ["dwcmshc.cpp", "rockchip_ops.cpp", "sdiodiag.cpp"]

DIRQL_ROOTS = ["MshcSlotInterrupt", "MshcSlotToggleEvents", "MshcSlotClearEvents"]

# Routines a DIRQL path must not reach. MshcSdioDiagKick refuses above
# DISPATCH_LEVEL itself, but a DIRQL callback has no business calling it.
FORBIDDEN = {
    "IoQueueWorkItem", "IoQueueWorkItemEx", "ExQueueWorkItem",
    "IoAllocateWorkItem", "IoFreeWorkItem",
    "ZwCreateKey", "ZwOpenKey", "ZwSetValueKey", "ZwQueryValueKey", "ZwFlushKey", "ZwClose",
    "KeDelayExecutionThread", "KeWaitForSingleObject", "KeWaitForMultipleObjects",
    "ExAllocatePool", "ExAllocatePool2", "ExAllocatePoolZero", "ExAllocatePoolWithTag",
    "ExFreePool", "ExFreePoolWithTag",
    "ExAcquireFastMutex", "ExReleaseFastMutex",
    "KeAcquireSpinLock", "KeReleaseSpinLock",
    "MshcSdioDiagKick", "MshcIssueRequestSynchronously",
}

DEF_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\($", re.M)
CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def functions(text):
    """Map function name -> body text, for definitions in this file."""
    out = {}
    for m in DEF_RE.finditer(text):
        name = m.group(1)
        rest = text[m.end():]
        # The parameter list ends in "    )" on its own line: a prototype
        # goes on with ";", a definition with "{" in column 0, and the body
        # ends at the next "}" in column 0.
        close = re.search(r"\n\s*\)(;?)\n", rest)
        if close is None or close.group(1) == ";":
            continue
        after = rest[close.end():]
        if not after.startswith("{\n"):
            continue
        end = after.find("\n}\n")
        if end < 0:
            continue
        out[name] = after[:end]
    return out


def main():
    bodies = {}
    for src in SOURCES:
        text = (DRIVER / src).read_text(encoding="utf-8-sig")
        bodies.update(functions(text))

    fail = 0
    for root in DIRQL_ROOTS:
        if root not in bodies:
            print(f"FAIL: DIRQL root {root} not found in {DRIVER}")
            fail = 1

    # Walk the call graph from the DIRQL roots.
    for root in DIRQL_ROOTS:
        seen = {root: [root]}
        todo = [root]
        while todo:
            fn = todo.pop()
            for callee in CALL_RE.findall(bodies.get(fn, "")):
                path = seen[fn] + [callee]
                if callee in FORBIDDEN:
                    print("FAIL: DIRQL path calls a DISPATCH_LEVEL-at-most routine: "
                          + " -> ".join(path))
                    fail = 1
                if callee in bodies and callee not in seen:
                    seen[callee] = path
                    todo.append(callee)

    # Request ownership.
    callers = [fn for fn, body in bodies.items() if "SdPortCompleteRequest(" in body]
    if callers != ["MshcCompleteRequest"]:
        print(f"FAIL: SdPortCompleteRequest must be called from MshcCompleteRequest only, found in {callers}")
        fail = 1
    else:
        body = bodies["MshcCompleteRequest"]
        calls = body.count("SdPortCompleteRequest(")
        guard = re.search(
            r"InterlockedCompareExchangePointer\(\s*\(PVOID volatile \*\)&MshcExtension->SdportRequest,\s*NULL,\s*Request\)\s*==\s*Request\)\)\s*\{\s*SdPortCompleteRequest\(",
            body)
        if calls != 1 or guard is None:
            print("FAIL: MshcCompleteRequest must hand a request to sdport once, behind the SdportRequest compare-exchange")
            fail = 1

    if not fail:
        print(f"OK: {len(bodies)} functions; no DISPATCH-only call under {', '.join(DIRQL_ROOTS)}; "
              "one guarded SdPortCompleteRequest")
    return fail


if __name__ == "__main__":
    sys.exit(main())
