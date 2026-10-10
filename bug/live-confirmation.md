went in to measure the duration of the unlocked window inside ```AfUnixDeliverDataToClient```. came out wit a UAF. rhis document covers how that happened.

The Starting Point

We had already confirmed the race and the state corruption in the original report. The open question was whether the unlocked window between ```0x14000aaef``` and ```0x14000acdc``` was wide enough and accessible enough for a concurrent close to land inside it and free the endpoint while a delivery thread was still executing inside it.

To answer that we set two timestamp breakpoints:

```
bp <aaef> ".printf \"ENTER %p\n\", @$ttime; g"
bp <acdc> ".printf \"EXIT %p\n\", @$ttime; g"
```
And built two PoCs to stress the window from different angles.

# measurement2.c

The first measurement PoC. posts 64 overlapped WSARecv on the accept side without draining them, floods the send side with 64 x 65536 byte sends to back up the queue, then cancels the receives one at a time with Sleep(0) between each cancel.

The goal was to see if queue depth increased the time between ENTER and EXIT. The cancel burst was the mechanism, each cancellation drives one iteration of the IRP completion loop inside the unlocked window, and with 64 pending receives that loop has 64 iterations of work to do before the window closes.

The process stays alive indefinitely after the cancel burst so the kernel activity can be observed without the socket pair tearing down.

# measurement3.c

The second measurement PoC, built after measurement2 to sustain the activity rather than run it once. Does the same initial population, 64 pending receives, 64 large sends, staggered cancel burst, but then keeps going with two background threads:

sender_thread = continuous WSASend loop with 1ms sleep between sends, keeps the send queue backed up permanently
receiver_thread = continuous WSARecv + 10ms wait + CancelIoEx loop, keeps the receive/cancel cycle running indefinitely

This keeps the AF_UNIX delivery path active after the initial burst so the window stays exercised across the full measurement period rather than just during the one-time cancel burst.

# The Pivot

While hammering the path with measurement3, we started seeing repeated hits at:
```
afunix!AfUnixDeliverDataToClient+0x194
mov rax, qword ptr [rcx+0x178]
```

made us stop asking “how long is the window” and start asking “what is RCX here and is it still valid.”

We put a conditional breakpoint on the dereference to capture the object:
```
bp <+0x194> ".if (@rcx == TARGET) { .printf \"*** DEREF P=%p ***\n\", @rcx; } .else { g }"
```
That gave us a concrete pointer — P = ```ffffe50c7f40cdd0```.

Following P Backwards

P is an AfUnixEndpoint. We found the reference decrement and free inside AfUnixEndpointDereference:
```
lock xadd qword ptr [rcx], rax   ; decrement
test rax, rax                     ; check if zero
jne ...                           ; skip free if still live

; refcount hit zero:
mov rcx, rdi
call ExFreePoolWithTag            ; free P
```
We added three breakpoints conditional on RDI == P:
```
bp <refcount zero> ".if (@rdi == P) { .printf \"*** REF0 P=%p ***\n\"; } .else { g }"
bp <ExFreePoolWithTag> ".if (@rdi == P) { .printf \"*** FREE P=%p ***\n\"; } .else { g }"
bp <+0x194> ".if (@rcx == P) { .printf \"*** DEREF P=%p ***\n\"; } .else { g }"
```
Now we could watch the sequence: refcount hits zero, free fires, then check if the dereference happens after.

# The Decisive Breakpoint

To eliminate any ambiguity we changed the FREE breakpoint to dynamically arm a one-shot breakpoint at the dereference site the moment the free fired:
```
bp <FREE> ".if (@rdi == P) {
    .printf \"*** FREE P=%p ***\n\", @rdi;
    bp /1 <+0x194> \".if (@rcx == P) {
        .printf \\\"*** POST-FREE DEREF P=%p ***\\n\\\", @rcx;
        k;
    } .else { g }\";
    g
} .else { g }"
```
One continuous run. Free fires, one-shot arms, execution continues, dereference fires.

The Output
```
*** FREE P=ffffe50c7f40cdd0 ***
*** POST-FREE DEREF P=ffffe50c7f40cdd0 ***
```
Same pointer. Free then deref. In a single continuous run under Driver Verifier special pool.

The Callstack at POST-FREE DEREF
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

Normal unprivileged WSASend crossing the syscall boundary, going through AFD, reaching afunix, executing mov rax, [rcx+0x178] on a freed endpoint struct.

# confirmed things

The endpoint has no refcount bracket in this call path ```AfUnixDeliverDataToClient``` takes no reference on entry, and the caller (fcn.140009d74 at 0x14000a4e0) takes none either


The pushlock is dropped mid-function at 0x14000aaef and not reacquired until ```0x14000acdc```, a concurrent close acquires it instantly during that window and runs to completion including the free


The dereference at +0x194 reads from the endpoint struct directly, with Driver Verifier special pool enabled the page is unmapped on free, making this a guaranteed fault if the timing lands


The entire path is reachable from an unprivileged WSASend with no special setup beyond opening an AF_UNIX socket pair
