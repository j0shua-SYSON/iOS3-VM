# Activation and guest code signing — do either kill M5?

The roadmap named two things that could kill the jump to userspace outright, and
left both uninvestigated: **activation** (a stock iPhone OS 3 sits at "connect to
iTunes" until `lockdownd` is satisfied) and **guest code signing** (3.1.3 refuses
to run unsigned binaries). This document retires both.

Every factual claim is labelled, exactly as `networking.md` does it:

- **CONFIRMED** — read directly out of the user's own `iPhone1,2_3.1.3_7E18`
  materials in `firmware/` (the decrypted `kernel.macho`, `devicetree.bin`, or the
  IPSW), by the commands and byte-offsets shown, or read out of a public
  **open-source reimplementation** whose text is quoted. Reproducible.
- **INFERRED** — follows from something confirmed plus how XNU / lockdownd of this
  vintage is documented to work. Strong, but not proven against these exact bytes.
- **GUESS** — plausible, unverified, flagged so it does not become an assumption.

The headline, up front, because it changes how M5 should be planned:

> **Guest code signing is KILLED — it is no longer a threat.** The check is real,
> but it is Apple's own development switch, and we are iBoot: we own the two inputs
> that turn it off. This is CONFIRMED against the 7E18 kernelcache down to the
> instruction.
>
> **Activation is MANAGEABLE, and softer than feared.** Two things de-fang it.
> First, an unactivated device still *launches and renders SpringBoard* — it shows
> the activation lock screen, not a black hole — so M5's "SpringBoard renders"
> milestone does **not** depend on activation at all. Second, we build the root
> filesystem and code signing is off, so reaching the actual home screen needs
> only a file we write or a byte we patch, never a record off real hardware.
>
> **The one honest gap:** the activation mechanism below is confirmed against the
> open-source `lockdownd` *protocol* and the historical jailbreak record, **not**
> yet against the 3.1.3 `lockdownd` *binary*. §B.6 gives the turnkey recipe to
> close that gap — nothing in it is hard, and none of it changes the verdict.

---

## Status update — what has changed since this was written

Two things, and neither overturns a verdict.

**1. The root filesystem is no longer encrypted or hypothetical.** This document
was written when `lockdownd` "lives on the encrypted root filesystem and the
RootFS key was not present in the repo". It is present now: a decrypted **HFSX**
volume (`H X` at offset 1024, version `10.0`, 433,274,880 bytes) sits in
`firmware/`, and the kernel **mounts it** — `BSD root: md0, major 2, minor 0`.
Steps 1–3 of §B.6's recipe are therefore done; only step 4 (read the strings out
of `lockdownd`) remains, and it no longer needs a key.

**2. The code-signing path has now been executed, and survived — without either
switch.** §A.3 predicted a "second, page-granularity layer
(`cs_enforcement_disable` in `vm_fault_enter`) [that] governs the mid-execution
kill of invalid pages", and judged it off the critical path. It turned out to be
squarely on it, and the story went through two stages:

*First*, pid 1's exec reached `cs_validate_page` and the hash of launchd's first
text page **mismatched**, so `vm_fault_enter` → `cs_invalid_page` → `psignal`
spun ~95,000 times with `_unix_syscall` never reached. §A.3's page-granularity
layer, exactly as described, actively killing an image.

*Then* it was found not to be a signing problem at all. The bytes were never
wrong: two from-scratch verifications (a UDIF `blkx`/CRC32 verifier, and an HFSX
reader checking code-directory page hashes for all 155 signed Mach-Os and 6,731
code pages on the volume) found zero mismatches. `cs_validate_page` hashes exactly
4096 bytes, and `SHA1UpdateUsePhysicalAddress` routes exactly-4096-byte buffers to
a **hardware** SHA-1 engine whenever `IOCryptoAcceleratorFamily` has installed its
hook — an engine at 0x38000000 that we do not model, so the digest was fabricated.
Keeping that kext off the nub restores software SHA-1.

Three consequences, stated plainly:

- **The document's structural reading of the enforcement layers is corroborated**
  by a running kernel, which is more than it claimed for itself.
- **The verdict "KILLED" is still a reading of the kernel's code, not a
  demonstration.** Neither `/chosen/debug-enabled = 1` nor
  `cs_enforcement_disable=1` / `amfi_get_out_of_my_way=1` has yet been set in an
  actual boot; the standard recipe is `debug=0x8 serial=1 nand-enable-adm=0`.
  Until one of those runs, §A remains CONFIRMED-by-disassembly and untested.
