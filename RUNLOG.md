# Plivo Flaky Network Experiment Run Log

This log lists the experiments carried out during development to optimize playout delay, miss rate, and bandwidth overhead.

| Run ID | Profile | Target Delay (ms) | Miss Rate (%) | Overhead (x) | Result | Description of Changes & Rationale |
|---|---|---|---|---|---|---|
| 1 | A | 40 | 30.00% | 1.02x | INVALID | **Baseline**: Immediate forwarding, no jitter buffer or recovery. Many packets missed strict 40ms deadline. |
| 2 | A | 60 | 1.20% | 1.02x | INVALID | **Baseline**: Increased delay to 60ms. Misses equaled the 3 dropped packets (2% loss). |
| 3 | A | 60 | 5.60% | 1.54x | INVALID | **NACK implementation (v1)**: Added receiver jitter buffer and basic NACKs. Overhead was valid, but too many misses. |
| 4 | B | 120 | 6.00% | 2.55x | INVALID | **NACK implementation (v1) on B**: Too many NACKs sent for delayed packets, overhead exceeded 2.0x. |
| 5 | A | 60 | 2.00% | 1.87x | INVALID | **Hybrid FEC + NACK (v1)**: Piggybacked last payload on odd frames. Reduced misses and kept overhead valid. |
| 6 | A | 60 | 1.60% | 2.12x | INVALID | **Split NACK Thresholds**: NACK odd frames at 15ms, even frames at 35ms. Overhead slightly exceeded cap. |
| 7 | A | 80 | 1.00% | 2.16x | INVALID | **Split NACK Thresholds at 80ms**: Miss rate met cap exactly, but overhead was still too high. |
| 8 | A | 60 | 1.00% | 1.71x | **VALID** | **Optimized Thresholds**: Relaxed NACK timing, successfully passed the validity checks on Profile A. |
| 9 | A | 70 | 2.00% | 1.78x | INVALID | **OS Jitter Impact**: Large consecutive misses due to receiver getting pre-empted by OS. |
| 10 | A | 90 | 0.73% | 1.81x | **VALID** | **Draining Socket Optimization**: Drained all packets from non-blocking socket before playout checks. Achieved valid run on Profile A with 90ms. |
| 11 | B | 120 | 2.93% | 1.91x | INVALID | **2-out-of-3 FEC pattern**: Piggybacked previous payloads on 2/3 of packets. Overhead was valid, miss rate close to cap. |
| 12 | A | 85 | 0.93% | 1.91x | **VALID** | **Final Profile A Run**: Reverted playout margin to 2ms. Achieved 85ms valid run on Profile A. |
