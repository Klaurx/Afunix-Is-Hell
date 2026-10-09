# Where This Starts

The state corruption is documented separately. This picks up from the point where the corrupted +0x1c0 field feeds into the refcount and lifetime logic of the endpoint.

The Uncovered Caller

AfUnixDeliverDataToClient (0x14000aa28) has two callers:
```
fcn.14000ae64  @ 0x14000b043   AfUnixDeliverEndpointSend — has ref bracket
(nofunc)       @ 0x14000a4e0   inside fcn.140009d74 — no ref bracket
```
The call at 0x14000a4e0:

```
0x14000a4db      mov rcx, rbx          ; raw endpoint pointer
0x14000a4de      xor edi, edi
0x14000a4e0      call fcn.14000aa28    ; no lock xadd before this
0x14000a4e5      mov ebx, 0x103
```

No lock xadd before the call. No fcn.14000b8fc after it. The endpoint is passed raw with no ownership taken. AfUnixDeliverDataToClient itself takes no reference on entry either — it just receives the pointer and operates on it directly.

This is the uncovered caller. One uncovered caller is enough. 

# The Unlocked Window

Inside AfUnixDeliverDataToClient, the lock is dropped mid-function and not reacquired for 0x1cd bytes of instructions:

```
0x14000aaef      call ExReleasePushLockExclusiveEx   ; lock dropped
0x14000aafe      call KeLeaveCriticalRegion
; ← window opens here
0x14000ab10      call fcn.14000a968                  ; IRP completion loop
0x14000ab24      call fcn.14000a9c0                  ; send buffer drain loop
; ... data list walk ...
0x14000abbc      mov rax, qword [rcx+0x178]          ; direct endpoint deref
0x14000abc3      mov rcx, qword [rcx+0x180]          ; direct endpoint deref
0x14000abce      call fcn.1400025e0                  ; AFD callback
; ... OR writes, partial delivery handling ...
0x14000accb      call KeEnterCriticalRegion
0x14000acdc      call ExAcquirePushLockExclusiveEx   ; lock reacquired
; ← window closes here
```
During this window the delivery thread holds nothing. No lock, no refcount. The endpoint is live only because nothing has freed it yet.

The Close Path

AfUnixTlConnectEndpointClose is the WSK dispatch callback for a connected endpoint close. Confirmed from the dispatch table at runtime:
```
fffff809`40a34000  afunix!AfUnixTlConnectEndpointClose
```
When close fires it acquires the pushlock before proceeding. If the delivery thread is inside the unlocked window, the lock is free — close acquires it instantly and runs to completion.

Refcount at close entry: 0x0000000000000001. Confirmed live:
```
afunix!AfUnixEndpointDereference+0x1b
afunix!AfUnixTlConnectEndpointClose+0xbe
afd!AfdCloseConnection+0x1b7
```
Refcount hits zero. ExFreePoolWithTag fires on the endpoint struct. Under Driver Verifier special pool the page is unmapped immediately on free.

The Fault Point

The direct endpoint dereference at +0x194 inside the unlocked window:

```
0x14000abbc      mov rax, qword [rcx+0x178]
```
RCX is the endpoint struct — loaded from [var_8h] at 0x14000abb0, which is arg1 saved at function entry. If the endpoint has been freed and special pool is active, this read faults immediately with full attribution to afunix.

The Full Sequence
```
Thread A: fcn.140009d74 → AfUnixDeliverDataToClient
    → drops lock at 0x14000aaef
    → no refcount held
    → executing inside IRP completion loop / buffer drain

Thread B: closesocket()
    → AfUnixTlConnectEndpointClose
    → lock is free, acquires instantly
    → AfUnixEndpointDereference
    → refcount 1 → 0
    → ExFreePoolWithTag frees endpoint struct
    → special pool unmaps the page

Thread A: continues executing
    → reaches 0x14000abbc
    → mov rax, [rcx+0x178]
    → RCX = freed endpoint
    → page fault
    → DRIVER_VERIFIER_DETECTED_VIOLATION