- **But the practical question is answered better than the switch would have
  answered it.** `cs_validate_page` now runs 15 times over launchd's pages and
  validates every one; `cs_invalid_page` and `psignal` are never reached; pid 1
  executes user-mode code and issues system calls. Apple's own signatures verify
  against Apple's own bytes on our emulated hardware, with enforcement *on*. That
  makes the switches in §A a contingency rather than a dependency — and it is a
  reminder that a signature failure in this emulator is far more likely to be a
  device we have not modelled than a policy we need to defeat.

---

# Part A — Guest code signing (Risk 2): **KILLED**

## A.1 What actually enforces signing, and where it lives

There are **two** independent enforcement layers on 3.1.3, and it matters to keep
them apart because the boot-args hit different ones.

**Layer 1 — the exec-time signature check, owned by AMFI.** `AppleMobileFileIntegrity`
is a **separate kext**, prelinked into the kernelcache — not part of the kernel
proper. CONFIRMED from strings in `__PRELINK_TEXT`:

```
com.apple.driver.AppleMobileFileIntegrity
/SourceCache/AppleMobileFileIntegrity/AppleMobileFileIntegrity-63.0.1/AppleMobileFileIntegrity.cpp
static int AppleMobileFileIntegrity::AMFI_vnode_check_signature(vnode*, label*, unsigned char*, void*, int)
AMFI: mac_policy_register() error: %d
AMFI: Invalid signature but permitting execution
```

So the version is **AMFI-63.0.1**, it registers as a **MAC policy**
(`mac_policy_register`), and it implements `AMFI_vnode_check_signature`. That method
is AMFI's handler for the kernel's `mac_vnode_check_signature` MACF hook — the hook
XNU calls when a binary is exec'd, to ask policy "is this code allowed to run?".
The kernel provides the *hook*; **AMFI provides the *answer*.** The MACF hook table
is CONFIRMED present in the kernel proper by symbol (`nm`-style dump of
`kernel.macho`'s `LC_SYMTAB`): `_mac_vnode_check_signature` at `0xc01aca44`,
`_mac_proc_check_run_cs_invalid` at `0xc01ab150`, and the whole `mac_vnode_check_*`
/ `mac_proc_check_*` family.

**Layer 2 — the page-fault validation kill, owned by the kernel.** As executable
pages fault in, XNU hashes each against the code-directory and, on a mismatch, can
kill the process. This lives in the kernel proper, gated by the global
`_cs_enforcement_disable` (`0xc020daac`, in `__DATA`). CONFIRMED: the only two code
references to that address in the entire kernelcache are both inside `_vm_fault_enter`
(`0xc003cac4`), the VM page-fault entry path.

## A.2 The switch, traced end to end

The claim in `networking.md` §1.5 was that
`/chosen/debug-enabled = 1` plus boot-args `cs_enforcement_disable=1
amfi_get_out_of_my_way=1` disables enforcement, labelled INFERRED. It is now
**CONFIRMED by disassembly**. The chain has four links, each verified:

**1. `/chosen/debug-enabled` → the `debug_enabled` global.** The device tree carries
`debug-enabled` (CONFIRMED: it is one of the security properties in
`devicetree.bin`, alongside `secure-boot`, `production-cert`, `development-cert`),
a 4-byte property currently zero. XNU copies it into the kernel global
`_debug_enabled` (`0xc0239384`) at boot.

**2. `debug_enabled` → `PE_i_can_has_debugger()`.** Disassembling
`_PE_i_can_has_debugger` (`0xc01a830c`, Thumb) shows it does nothing but return that
global — the literal it loads (`[pc,#0x1c]` at `0xc01a8330`) is `&debug_enabled`,
and every return path ends `ldr r0,[r2]; bx lr`:

```
c01a8310  ldr  r2,[pc,#0x1c]   ; r2 = &debug_enabled (0xc0239384)
c01a8312  ldr  r3,[r2]
...        (stores current value through the caller's out-ptr)
c01a832a  ldr  r0,[r2]         ; return *debug_enabled
c01a832c  bx   lr
```

**3. `PE_i_can_has_debugger()` gates whether AMFI even *reads* its boot-args.** This
is the crux, and it is unambiguous in AMFI's policy-init routine (ARM, at
`0xc044e6c0`):

