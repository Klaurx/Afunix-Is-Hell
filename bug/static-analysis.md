# Static analysis

Ref bracket audit, uncovered caller, unlocked window contents, and dispatch table confirmation. 

The locking audit and lost-update scenario for the state corruption are in the other repo, go read that first or this won't make sense.

---

## Ref Bracket Audit

Every caller of `AfUnixDeliverDataToClient` (`0x14000aa28`) was checked for a refcount bracket around the call. There are two callers. one is fine. one is not.

**fcn.14000ae64 (`AfUnixDeliverEndpointSend`) at `0x14000b043` -- covered:**

```asm
0x14000aed1      lock xadd qword [rdi], rax   ; increment peer refcount
; ... work ...
0x14000b043      call fcn.14000aa28            ; DeliverDataToClient
; ... work ...
0x14000b127      call fcn.14000b8fc            ; destructor -- decrement
```

Increment before, decrement after. Fine.

**fcn.140009d74 at `0x14000a4e0` -- not covered:**

```asm
0x14000a4db      mov rcx, rbx
0x14000a4de      xor edi, edi
0x14000a4e0      call fcn.14000aa28            ; no lock xadd before this
0x14000a4e5      mov ebx, 0x103
```

No increment before. No decrement after. Raw pointer, no ownership, no protection. This is the one that matters.

`AfUnixDeliverDataToClient` itself also takes no reference on entry. Full function prologue was checked, no `lock xadd` anywhere before the first use of the endpoint pointer. So the caller doesn't protect it and the function doesn't protect itself. Nobody is protecting it.

---

## Unlocked Window Contents

The unlocked region sits between `ExReleasePushLockExclusiveEx` at `0x14000aaef` and `ExAcquirePushLockExclusiveEx` at `0x14000acdc`. Here is what actually runs in that window.

**`fcn.14000a968` -- IRP completion loop:**

```asm
0x14000a97c      mov rax, qword [rbx]          ; load IRP
0x14000a985      mov r8, qword [rbx+0x30]
0x14000a989      mov rcx, qword [rbx+8]
0x14000a98d      call fcn.1400025e0            ; fire completion callback
0x14000a99b      call ExFreePoolWithTag         ; free the IRP buffer
0x14000a997      mov rbx, qword [rbx+0x50]     ; next IRP
0x14000a9aa      jne 0x14000a97c               ; loop
```

Walks and frees the pending IRP list. One iteration per pending receive. queue depth is controllable from userland via overlapped WSARecv calls, which is convenient.

**`fcn.14000a9c0` -- send buffer drain loop:**

```asm
0x14000a9da      mov rax, qword [rbx+0x20]
0x14000a9eb      call fcn.1400025e0            ; fire callback per buffer
0x14000a9fb      call ExFreePoolWithTag         ; free the buffer
0x14000aa07      mov rbx, qword [rdi]           ; next buffer
0x14000aa0d      jne 0x14000a9da               ; loop
```

Same pattern but for send buffers. One iteration per backed-up send. Also controllable from userland via overlapped WSASend calls. Both loops together give you a window proportional to queue depth, which you control.

**The part that actually kills you:**

```asm
0x14000abbc      mov rax, qword [rcx+0x178]    ; endpoint+0x178
0x14000abc3      mov rcx, qword [rcx+0x180]    ; endpoint+0x180
0x14000abce      call fcn.1400025e0             ; AFD callback
```

`RCX` is the endpoint struct, loaded from `[var_8h]` which is `arg1` saved at function entry. Direct reads from the endpoint struct itself, not from some child object. If the endpoint got freed before execution reaches here, these are your UAF. Under special pool the first read faults immediately and you get full attribution.

---

## Dispatch Table Confirmation

`AfUnixTlConnectEndpointClose` confirmed as the WSK close callback from the runtime dispatch table. Found by searching the binary for the static address of the close function, locating it in the read-only data section, then relocating to the loaded module base and dumping with `dps`:

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

Module base at runtime: `fffff809'79130000`. Static address `0x140004000` relocated to `fffff809'79134000`. Every reboot shifts the base so all the addresses have to be recalculated, which gets old really fast.