```

# Live Confirmation

Caught in a single continuous run under Driver Verifier special pool:
```
*** FREE P=ffffe50c7f40cdd0 ***
*** POST-FREE DEREF P=ffffe50c7f40cdd0 ***
```
Same pointer. Free then deref. Full callstack at the deref:
```
afunix!AfUnixDeliverDataToClient+0x194
afunix!AfUnixDeliverEndpointSend+0x1e4
afunix!AfUnixTlConnectEndpointSend+0x118
afd!AfdTLStartBufferedVcSend+0xd8
afd!AfdFastConnectionSend+0x497
afd!AfdFastIoDeviceControl+0xe1e
nt!IopXxxControlFile+0x431
nt!NtDeviceIoControlFile+0x5e
nt!KiSystemServiceCopyEnd+0x25
```
Unprivileged WSASend. No special setup. Any process that can open an AF_UNIX socket pair can reach this path.

bug/static-analysis.md


# Scope

This covers the static analysis done to map the UAF geometry specifically — the ref bracket audit, the uncovered caller, the unlocked window contents, and the dispatch table confirmation. The locking audit and lost-update scenario for the state corruption are in the separate state corruption repo.

Ref Bracket Audit

Every caller of AfUnixDeliverDataToClient (0x14000aa28) was checked for a refcount bracket around the call.

fcn.14000ae64 (AfUnixDeliverEndpointSend) at 0x14000b043 — covered:

```
0x14000aed1      lock xadd qword [rdi], rax   ; increment peer refcount
; ... work ...
0x14000b043      call fcn.14000aa28            ; DeliverDataToClient
; ... work ...
0x14000b127      call fcn.14000b8fc            ; destructor — decrement
```

Increment before, decrement after. Covered.

fcn.140009d74 at 0x14000a4e0 — not covered:

```
0x14000a4db      mov rcx, rbx
0x14000a4de      xor edi, edi
0x14000a4e0      call fcn.14000aa28            ; no lock xadd before
0x14000a4e5      mov ebx, 0x103
```

No increment before. No decrement after. Raw pointer passed with no ownership. This is the uncovered caller that makes the race geometrically valid.

AfUnixDeliverDataToClient itself takes no reference on entry — confirmed by reading the full function prologue, no lock xadd anywhere before the first use of the endpoint pointer.

Unlocked Window Contents

Full disassembly of the unlocked region between ExReleasePushLockExclusiveEx at 0x14000aaef and ExAcquirePushLockExclusiveEx at 0x14000acdc.

fcn.14000a968 — IRP completion loop:

```
0x14000a97c      mov rax, qword [rbx]          ; load IRP
0x14000a985      mov r8, qword [rbx+0x30]
0x14000a989      mov rcx, qword [rbx+8]
0x14000a98d      call fcn.1400025e0            ; fire completion callback
0x14000a99b      call ExFreePoolWithTag         ; free the IRP buffer
0x14000a997      mov rbx, qword [rbx+0x50]     ; next IRP
0x14000a9aa      jne 0x14000a97c               ; loop
```

Walks and frees the pending IRP list. One iteration per pending receive. Queue depth is userland controllable via how many overlapped WSARecv calls are posted.

fcn.14000a9c0 — send buffer drain loop:

```
0x14000a9da      mov rax, qword [rbx+0x20]
0x14000a9eb      call fcn.1400025e0            ; fire callback per buffer
0x14000a9fb      call ExFreePoolWithTag         ; free the buffer
0x14000aa07      mov rbx, qword [rdi]           ; next buffer
0x14000aa0d      jne 0x14000a9da               ; loop
```

Same pattern. Walks and frees the send buffer list. One iteration per backed-up send. Queue depth controllable via overlapped WSASend calls.

The direct endpoint dereferences:

```
0x14000abbc      mov rax, qword [rcx+0x178]    ; endpoint+0x178
0x14000abc3      mov rcx, qword [rcx+0x180]    ; endpoint+0x180
0x14000abce      call fcn.1400025e0             ; AFD callback
```

RCX here is the endpoint struct loaded from [var_8h] — arg1 saved at function entry. These are direct reads from the endpoint struct itself, not from child objects. If the endpoint is freed before this point, these reads are the UAF surface. Under special pool the first read faults immediately.

Dispatch Table Confirmation

AfUnixTlConnectEndpointClose confirmed as the WSK close callback from the runtime dispatch table:
```
fffff809`40a34000  afunix!AfUnixTlConnectEndpointClose
fffff809`40a34008  afunix!AfUnixTlConnectEndpointIoControl
fffff809`40a34010  afunix!TlDefaultRequestQueryDispatchEndpoint
fffff809`40a34018  afunix!AfUnixTlConnectEndpointSend
fffff809`40a34020  afunix!AfUnixTlConnectEndpointReceive
fffff809`40a34028  afunix!AfUnixTlConnectEndpointDisconnect
fffff809`40a34030  afunix!AfUnixTlEndpointClose
fffff809`40a34038  afunix!AfUnixTlEndpointIoControl
```
Module base at runtime: fffff80979130000. Static address 0x140004000relocated tofffff80979134000. Table dumped with dps after resolving the loaded base via lm m afunix.