```
c044e724  mov  r0,#0
c044e728  ldr  r3,[pc,#0x24c]  ; r3 = PE_i_can_has_debugger  (pool @c044e97c = 0xc01a830d)
c044e72c  blx  r3
c044e730  cmp  r0, #0
c044e738  beq  #0xc044e858     ; if !debug-enabled -> SKIP the entire boot-arg block
   ... PE_parse_boot_argn("amfi_unrestrict_task_for_pid", ...)  ; pool @c044e980 = PE_parse_boot_argn
   ... PE_parse_boot_argn("amfi_allow_any_signature",    ...)
   ... PE_parse_boot_argn("amfi_get_out_of_my_way",      ...)
   ... PE_parse_boot_argn("cs_enforcement_disable",      ...)
c044e858: (continues, boot-args ignored)
```

The four argument-name strings sit consecutively in AMFI's literal pool
(`0xc044e988…0xc044e99c`), each paired with the log it prints when set —
`"%s: unrestricted task_for_pid enabled by boot-arg"`,
`"%s: signature enforcement disabled by boot-arg"`,
`"%s: cs_enforcement disabled by boot-arg"`. When the gate at `c044e738` is taken
(debugger not enabled) **none of them are parsed**; the four boot-args are simply
inert on a production-fused device. Set `debug-enabled=1` and they are honoured.
This is the entire reason development devices could run unsigned code and retail
ones could not — and we stand in for iBoot, so we write `debug-enabled` ourselves.

**4. When honoured, AMFI's check permits the binary.** `amfi_get_out_of_my_way` /
`amfi_allow_any_signature` make `AMFI_vnode_check_signature` return success for a
binary whose signature is missing or invalid, logging the CONFIRMED string
`"AMFI: Invalid signature but permitting execution"`.

For the page-fault layer, the polarity is likewise CONFIRMED. The real read of
`_cs_enforcement_disable` is at `0xc003cb46` inside `_vm_fault_enter`:

```
c003cb46  ldr  r3,[pc,#0x2f0]  ; &cs_enforcement_disable
c003cb4a  ldr  r2,[r3]
c003cb56  cmp  r2, #0
c003cb58  beq  #0xc003cb5c     ; ==0 : fall into the enforcement path
c003cb5a  b    #0xc003cd00     ; !=0 : branch past it (kill suppressed)
```

Non-zero disables the page-kill. Exactly the shape `dt_set_u32()` and our boot-arg
writer already produce.

## A.3 One honest subtlety, and why it does not bite

The kernel global `_cs_enforcement_disable` **ships as 0** (CONFIRMED: the word at
file offset `0x205aac` is `0x00000000`) and I found **no writer for it anywhere in
the kernelcache** — not a literal-pool pointer, not a `movw/movt` immediate, in
either the kernel proper or AMFI. AMFI parses a boot-arg *named*
`cs_enforcement_disable` but stores the result in its own policy object (`[r5+0x64]`),
not into this global. So two readings are possible, and I cannot yet decide between
them from these bytes:

- **INFERRED (likely):** the page-fault kill only fires for pages that belong to a
  code-signed mapping (one with a code-directory blob). A genuinely *unsigned*
  binary has no blob, so `vm_fault_enter` has nothing to validate and never reaches
  the kill — meaning the AMFI exec-time allow in §A.2 is already sufficient for our
  case, and this global is simply irrelevant to it.
- **GUESS:** some init path I did not trace propagates AMFI's flag into the global.

Either way it is not on the critical path, because we control three redundant
knobs and only need one to land:

1. `amfi_get_out_of_my_way=1` (exec-time allow — the layer that actually gates
   unsigned exec).
2. Ad-hoc sign our own binaries with `ldid` so their page hashes are valid — the
   standard jailbreak move — which sidesteps the page-fault layer entirely.
3. We load the kernelcache, so if the page-kill ever bites we can flip the shipped
   `0` to `1` at file offset `0x205aac` — a one-word patch, the same class of edit
   `tools/bootkernel.c` already performs on the device tree.

## A.4 Answers to the three questions

1. **Do the boot-args / `debug-enabled` disable signing for *userspace* (not just
   kexts)? What is the check and what does it gate?** — **CONFIRMED, yes, userspace.**
   The gate is AMFI's `AMFI_vnode_check_signature`, the policy answer to the
   `mac_vnode_check_signature` hook that XNU calls on **every exec of a userspace
   Mach-O**. It is disabled by `amfi_get_out_of_my_way` / `amfi_allow_any_signature`,
   which AMFI only reads when `PE_i_can_has_debugger()` (i.e. `/chosen/debug-enabled`)
   is true. A second, page-granularity layer (`cs_enforcement_disable` in
   `vm_fault_enter`) governs the mid-execution kill of invalid pages. None of this
   is kext-specific — it is the userspace path.

