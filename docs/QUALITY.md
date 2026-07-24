# Quality and validation

This file records what was actually checked for the post-run18 TV-out,
framebuffer-planning, CLCD-bounds, and boot-diagnostic change at commit
`afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9`. It separates local engineering
evidence from hosted CI and real-firmware evidence so that a green unit test
is never presented as a SpringBoard boot.

## Current verdict

- The complete Release build succeeded locally.
- All 23 registered CTest tests passed on the final tree after the snapshot
  edge-case patch.
- The affected binaries reported **5,498 passed, 0 failed** for `test_soc` and
  **468 passed, 0 failed** for `test_snapshot`. A final targeted CTest gate for
  `s5l8900_machine`, `snapshot`, and `external_md_cli_preflight` passed
  **3/3**.
- `external_md_cli_preflight`, strict compiler checks, diagnostic analyzer
  checks, and zero-step tool smokes passed as recorded below.
- Hosted GitHub Actions passed for the exact commit: the
  [core run](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519878)
  was green across Linux, macOS, Windows, warnings-as-errors, ASan+UBSan, and
  JIT jobs; the
  [unsigned iOS run](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519892)
  was also green.
- Run19, the first post-change real-firmware validation, is **pending**. Nothing
  here proves that IRQ 30 reaches the shipped handler, that `IOServiceClose`
  returns, that SpringBoard renders, or that the iOS app is stable on-device.

## Evidence ledger

| Check | Result | What it establishes | What it does not establish |
|---|---|---|---|
| Full CMake Release build | Passed locally | All configured core tests and host tools compiled and linked together | Cross-platform or iPhone compilation |
| Full Release CTest suite | **23/23 passed** on the final tree | The public firmware-free suite, including storage, firmware parsers, CPU, SoC, snapshot, framebuffer bridge, and CLI preflight, was green after the final snapshot edge patch | Private-firmware or device behavior |
| Final focused unit binaries | `test_soc`: **5,498/0**; `test_snapshot`: **468/0** | The final TV-out, IRQ/WFI, MMIO, CLCD, and snapshot code in `afa650e` passed its affected assertions | Real Apple driver behavior |
| Final targeted CTest | **3/3 passed**: `s5l8900_machine`, `snapshot`, `external_md_cli_preflight` | The final linked CMake targets and startup preflight pass after the hosted gate | Private-firmware execution |
| `external_md_cli_preflight` | Passed | Startup self-checks pin framebuffer PA `0x0885c000` and `topOfKernelData` `0x088f4000`; incompatible external-md/tree/RAM/root/snapshot combinations fail closed before firmware is opened | A rootfs copy, guest boot, or long storage run |
| Strict GCC pass | Passed with `-std=c11 -Wall -Wextra -Werror` on the changed core and focused test sources | The affected portable-C paths are warning-free under the local GCC frontend | Clang, MSVC, Xcode, sanitizers, or runtime behavior |
| Targeted diagnostic warnings | Relevant `-Wformat=2` and `-Wconversion` checks passed | New diagnostic formatting and selected conversion-sensitive paths were checked more strictly than the default build | A whole-repository conversion-clean guarantee |
| GCC static analyzer | Passed for the changed `bootkernel` diagnostic paths | No analyzer finding remained in the new TV-out/framebuffer diagnostic control flow | Proof that the analyzer models every guest/host interaction |
| Zero-step tool smokes | `bootkernel` and `snapboot` passed | Startup invariants, option/report plumbing, and the new non-invasive state diagnostics execute without retiring guest instructions | Any emulated-time progress or firmware stability |
| [Hosted core run 30088519878](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519878) | Passed for exact commit `afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9` | Linux, macOS, Windows, warnings-as-errors, ASan+UBSan, and JIT jobs were green in GitHub Actions | Private-firmware execution or an on-device boot |
| [Hosted iOS run 30088519892](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519892) | Passed for the same exact commit | The unsigned iOS workflow built successfully on the hosted runner | Installation, signing, JIT entitlement activation, or iPhone runtime stability |
| Diff hygiene | `git diff --check` passed after the documentation update | No whitespace-error patch was introduced | Markdown rendering on every client |

The full 23-test result, the focused assertion totals, the final targeted 3/3
gate, and hosted CI all describe the committed implementation. Run19 remains
the first real-firmware verdict for the new model.

## Edge coverage in the final focused tests

