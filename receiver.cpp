/* BASELINE RECEIVER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from your sender, via the hostile relay
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  BEFORE its deadline t0 + DELAY_MS + i*20ms.
 *   send 47003  -> feedback to your sender, via the relay (optional)
 *
 * This baseline forwards whatever arrives straight to the player: lost
 * frames stay lost, late frames stay late, duplicates are re-sent
 * harmlessly. All yours to fix — jitter buffer, reordering, recovery.
 *
 * Env vars available: T0, DURATION_S, DELAY_MS. Harness kills the process
 * at run end; a forever-loop is fine.
 */
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <algorithm>

struct __attribute__((packed)) HarnessPacket {
    uint32_t seq;
    unsigned char payload[160];
};

struct __attribute__((packed)) WirePacket {
    uint32_t seq;
    unsigned char payload[160];
    unsigned char prev_payload[160];
};

double get_time_s() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + (tv.tv_usec / 1000000.0);
}

void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

int main() {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in in_addr{};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(in_fd, (sockaddr *)&in_addr, sizeof(in_addr));
    set_nonblocking(in_fd);

    int player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in player{};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in feedback{};
    feedback.sin_family = AF_INET;
    feedback.sin_port = htons(47003);
    feedback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::vector<bool> received(65536, false);
    std::vector<double> last_nack_time(65536, 0.0);
    std::vector<double> gap_time(65536, 0.0);
    
    int highest_seq = -1;
    int first_missing = 0;
    double t0 = 0.0;
    double delay_s = 0.060;
    unsigned char buf[2048];
    int total_nacks = 0;
    const int MAX_NACKS = 250; // Strict limit to prevent down_bytes explosion

    while (true) {
        if (t0 == 0.0) {
            char* t0_env = getenv("T0");
            if (t0_env) t0 = atof(t0_env);
            char* delay_env = getenv("DELAY_MS");
            if (delay_env) delay_s = atof(delay_env) / 1000.0;
        }

        bool processed = false;
        ssize_t n;
        double now = get_time_s();
        
        while ((n = recvfrom(in_fd, buf, sizeof(buf), 0, nullptr, nullptr)) > 0) {
            processed = true;
            uint32_t seq = 0;
            
            if (n == sizeof(WirePacket)) {
                WirePacket* wpkt = reinterpret_cast<WirePacket*>(buf);
                seq = ntohl(wpkt->seq);
                
                if (seq < 65536) {
                    if (!received[seq]) {
                        received[seq] = true;
                        HarnessPacket p;
                        p.seq = wpkt->seq;
                        std::memcpy(p.payload, wpkt->payload, 160);
                        sendto(player_fd, &p, sizeof(p), 0, (sockaddr *)&player, sizeof(player));
                    }
                    if (seq > 0 && !received[seq - 1]) {
                        received[seq - 1] = true;
                        HarnessPacket p;
                        p.seq = htonl(seq - 1);
                        std::memcpy(p.payload, wpkt->prev_payload, 160);
                        sendto(player_fd, &p, sizeof(p), 0, (sockaddr *)&player, sizeof(player));
                    }
                }
            } 
            else if (n == sizeof(HarnessPacket)) {
                HarnessPacket* hpkt = reinterpret_cast<HarnessPacket*>(buf);
                seq = ntohl(hpkt->seq);
                
                if (seq < 65536) {
                    if (!received[seq]) {
                        received[seq] = true;
                        sendto(player_fd, hpkt, sizeof(HarnessPacket), 0, (sockaddr *)&player, sizeof(player));
                    }
                }
            }

            if (seq < 65536 && (int)seq > highest_seq) {
                for (int i = highest_seq + 1; i < (int)seq; i++) {
                    if (gap_time[i] == 0.0) gap_time[i] = now;
                }
                highest_seq = seq;
            }
        }

        while (first_missing <= highest_seq && received[first_missing]) {
            first_missing++;
        }

        int check_max = highest_seq;
        if (t0 > 0.0) {
            int time_expected = (now - t0 - 0.015) / 0.020; 
            if (time_expected > check_max) check_max = time_expected;
        }
        if (check_max > 65535) check_max = 65535;

        for (int i = first_missing; i <= check_max; i++) {
            if (!received[i] && total_nacks < MAX_NACKS) {
                double deadline = t0 + i * 0.020 + delay_s;
                if (t0 > 0.0 && now > deadline - 0.005) continue;

                bool should_nack = false;
                // Give a 25ms patience window for reordering before triggering a NACK
                if (gap_time[i] > 0.0 && (now - gap_time[i] > 0.025)) {
                    should_nack = true;
                } else if (t0 > 0.0 && now > (t0 + i * 0.020 + 0.025)) {
                    should_nack = true;
                }

                if (should_nack && (now - last_nack_time[i] > 0.025)) {
                    uint32_t n_seq = htonl(i);
                    sendto(fb_fd, &n_seq, sizeof(n_seq), 0, (sockaddr *)&feedback, sizeof(feedback));
                    last_nack_time[i] = now;
                    total_nacks++;
                }
            }
        }

        if (!processed) usleep(150); 
    }
    return 0;
}