2. **Is AMFI a separate kext, and what does it enforce that the kernel does not?**
   — **CONFIRMED separate kext** (`com.apple.driver.AppleMobileFileIntegrity`,
   AMFI-63.0.1, prelinked). The kernel supplies the *mechanism*: the MACF hook
   dispatch table and the page-hashing primitives (`cs_validate_page`,
   `_load_code_signature`, `_find_code_signature` are all kernel symbols). AMFI
   supplies the *policy* — the actual allow/deny for signatures, plus
   entitlement-gated decisions the kernel has no opinion on: `task_for_pid`
   restriction (`amfi_unrestrict_task_for_pid`), running invalidated code
   (`mac_proc_check_run_cs_invalid` → "AMFI: run invalid not allowed"), and
   entitlement lookups (`AppleMobileFileIntegrityUserClient::getEntitlements…`).
   Without AMFI registered, the hooks have no policy behind them.

3. **What is required to run an unsigned userspace binary?** — Two device-tree /
   boot-arg writes we already know how to make, both performed by us as iBoot:

   ```
   device tree:  /chosen/debug-enabled = 1        (tools/bootkernel.c dt_set_u32)
   boot-args:    cs_enforcement_disable=1 amfi_get_out_of_my_way=1 amfi_allow_any_signature=1
   ```

   `amfi_allow_any_signature` is belt-and-braces alongside `amfi_get_out_of_my_way`;
   `cs_enforcement_disable` covers the page-fault layer. For our own injected
   binaries, additionally ad-hoc-sign with `ldid` so no layer has cause to complain.
   This is milestone **N2**'s de-risking already, and it is the same recipe that
   made `networking.md`'s kext plan viable.

---

# Part B — Activation (Risk 1): **MANAGEABLE**

## B.1 What "activated" means to lockdownd

On a real device `launchd` starts `lockdownd`, and SpringBoard asks it whether the
phone is activated. The check is a single lockdown value. **CONFIRMED from
libimobiledevice**, the open-source reimplementation of Apple's `lockdownd`
protocol — quoting its `ideviceactivation` tool verbatim:

```c
lockdownd_get_value(lockdown, NULL, "ActivationState", &state);
...
if (state_str && strcmp(state_str, "Unactivated") != 0) { /* already activated */ }
```

So the global lockdown value is **`ActivationState`**, and `"Unactivated"` is the
sentinel for the not-yet-activated device. The activation *transaction* is likewise
CONFIRMED from libimobiledevice's `lockdown.c` / `lockdown.h`:

- `lockdownd_activate(client, activation_record)` sends a request `"Activate"` with
  an `"ActivationRecord"` plist. Its documentation: *"The ActivationRecord plist
  dictionary must be obtained using the activation protocol requesting from Apple's
  https webservice."*
- The device can reject it: error codes `"InvalidActivationRecord"` /
  `"MissingActivationRecord"` (`LOCKDOWN_E_INVALID_ACTIVATION_RECORD`).
- `lockdownd_deactivate()` is documented as *"Deactivates the device, returning it
  to the locked 'Activate with iTunes' screen."* — note that word **screen**: the
  UI is up and rendering; deactivation drops it back to a lock screen, it does not
  tear SpringBoard down.

## B.2 Where the state lives on the device

This is the part I could **not** confirm against the 3.1.3 binary (see the caveat
in the headline and §B.6). What follows is **INFERRED** from the documented
lockdownd file layout that libimobiledevice/usbmuxd tooling and the era's
jailbreak tools (PwnageTool, redsn0w, blackra1n) all target, cross-checked against
the protocol facts in §B.1 which *are* confirmed:

- `/var/root/Library/Lockdown/data_ark.plist` — lockdownd's persistent property
  store ("data ark"). The value read out as `ActivationState` is a key in here.
  Writing `ActivationState = Activated` (or `FactoryActivated`) into this file is
  the record-free activation the era's tools performed. **INFERRED.**
- `/var/root/Library/Lockdown/activation_record.plist` — the Apple-signed record
  applied by `lockdownd_activate` (the `AccountToken`, its RSA signature, and a
  device certificate). Present on an activated device; **its signature is what a
  from-Apple activation would satisfy.** **INFERRED.**
