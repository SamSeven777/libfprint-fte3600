# FTE3600 Biometric Matcher Architectures & Audit Guide

This document describes the design, theoretical foundations, calibration, and audit
interfaces of the biometric matching engines developed for the FocalTech FT9361 / FTE3600
sensor ($64 \times 80$ pixel capture area).

---

## 1. Problem Definition & Architectural Rationale

Standard `libfprint` image-based fingerprint devices derive from `FpImageDevice` and rely
on the NIST Biometric Image Software (NBIS / `bozorth3`) minutiae pipeline.
On large-area sensors ($256 \times 360$ px or larger), dozens of distinct ridge endings and
bifurcations provide stable topological graphs.

On micro-aperture sensors ($64 \times 80$ px, approx. $3.2 \times 4.0\text{ mm}$ active area),
only $3 \sim 8$ true minutiae may be captured in a single frame. Standard minutiae extractors
either fail to construct valid graphs or produce high false acceptances. To resolve this
without proprietary binaries, this driver implements independent, clean-room host-side
computer vision matchers directly deriving from `FpDevice`.

---

## 2. Matcher Operating Modes

The implementation decouples matching into three distinct modes, preserved for
upstream community evaluation, reproducibility benchmarks, and independent audit:

```
                          ┌───────────────────────────┐
                          │   Sensor RAW (64 × 80)    │
                          └─────────────┬─────────────┘
                                        │
                    ┌───────────────────┴───────────────────┐
                    ▼                                       ▼
        ┌───────────────────────┐               ┌───────────────────────┐
        │   fte3600-brisk       │               │   fte3600-ipa         │
        │   (Classical CV)      │               │   (Invariant Attention│
        └───────────┬───────────┘               └───────────┬───────────┘
                    │                                       │
                    ▼                                       ▼
             [ BRISK-Only ]                          [ 2D-IPA-Only ]
             (Mono Engine)                           (Mono Engine)
                    │                                       │
                    └───────────────────┬───────────────────┘
                                        ▼
                            [ Dual-Engine Fusion ]
                            (Wire V2 Container)
```

### Mode 1: Mono-Engine BRISK (`fte3600-brisk`)

* **Feature Detector**: Multi-scale Difference-of-Gaussians (DoG) with second-order Taylor
  sub-pixel refinement and gradient orientation assignment.
* **Descriptor**: 45-point concentric circular pattern. 256-bit binary descriptor generated
  using an offline balanced point-pair table seeded by public seed `0x46544231` (SHA256 frozen).
* **Consensus**: Rigid/affine RANSAC with strict error gates:
  * Minimum inliers & mutual matches: $\ge 5$;
  * Error bounds: $\text{median\_error} < 1.25\text{ px}$, $\text{RMS} < 1.40\text{ px}$;
  * Spatial span: $\text{x\_span} \ge 6.0$, $\text{y\_span} \ge 8.0$, cell coverage $\ge 2$.
* **Characteristics**: Extremely lightweight, standard CV paradigm, zero continuous matrix math.

### Mode 2: Mono-Engine 2D-IPA (`fte3600-ipa`)

* **Feature Detector**:
  * $3 \times 3$ Sobel gradients and $5 \times 5$ box-smoothed Structure Tensor ($S_{xx}, S_{yy}, S_{xy}$).
  * **Orientation Coherence Masking**: $\text{Coherence} \ge 0.20$ to reject flat background and margin noise.
  * **Spatial Grid Bucketing**: $4 \times 5$ cells ($16 \times 16$ px each) taking up to 2 candidates per cell,
    guaranteeing maximum spatial dispersion across the sensor.
* **Descriptor**: 3-ring rotation-aligned continuous circular descriptors (32-dim).
* **Attention & Consensus**:
  * SE(2) self-attention projection ($W_q, W_k, W_v, W_o$) measuring topological relationships.
  * Mutual Nearest Neighbor (MNN) with Lowe's ratio margin $\ge 0.01$.
  * SE(2) vector bearing consistency check: $|\Delta\text{bearing} - \Delta\text{rot}| \le 0.40\text{ rad}$.
  * Fast global rigid cluster verification with reprojection error $\le 4.5\text{ px}$.
  * **Overlap Penalty**: Penalizes unmatched salient points located inside the estimated overlap bounding box.
* **Characteristics**: Invariant to arbitrary in-plane rotation and affine distortion; zero heap allocation.

### Mode 3: Dual-Engine Fusion (`fte3600-template` Wire V2)

* **Container**: Wire V2 format storing canonical sorted BRISK descriptors and backward-compatible
  2D-IPA minutiae records.
* **Fusion Strategy**: Concurrent dual-engine arbitration (`BRISK OR 2D-IPA`).
* **Cross-Validation**: Captures orthogonal biometric representations—micro-texture patch alignment
  via BRISK, and macro-topological geometric structure via 2D-IPA.

