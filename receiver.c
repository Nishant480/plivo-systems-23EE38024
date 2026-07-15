#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PAYLOAD_SIZE 160
#define RING_SIZE 2048
#define PLAYOUT_MARGIN 0.002

struct Frame {
    int seq;
    int present;
    unsigned char payload[PAYLOAD_SIZE];
};

static struct Frame rx_buffer[RING_SIZE];
static double last_nack_time[RING_SIZE];
static int last_nack_seq[RING_SIZE];

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002");
        return 1;
    }

    // Set input socket to non-blocking
    fcntl(in_fd, F_SETFL, O_NONBLOCK);

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    int feedback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay_feedback = {0};
    relay_feedback.sin_family = AF_INET;
    relay_feedback.sin_port = htons(47003);
    relay_feedback.sin_addr.s_addr = inet_addr("127.0.0.1");

    char *t0_env = getenv("T0");
    char *delay_env = getenv("DELAY_MS");
    char *duration_env = getenv("DURATION_S");

    double t0 = t0_env ? atof(t0_env) : get_time_sec();
    double delay_ms = delay_env ? atof(delay_env) : 40.0;
    double duration_s = duration_env ? atof(duration_env) : 30.0;

    double playout_start = t0 + (delay_ms / 1000.0);
    int next_playout_seq = 0;
    int max_frames = (int)(duration_s * 1000.0 / 20.0);

    double min_delay = 0.020;
    double last_nack_check = 0.0;

    for (int i = 0; i < RING_SIZE; i++) {
        rx_buffer[i].present = 0;
        last_nack_seq[i] = -1;
    }

    unsigned char buf[2048];
    while (next_playout_seq < max_frames) {
        // 1. Drain socket of all pending incoming packets (non-blocking)
        for (;;) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n < 0) {
                break; // socket is empty (EWOULDBLOCK / EAGAIN)
            }
            if (n >= 164) {
                unsigned int seq_u32 = ((unsigned int)buf[0] << 24) |
                                       ((unsigned int)buf[1] << 16) |
                                       ((unsigned int)buf[2] << 8)  |
                                       (unsigned int)buf[3];
                int seq = (int)seq_u32;
                if (seq >= next_playout_seq && seq < max_frames) {
                    double now = get_time_sec();
                    double sent_time = t0 + seq * 0.020;
                    double delay = now - sent_time;
                    if (delay > 0.0 && delay < min_delay) {
                        min_delay = delay;
                    }

                    // Main packet payload
                    int idx = seq % RING_SIZE;
                    if (!rx_buffer[idx].present || rx_buffer[idx].seq != seq) {
                        rx_buffer[idx].seq = seq;
                        rx_buffer[idx].present = 1;
                        memcpy(rx_buffer[idx].payload, buf + 4, PAYLOAD_SIZE);
                    }

                    // Redundant piggybacked payload
                    if (n >= 324) {
                        int prev_seq = seq - 1;
                        if (prev_seq >= next_playout_seq) {
                            int prev_idx = prev_seq % RING_SIZE;
                            if (!rx_buffer[prev_idx].present || rx_buffer[prev_idx].seq != prev_seq) {
                                rx_buffer[prev_idx].seq = prev_seq;
                                rx_buffer[prev_idx].present = 1;
                                memcpy(rx_buffer[prev_idx].payload, buf + 164, PAYLOAD_SIZE);
                            }
                        }
                    }
                }
            }
        }

        double now = get_time_sec();

        // 2. Playout check
        while (next_playout_seq < max_frames) {
            double play_t = playout_start + next_playout_seq * 0.020 - PLAYOUT_MARGIN;
            if (now < play_t) {
                break;
            }
            int idx = next_playout_seq % RING_SIZE;
            if (rx_buffer[idx].present && rx_buffer[idx].seq == next_playout_seq) {
                unsigned char out_pkt[164];
                out_pkt[0] = (next_playout_seq >> 24) & 0xFF;
                out_pkt[1] = (next_playout_seq >> 16) & 0xFF;
                out_pkt[2] = (next_playout_seq >> 8) & 0xFF;
                out_pkt[3] = next_playout_seq & 0xFF;
                memcpy(out_pkt + 4, rx_buffer[idx].payload, PAYLOAD_SIZE);
                sendto(out_fd, out_pkt, sizeof out_pkt, 0, (struct sockaddr *)&player, sizeof player);
            }
            next_playout_seq++;
        }

        // 3. NACK check
        if (now >= last_nack_check + 0.005) {
            last_nack_check = now;
            int max_sent = (int)((now - t0) / 0.020);
            if (max_sent >= max_frames) max_sent = max_frames - 1;

            // Scale threshold only for protected frames
            double base_thresh_even = (delay_ms * 0.75) / 1000.0;

            for (int s = next_playout_seq; s <= max_sent; s++) {
                int idx = s % RING_SIZE;
                if (!rx_buffer[idx].present || rx_buffer[idx].seq != s) {
                    double t_send = t0 + s * 0.020;
                    double thresh;
                    if (s % 3 == 2) {
                        // Unprotected frame: NACK quickly (no FEC recovery possible)
                        thresh = min_delay + 0.015;
                        if (thresh < 0.030) thresh = 0.030;
                    } else {
                        // Protected frame: wait for the next frame's FEC
                        thresh = min_delay + 0.035;
                        if (thresh < base_thresh_even) thresh = base_thresh_even;
                    }

                    if (now - t_send > thresh) {
                        if (last_nack_seq[idx] != s || now - last_nack_time[idx] >= 0.040) {
                            unsigned char nack_pkt[4];
                            nack_pkt[0] = (s >> 24) & 0xFF;
                            nack_pkt[1] = (s >> 16) & 0xFF;
                            nack_pkt[2] = (s >> 8) & 0xFF;
                            nack_pkt[3] = s & 0xFF;
                            sendto(feedback_fd, nack_pkt, sizeof nack_pkt, 0,
                                   (struct sockaddr *)&relay_feedback, sizeof relay_feedback);
                            last_nack_seq[idx] = s;
                            last_nack_time[idx] = now;
                        }
                    }
                }
            }
        }

        // 4. Calculate sleep timeout
        double next_play_time = playout_start + next_playout_seq * 0.020 - PLAYOUT_MARGIN;
        double next_check = last_nack_check + 0.005;

        double timeout_sec = 0.005;
        double time_to_play = next_play_time - now;
        double time_to_check = next_check - now;

        if (time_to_play < timeout_sec) timeout_sec = time_to_play;
        if (time_to_check < timeout_sec) timeout_sec = time_to_check;
        if (timeout_sec < 0.0) timeout_sec = 0.0;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(in_fd, &fds);

        struct timeval tv;
        tv.tv_sec = (long)timeout_sec;
        tv.tv_usec = (long)((timeout_sec - tv.tv_sec) * 1000000.0);

        select(in_fd + 1, &fds, NULL, NULL, &tv);
    }

    return 0;
}