- `/var/root/Library/Lockdown/device_private_key.pem` +
  `device_certificate.pem` — the device's lockdown identity, generated first boot,
  unrelated to activation state per se. **INFERRED.**

The three named paths, and the exact `Activated`/`Unactivated`/`FactoryActivated`
spelling of the state, are the specific items §B.6 will verify or correct against
the real binary. The **protocol** around them (§B.1) is not in doubt.

## B.3 It is NOT enforced by anything we already control at boot

The task asked whether a device-tree property or boot-arg could switch activation
off. **CONFIRMED no.** `devicetree.bin` contains only boot-chain security
properties — `secure-boot`, `production-cert`, `development-cert`, `debug-enabled`,
`secure-root-prefix` — and **nothing activation-related**. Activation is decided
entirely in userspace by lockdownd reading a userspace plist; the kernel and device
tree have no opinion on it, and `kernel.macho` contains no activation logic
(CONFIRMED — no matching symbols or strings). So activation cannot be waved away
the way code signing can; it has to be satisfied *in the root filesystem*. The good
news is that the root filesystem is precisely the thing we construct.

## B.4 Does SpringBoard refuse to launch, or launch into the activation UI?

**It launches and renders** — INFERRED, but strongly, and this is the single most
important finding for M5. Three independent supports:

- `lockdownd_deactivate`'s own documentation (§B.1) calls the unactivated state a
  **"screen"** the device is "returned to". A screen is drawn by SpringBoard.
- The historical behaviour of every iPhone OS 1–3 device is the "Slide for
  emergency" / "Connect to iTunes" **lock screen**, which is a SpringBoard screen
  (SpringBoard owns the lock screen and the emergency dialer). SpringBoard is up;
  it is gated at the *unlock-to-home* step, not at launch.