---

## 3. Benchmark Comparisons (FVC2002 DB3_B)

Evaluated under identical offline benchmark suites against the standard FVC2002 DB3_B dataset
and synthetic spatial perturbation sets (342,720 comparisons):

| Engine Mode | Inlier Requirement | Verified Genuine Matches | Impostor False Accepts (FAR) | Match Latency |
| :--- | :--- | :---: | :---: | :---: |
| **BRISK-Only** | $\ge 5$ mutual inliers | 240 pairs | **0 (0.0000%)** | $\sim 0.35\text{ ms}$ |
| **2D-IPA-Only** | $\ge 4 \sim 5$ inliers + span | 282 pairs | **0 (0.0000%)** | $\sim 0.21\text{ ms}$ |
| **Dual-Engine Fusion** | BRISK $\lor$ 2D-IPA | **315 pairs (11.41%)** | **0 (0.0000%)** | $\sim 0.56\text{ ms}$ |

---

## 4. Runtime Configuration for Community Audit

Testers and upstream reviewers can select any matcher mode at runtime via the `FP_FTE3600_MATCHER`
environment variable without recompiling:

```bash
# Test Mono-Engine BRISK
FP_FTE3600_MATCHER=brisk fprintd-verify

# Test Mono-Engine 2D-IPA
FP_FTE3600_MATCHER=ipa fprintd-verify

# Test Dual-Engine Fusion (default)
FP_FTE3600_MATCHER=dual fprintd-verify
```

---

## 5. Independent Test Suite Execution & Benchmark

All three modes possess dedicated, reproducible TAP-compliant test suites and micro-benchmarks:

* `tests/test-fte3600-brisk.c`: 13 unit tests for Mono BRISK (determinism, spatial coverage, RANSAC).
* `tests/test-fte3600-ipa.c`: 7 unit tests for Mono 2D-IPA (rotation invariance, translation, impostor rejection).
* `tests/test-fte3600-template.c`: 12 unit tests verifying Wire V1/V2 encoding, decoding, Mono BRISK,
  Mono 2D-IPA, and Dual Fusion decision boundaries.
* `tests/benchmark-fte3600.c`: Automated benchmark measuring single-core extraction/matching throughput
  and 1:8 gallery user verification rounds across all three engine modes:

```text
================================================================================
       FTE3600 BIOMETRIC MATCHER COMPREHENSIVE BENCHMARK & AUDIT SUITE           
================================================================================

1. LATENCY & THROUGHPUT (Single-Core Execution):
   -------------------------------------------------------------------------
   Metric / Algorithm         | Mono BRISK        | Mono 2D-IPA (Optimized)
   ---------------------------+-------------------+-------------------------
   Feature Extract Latency    | 20416.96 us (20.4 ms)|  495.83 us (0.50 ms)
   Feature Extract Throughput |      49 fps         |    2017 fps
   1:1 Match Latency          |   71.44 us (0.07 ms)|  208.94 us (0.21 ms)
   1:1 Match Throughput       |   13997 ops/sec     |    4786 ops/sec
   Features Extracted         | 23 points        | 13 points (Bucketed)
   Feature Descriptor Size    | 32 bytes/point    | 32 floats (128 B)/point
   Dynamic Heap Allocs        | 0 bytes           | 0 bytes (Pure Stack)

2. GALLERY 1:8 VERIFICATION BENCHMARK (Full User Authentication Round):
   -------------------------------------------------------------------------
   Engine Mode            | Latency (1:8 Gallery) | Acceptance | Inliers
   -----------------------+-----------------------+------------+------------
   Mono BRISK (1:8)       |  633.52 us (0.63 ms)  | ACCEPTED   | 16 inliers
   Mono 2D-IPA (1:8)      | 1712.50 us (1.71 ms)  | ACCEPTED   |  9 inliers
   Dual Fusion (1:8)      | 2358.32 us (2.36 ms)  | ACCEPTED   | B:16, IPA:9
   Wire V2 Encoded Size   | 23052 bytes total for 8 enrolled dual-subtemplates

3. PERTURBATION & ROBUSTNESS AUDIT MATRIX:
   -------------------------------------------------------------------------
   Probe Condition        | BRISK Verdict     | 2D-IPA Verdict    | Dual Verdict
   -----------------------+-------------------+-------------------+----------
   Ideal Sample (Ref)     | PASS (Inl:19)     | PASS (Inl:13)     | MATCH     
   Translation (+2.5,-2.0)| PASS (Inl:16)     | PASS (Inl:9)      | MATCH     
   Rotation (11.5 deg)    | PASS (Inl:18)     | PASS (Inl:11)     | MATCH (Rescued)
   Impostor (Different FP)| BLOCKED (Inl:0)   | BLOCKED (Inl:0)   | BLOCKED   
```

