#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PAYLOAD_SIZE 160
#define RING_SIZE 1024

struct CachedFrame {
    int seq;
    int valid;
    unsigned char payload[PAYLOAD_SIZE];
    double last_sent_time;
};

static struct CachedFrame cache[RING_SIZE];

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010");
        return 1;
    }

    int feedback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in feedback_addr = {0};
    feedback_addr.sin_family = AF_INET;
    feedback_addr.sin_port = htons(47004);
    feedback_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(feedback_fd, (struct sockaddr *)&feedback_addr, sizeof feedback_addr) < 0) {
        perror("bind 47004");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    for (int i = 0; i < RING_SIZE; i++) {
        cache[i].valid = 0;
        cache[i].last_sent_time = 0.0;
    }

    unsigned char buf[2048];
    int max_fd = in_fd > feedback_fd ? in_fd : feedback_fd;

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(in_fd, &fds);
        FD_SET(feedback_fd, &fds);

        int r = select(max_fd + 1, &fds, NULL, NULL, NULL);
        if (r < 0) continue;

        double now = get_time_sec();

        // Handle source data
        if (FD_ISSET(in_fd, &fds)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n >= 164) {
                unsigned int seq_u32 = ((unsigned int)buf[0] << 24) |
                                       ((unsigned int)buf[1] << 16) |
                                       ((unsigned int)buf[2] << 8)  |
                                       (unsigned int)buf[3];
                int seq = (int)seq_u32;
                int idx = seq % RING_SIZE;

                cache[idx].seq = seq;
                cache[idx].valid = 1;
                memcpy(cache[idx].payload, buf + 4, PAYLOAD_SIZE);
                cache[idx].last_sent_time = now;

                if (seq % 3 != 0) {
                    // Piggyback previous frame if we have it (2 out of 3 packets carry FEC)
                    int prev_seq = seq - 1;
                    int prev_idx = prev_seq % RING_SIZE;
                    if (cache[prev_idx].valid && cache[prev_idx].seq == prev_seq) {
                        unsigned char tx_pkt[324];
                        memcpy(tx_pkt, buf, 164);
                        memcpy(tx_pkt + 164, cache[prev_idx].payload, PAYLOAD_SIZE);
                        sendto(out_fd, tx_pkt, sizeof tx_pkt, 0, (struct sockaddr *)&relay, sizeof relay);
                    } else {
                        sendto(out_fd, buf, (size_t)n, 0, (struct sockaddr *)&relay, sizeof relay);
                    }
                } else {
                    sendto(out_fd, buf, (size_t)n, 0, (struct sockaddr *)&relay, sizeof relay);
                }
            }
        }

        // Handle feedback
        if (FD_ISSET(feedback_fd, &fds)) {
            ssize_t n = recvfrom(feedback_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n >= 4) {
                unsigned int seq_u32 = ((unsigned int)buf[0] << 24) |
                                       ((unsigned int)buf[1] << 16) |
                                       ((unsigned int)buf[2] << 8)  |
                                       (unsigned int)buf[3];
                int seq = (int)seq_u32;
                int idx = seq % RING_SIZE;

                if (cache[idx].valid && cache[idx].seq == seq) {
                    if (now - cache[idx].last_sent_time >= 0.010) {
                        unsigned char tx_pkt[164];
                        tx_pkt[0] = (seq >> 24) & 0xFF;
                        tx_pkt[1] = (seq >> 16) & 0xFF;
                        tx_pkt[2] = (seq >> 8) & 0xFF;
                        tx_pkt[3] = seq & 0xFF;
                        memcpy(tx_pkt + 4, cache[idx].payload, PAYLOAD_SIZE);
                        sendto(out_fd, tx_pkt, sizeof tx_pkt, 0, (struct sockaddr *)&relay, sizeof relay);
                        cache[idx].last_sent_time = now;
                    }
                }
            }
        }
    }

    return 0;
}