- On this era there is no separate "Setup"/activation app that replaces SpringBoard
  (that model arrives with iOS 5's `Setup.app`); on 3.x SpringBoard itself presents
  the activation lock screen.

**Consequence for the roadmap:** M5's criterion 3 — *"SpringBoard renders the home
screen into the framebuffer"* — splits into two. *SpringBoard renders* is reachable
with **zero** activation work: an unactivated boot should paint the activation lock
screen, which is already the shareable "a 2009 phone's UI, rendered by our
emulator" artefact. *The home screen specifically* is what needs §B.5. That is a
much better position than "no activation ⇒ no pixels", which is what the risk
feared. **GUESS:** the activation lock screen may itself try to reach
`albert.apple.com` over TLS and log a failure; that does not stop it drawing, and
it is dead-endpoint/dead-protocol regardless (see `networking.md` §6.3).

## B.5 The cheapest route to a home screen

In increasing order of effort — and we can stop at the first that works, because we
own the root filesystem and (Part A) code signing is off:

**Route 0 — do nothing.** Boot unactivated, get the SpringBoard **activation lock
screen**. Satisfies "SpringBoard renders" for M5. Costs nothing. **Confidence: high.**

**Route 1 — pre-seed the data ark. ⭐ recommended.** Drop a
`/var/root/Library/Lockdown/data_ark.plist` into the root filesystem image we build,
carrying `ActivationState = Activated` (and `ActivationStateAcknowledged = true`,
which `ideviceactivation` sets after a real activation — CONFIRMED it writes that
key). No Apple record, no network, no physical device — just a file placed in an
image we are already assembling. This is the record-free hacktivation the era's
tools did, reduced to a build step because we author the filesystem offline.
**Confidence: INFERRED-high**, pending the §B.6 confirmation that 3.1.3 keys on this
exact file/value and does not additionally require the Apple-signed
`activation_record.plist` to be present and self-consistent.

**Route 2 — patch lockdownd.** If 3.1.3's lockdownd insists on a signature it cannot
get offline, patch the one branch that maps "no/!valid record" → `Unactivated` so it
yields `Activated`. Code signing is off (Part A), so a patched `lockdownd` runs
unmodified-kernel. This is exactly what PwnageTool's "activate" option did in 2009;
it is a few bytes once the binary is in hand. **Confidence: INFERRED-high** it is
always available as a fallback; the *specific* bytes are not known until §B.6.

**Route 3 — the real record.** Not applicable: `gs.apple.com` / `albert.apple.com`
for 3.1.3 are long dead and speak TLS a 2026 host can't easily re-offer. Named only
to close it off. Routes 1–2 are strictly better and need nothing from Apple.

Because Route 0 already yields a rendered SpringBoard and Routes 1–2 are file/byte
edits on an artefact we construct with signing disabled, **activation does not gate
whether M5 is possible — only how many home-screen icons appear versus a lock
screen.** That is the definition of MANAGEABLE, not a killer.

## B.6 The one thing left to verify, and exactly how

Everything in §B.2 and the byte-level specifics of Routes 1–2 is INFERRED because
`lockdownd` had not been read. **Steps 1–3 below are now done** — a decrypted
HFSX volume is in `firmware/` and the kernel mounts it as `md0` — so only step 4
is outstanding, and it needs no key. The recipe is kept in full because it
records how the container was established:

1. **Extract** the rootfs DMG: it is member `018-6482-014.dmg` in the IPSW
   (CONFIRMED 208,195,584 bytes). `tools/ipsw_explore.py <ipsw> -x 018-6482-014.dmg`.
2. **Decrypt.** CONFIRMED by parsing its header: it is a standard **`encrcdsa`
   v2** (vfdecrypt) image — magic `encrcdsa`, version 2, AES-128-CBC, block size
   4096, payload at offset `0x1e000`, 208,071,227 bytes. With the published 36-byte
   RootFS vfdecrypt key (16-byte AES-128 key ‖ 20-byte HMAC-SHA1 key): for each
   block *i*, `IV = HMAC-SHA1(hmac_key, BE32(i))[:16]`, then
   `AES-128-CBC-decrypt(aes_key, IV, block)`. This is ~20 lines against the
   `cryptography` module (available here) or a tiny reuse of the AES already in
   `core/src/firmware/`. The RootFS key is on theapplewiki's 7E18 iPhone1,2 key
   page — the same source the project already assumes for iBoot/kernel keys
   (`docs/BOOT_CHAIN.md`), dropped in via `keys.json`.
3. **Mount/parse.** The decrypted image is an Apple Partition Map wrapping an
   **HFSX** volume (CONFIRMED by the prior investigation this task cited). Parse the
   APM, then the HFS+ catalog B-tree, to pull `/usr/libexec/lockdownd`,
   `/System/Library/PrivateFrameworks/` (the MobileActivation/lockdown frameworks),
   and `/var/root/Library/Lockdown/` if the image is a provisioned one.
4. **Inspect — strings first, disassembly only if needed.** `strings` on
   `lockdownd` will name the plist paths, the keychain/`data_ark` keys, and the
   literal `ActivationState` values it compares. That alone confirms or corrects
   §B.2 without touching a disassembler — the same "read the strings, trust the
   bytes" discipline that recovered the timer map and the AMFI switch above.

Nothing in that recipe is research; it is an afternoon of plumbing that upgrades
§B.2 and Routes 1–2 from INFERRED to CONFIRMED. It does not change the verdict — it
pins the exact filenames.

**And a host-side HFSX reader now exists**, built to exonerate the disk image
during the launchd signature investigation: it walks the catalog and verified
code-directory page hashes for all 155 signed Mach-Os on the volume. Step 4 is
therefore no longer "write a tool and then read `lockdownd`" — it is just reading
`lockdownd`.

---

# Verdicts

| Risk | Verdict | Basis |
|---|---|---|
| **Guest code signing** | **KILLED** *(on paper — not yet exercised in a boot)* | CONFIRMED to the instruction against the 7E18 kernelcache: AMFI is a separate kext whose exec-time signature check is switched off by boot-args it reads only when `/chosen/debug-enabled` is set — and we are iBoot. No boot has yet set them; see the status update at the top. |
| **Activation** | **MANAGEABLE** | Unactivated still renders SpringBoard (the activation lock screen), so M5's "renders" goal is independent of it (INFERRED-strong). The home screen needs only a data-ark file we write, or a lockdownd patch (signing is off) — never a physical-device record. Protocol CONFIRMED from open source; the exact on-device filenames INFERRED pending §B.6. |

**What would change the activation verdict to STILL-A-KILLER:** only if 3.1.3's
`lockdownd` refused to treat a locally-written `ActivationState=Activated` as
authoritative *and* cross-checked the Apple-signed `activation_record.plist`'s
signature before allowing unlock, *and* that same check could not be patched out of
the binary. All three would have to hold at once. Given that (a) we can patch the
binary freely because code signing is off, and (b) the era's tools activated
record-free in practice, this is **improbable** — but it is the specific thing §B.6
should look for first when the RootFS key is available. If it somehow held, the
fallback is still a rendered SpringBoard lock screen (Route 0), i.e. M5's visual
goal survives even the worst case.
