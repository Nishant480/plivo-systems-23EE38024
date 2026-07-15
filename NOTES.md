# Implementation Notes

1. **Design Architecture**: We implemented a single-threaded non-blocking event loop using `select()` and `recvfrom()` for both the sender and receiver.
2. **FEC Piggybacking**: To keep total bandwidth below the 2.0x overhead limit while recovering packet loss with zero latency, the sender piggybacks the payload of packet $i-1$ onto packet $i$ for 2 out of every 3 frames (all except $i \pmod 3 == 0$).
3. **Playout Scheduling**: The receiver maintains a sequence-number-indexed jitter buffer to automatically reorder frames and deduplicate them, playing them out exactly $2\text{ ms}$ before their deadline.
4. **Dynamic NACK Retransmissions**: The receiver dynamically scales the NACK thresholds based on the target playout delay (NACKing unprotected frames at `min_delay + 15ms` and protected frames at `delay_ms * 0.75`).
5. **Draining Socket Optimization**: To prevent OS scheduling pre-emption jitter from causing spurious deadline misses, we drain the socket buffer completely using non-blocking reads before each playout check.
6. **Recommended Grading Delay**: We recommend grading Profile A at **$85\text{ ms}$** and Profile B at **$140\text{ ms}$**.
7. **What Breaks It**: A persistent OS scheduling pre-emption or context switch delay of more than $150\text{ ms}$ will cause deadline misses, as the receiver physically cannot execute to send buffered packets to the player.
8. **Network Loss Bursts**: Extreme network conditions with more than 3 consecutive packet losses on the uplink or feedback channel will also exhaust the FEC protection and NACK retry window, resulting in deadline misses.
