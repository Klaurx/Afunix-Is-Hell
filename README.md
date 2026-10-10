Race condition in afunix.sys, the Windows kernel AF_UNIX transport driver, that chains from a sync issue to state corruption to a timing window to a confirmed kernel UAF. Triggered from unprivileged userspace via a normal WSASend syscall.

The full repo of the state corruption can be found [here](https://github.com/Klaurx/afunix-msrc-disclosure)

what dis is;

any unpriv process can open an AF_UNIX socket and drive afunix.sys into a state where it frees a kernel endpoint struct while another thread is still executing inside it. that thread then dereferences the freed memory. confirmed live under Driver Verifier special pool with full attribution.

The chain;
```
non-atomic OR outside pushlock
    → delivery flags corrupted (+0x1c0)
        → buffer accounting permanently short-circuited
        → bit 1 resurrection via lost-update
            → cancel pending io defers indefinitely
                → fcn.140009d74 called spuriously
                    → bit 0 of +0x104 set incorrectly
                        → AfUnixDeliverEndpointSend early exit
                            → destructor called without refcount increment
                                → refcount underflow
                                    → endpoint freed with live references
                                        → UAF
```


Unlocked OR firing at AfUnixDeliverDataToClient+0x1d0 with pushlock measured not held
Hardware watchpoint showing the write firing from 3 different CPU cores on the same endpoint
Full callstack from unprivileged WSASend all the way down to the bad instruction
Flags word latching to 0x00000003 and never recovering
Refcount hitting zero inside AfUnixTlConnectEndpointClose confirmed live
FREE(P=ffffe50c7f40cdd0) followed by POST-FREE DEREF(P=ffffe50c7f40cdd0) at AfUnixDeliverDataToClient+0x194 in a single continuous run under Driver Verifier special pool

The UAF callstack
```
afunix!AfUnixDeliverDataToClient+0x194   ← mov rax,[rcx+0x178] on freed endpoint
afunix!AfUnixDeliverEndpointSend+0x1e4
afunix!AfUnixTlConnectEndpointSend+0x118
afd!AfdTLStartBufferedVcSend+0xd8
afd!AfdFastConnectionSend+0x497
afd!AfdFastIoDeviceControl+0xe1e
nt!IopXxxControlFile+0x431
nt!NtDeviceIoControlFile+0x5e
nt!KiSystemServiceCopyEnd+0x25
```

Normal unprivileged WSASend crossing the syscall boundary, going through AFD, reaching afunix, dereferencing freed kernel pool memory.

Affected component:
```
Binary:   afunix.sys
SHA256:   217317a359147fe6b0855f4b5abde876a21ca39c0760844026b4922e049e7ceb
PDB GUID: CDF7B3740E839EBFFBF7F38A35024F391
Version:  10.0.28000.2804
OS:       Windows 11 Insider Preview
```

# Repo structure
```
README.md — this file
bug/static-analysis.md — full disassembly evidence, locking audit, lost-update scenario (existing)
bug/live-confirmation.md — WinDbg output, watchpoints, FREE to DEREF proof
bug/uaf-chain.md — the full UAF geometry, ref bracket analysis, window measurements
poc/measure.c — window measurement and UAF trigger
poc/measurement2.c
poc/measurement3.c
logs/afunix_uaf_1d30_2026-10-08_09-21-47-210
logs/Screenshot 2026-10-08 092245
logs/Screenshot 2026-10-08 075136
logs/Screenshot 2026-10-07 024209
logs/Screenshot 2026-10-02 102755
logs/Screenshot 2026-10-02 131626
```

UAF confirmed after closure during continued independent research.
Ceiling is unknown beyond UAF. non more claimed here. 
This repo exists so the bug is documented publicly regardless of what MSRC decides to do with it.

MSRC

They closed it. Twice effectively. Make of that what you will.