### TV-out MMIO and interrupt behavior

- All three independent 4 KiB banks are present at `0x39100000`,
  `0x39200000`, and `0x39300000`, with exact window names and sizes.
- Reset exposes hardware-derived ready bit 1 while stopped, ignores guest
  attempts to store that bit, and preserves unrelated control bits such as the
  mixer's bit 2.
- Byte, halfword, and word accesses use little-endian lanes. An unaligned
  halfword crossing two backing words round-trips, while an access crossing a
  4 KiB bank boundary is rejected and counted as unmapped.
- VSYNC is generated only after all three run gates are active. SDO pending bit
  0 latches, its mask gates the level without destroying pending state, and a
  byte write-one-to-clear acknowledgement deasserts it.
- A run-gate transition resets phase but does not silently consume a latched
  completion. Restart does not manufacture an immediate stale VBlank.
- Large tick intervals preserve the number of elapsed boundaries and residual
  phase. VIC0 line 30 reaches the CPU; mixer acknowledgement does not fabricate
  IRQ 38.
- WFI advances to the next deliverable TV-out boundary without adding retired
  instructions, and existing earliest-edge behavior remains covered against
  timer and CLCD activity.

### Snapshot format and malformed state

- Snapshot format v4 serializes all three TV-out register banks, frame period,
  residual phase, and frame counter; independent field tests verify the round
  trip.
- Invalid `frame_accum >= frame_ticks`, residual phase while stopped, a stored
  hardware-owned ready bit, unsupported SDO status bits, and nonzero
  nonasserting mixer status are rejected.
- Every malformed save case starts with non-null output sentinels and proves
  that failure returns no allocation and a zero length.
- A legitimate pending VSYNC may remain latched while stopped until explicit
  W1C and is accepted. Version mismatch, malformed structure, checksum, and
  transactional-load behavior remain covered by the broader snapshot suite.

### Framebuffer planning and CLCD bounds

- The external-md startup invariant requires the 320x480x4 framebuffer range
  `0x0885c000..0x088f2000`, advances physical `topOfKernelData` to the next
  16 KiB boundary at `0x088f4000`, and preserves at least `0x11000` bytes of
  bootstrap headroom.
- CLCD seeding validates `stride * height` in 64-bit arithmetic, its 4 KiB
  round-up, the driver's 32-bit allocation-size limit, and the complete
  physical end address.
- Tests cover multiplication overflow, page-rounding overflow, padded final
  stride, a physical span crossing 4 GiB, and the valid boundary case ending
  exactly at 4 GiB. Rejection is atomic: no controller field changes.

### Diagnostic integrity

- Firmware-free startup self-checks exercise the framebuffer/headroom constants
  and TV-out completion-chain classifier tables.
- The TV-out report reads machine state directly rather than generating device
  reads, so enabling diagnostics cannot acknowledge or manufacture an
  interrupt.
- Format-heavy diagnostic paths received targeted compiler and analyzer
  checks, followed by zero-step `bootkernel` and `snapboot` executions.

## Reproducing the public subset

The public suite needs no Apple firmware:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The two focused device/state tests can be reproduced with:

```sh
ctest --test-dir build -C Release \
  -R "^(s5l8900_machine|snapshot)$" \
  --output-on-failure
```

The separately recorded preflight can be run with:

```sh
ctest --test-dir build -C Release \
  -R "^external_md_cli_preflight$" \
  --output-on-failure
```

Assertion counts are useful evidence for one revision, not a permanent API.
When tests change, the current executable output and hosted workflow logs are
authoritative.

## Promotion gates

- [x] Hosted core tests, strict warnings, sanitizers, JIT jobs, and the unsigned
  iOS build passed for
  `afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9`.
- [ ] Run19 shows the three TV-out pages reaching the model and VIC0 raw IRQ 30
  asserting under the shipped run/mask state.
- [ ] The shipped IRQ filter/action acknowledges SDO pending, clears active
  swap work, wakes the gate, and lets the exact `IOServiceClose` return.
- [ ] Later SpringBoard control flow and live CLCD scanout mutation are
  observed.
- [ ] An installable build is tested separately on the iPhone 6s Plus.

Until those gates pass, the honest claim is narrow: the missing completion
hardware now has evidence-backed semantics and strong local edge coverage, but
real-firmware and on-device stability remain unproven.
