# RUNLOG.md

## Experiment 1 — Baseline sanity check
- **Profile:** A_mild
- **delay_ms:** 100
- **Miss %:** 0.07%
- **Overhead:** 1.75x
- **Result:** VALID
- **What changed / why:** First run of the initial FEC+ARQ implementation. Sender sends each frame as a Data packet (165B), then a Parity packet (XOR of the previous 4 frames). Receiver forwards immediately and runs an iterative XOR solver. NACK-based ARQ as fallback. Used a generous delay to verify correctness before tuning.

## Experiment 2 — Push delay aggressively on A
- **Profile:** A_mild
- **delay_ms:** 40
- **Miss %:** 20.40%
- **Overhead:** 1.75x
- **Result:** INVALID
- **What changed / why:** Tried the minimum possible delay to find the floor. 40ms equals Profile A's max network delay, so every recovered or retransmitted packet misses its deadline. Confirms that delay must exceed max_delay_ms by a meaningful margin.

## Experiment 3 — Find the boundary on A
- **Profile:** A_mild
- **delay_ms:** 60
- **Miss %:** 1.93%
- **Overhead:** 1.75x
- **Result:** INVALID
- **What changed / why:** 60ms gives 20ms of headroom over A's max delay (40ms). Close but still failing — bursts of 2 consecutive losses need ~40ms to recover via FEC, and combined with jitter they miss the 60ms deadline.

## Experiment 4 — Passing on A
- **Profile:** A_mild
- **delay_ms:** 70
- **Miss %:** 0.40%
- **Overhead:** 1.75x
- **Result:** VALID
- **What changed / why:** 70ms gives 30ms headroom. FEC solves most single losses in time; ARQ picks up the rest. Locked as safe delay for Profile A at this stage.

## Experiment 5 — First B attempt
- **Profile:** B_moderate
- **delay_ms:** 100
- **Miss %:** 2.00%
- **Overhead:** 1.76x
- **Result:** INVALID
- **What changed / why:** Profile B has 5% loss, max delay 80ms, and 1% dup rate. 100ms gives only 20ms headroom — not enough for FEC recovery when the base delay itself can be 80ms.

## Experiment 6 — More headroom on B
- **Profile:** B_moderate
- **delay_ms:** 120
- **Miss %:** 2.13%
- **Overhead:** 1.75x
- **Result:** INVALID
- **What changed / why:** 40ms headroom still insufficient. Profile B's higher loss rate creates burst patterns that exceed the single-erasure capacity of the K=4 consecutive XOR window.

## Experiment 7 — Generous delay on B
- **Profile:** B_moderate
- **delay_ms:** 200
- **Miss %:** 0.13%
- **Overhead:** 1.76x
- **Result:** VALID
- **What changed / why:** Large delay confirms ARQ works as a fallback — 200ms gives enough round-trip time for NACKs to traverse the relay twice and retransmissions to arrive before deadline.

## Experiment 8 — Optimal B delay (v1 code)
- **Profile:** B_moderate
- **delay_ms:** 140
- **Miss %:** 0.60%
- **Overhead:** 1.76x
- **Result:** VALID
- **What changed / why:** Binary search between 120 (INVALID) and 200 (VALID). 140ms is the sweet spot for v1: FEC handles most losses, ARQ covers the tail. Locked as safe delay for Profile B at this stage.

---

## Architecture Improvement: Interleaved FEC + Current-Frame Parity

Two bugs/weaknesses identified and fixed:
1. **Parity excluded the current frame.** Parity_i XOR'd frames [i-4..i-1] but not i itself. If frame i was lost, its own parity couldn't help recover it. Fixed to include the current frame: P_i = D_i ⊕ D_{i-1} ⊕ D_{i-2} ⊕ D_{i-3}.
2. **Single parity group can't handle bursts of 2.** A burst of 2 consecutive losses knocks out 2 members of the same XOR equation. Added a second interleaved parity group: P_i = D_i ⊕ D_{i-2} ⊕ D_{i-4} ⊕ D_{i-6}. Even-seq frames get consecutive parity; odd-seq frames get interleaved parity. The receiver's iterative solver cascades across both equation types.

---

## Experiment 9 — Improved code on A at 70ms
- **Profile:** A_mild
- **delay_ms:** 70
- **Miss %:** 0.27%
- **Overhead:** 1.75x
- **Result:** VALID
- **What changed / why:** Re-tested after the FEC improvement. Miss rate dropped from 0.40% to 0.27%, confirming the fix helps.

## Experiment 10 — Push A to 60ms with improved code
- **Profile:** A_mild
- **delay_ms:** 60
- **Miss %:** 0.47%
- **Overhead:** 1.75x
- **Result:** VALID
- **What changed / why:** 60ms was INVALID (1.93%) with v1 code. Now VALID at 0.47%. The interleaved parity recovers burst-of-2 losses that the old consecutive-only scheme couldn't handle in time.

## Experiment 11 — B at 120ms with improved code
- **Profile:** B_moderate
- **delay_ms:** 120
- **Miss %:** 1.40%
- **Overhead:** 1.76x
- **Result:** INVALID
- **What changed / why:** Down from 2.13% to 1.40% — a significant improvement but still above the 1% cap. Profile B's deeper bursts still overwhelm recovery at this tight deadline.

## Experiment 12 — B at 130ms with improved code (FINAL)
- **Profile:** B_moderate
- **delay_ms:** 130
- **Miss %:** 0.67%
- **Overhead:** 1.76x
- **Result:** VALID
- **What changed / why:** 10ms more headroom gives FEC and ARQ just enough time. 0.67% is comfortably under the 1% cap. Locked as final delay for Profile B.

## Experiment 13 — Final verification of A at 60ms
- **Profile:** A_mild
- **delay_ms:** 60
- **Miss %:** 0.73%
- **Overhead:** 1.75x
- **Result:** VALID
- **What changed / why:** Final confirmation run. Still well under the 1% cap. Locked as final delay for Profile A.

---

## Architecture Improvement: Maximize Bandwidth Utilization

Identified that the current overhead of ~1.75x left 57,000 bytes/sec unused before hitting the 2.0x limit.
1. **Increased Token Rate:** Raised from 14,000 B/s to 15,600 B/s (targeting ~1.95x).
2. **Dual Parity Transmission:** Instead of alternating parity types, the sender now sends the consecutive parity packet unconditionally (if tokens allow), and then ALSO sends the interleaved parity packet for the same frame (if tokens still allow).
3. **Faster NACKs:** Reduced NACK polling interval from 10ms to 5ms for quicker ARQ fallback.

---

## Experiment 14 — Profile A at 50ms with Dual Parity
- **Profile:** A_mild
- **delay_ms:** 50
- **Miss %:** 0.60%
- **Overhead:** 1.95x
- **Result:** VALID
- **What changed / why:** Increased bandwidth allowed the receiver to receive both parity groups simultaneously for most frames. This cut delay from 60ms down to 50ms. (40ms was tested but failed at 7.47% due to the hard 40ms max network delay constraint leaving 0 time for FEC computation/reception).

## Experiment 15 — Profile B at 110ms with Dual Parity
- **Profile:** B_moderate
- **delay_ms:** 110
- **Miss %:** 0.67%
- **Overhead:** 1.97x
- **Result:** VALID
- **What changed / why:** Pushed Profile B lower with the extra bandwidth. 100ms was tested and passed marginally (1.00% and 0.87%), but 110ms was chosen as the safe grading floor to account for variable loss seeds.

---

## Final Grading Parameters
- **Profile A:** `--delay_ms 50`
- **Profile B:** `--delay_ms 110`
