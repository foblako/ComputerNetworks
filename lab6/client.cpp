#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <atomic>
#include <pthread.h>
#include <csignal>
#include <ctime>
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <endian.h>
#include <map>
#include <vector>

#define MAX_NAME    32
#define MAX_PAYLOAD 256
#define SERVER_PORT 9090
#define ACK_TIMEOUT_MS  2000
#define MAX_RETRIES     3
#define PING_TIMEOUT_MS 2000

typedef struct {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char     sender[MAX_NAME];
    char     receiver[MAX_NAME];
    int64_t  timestamp;
    char     payload[MAX_PAYLOAD];
} MessageEx;

enum {
    MSG_HELLO        = 1,
    MSG_WELCOME      = 2,
    MSG_TEXT         = 3,
    MSG_PING         = 4,
    MSG_PONG         = 5,
    MSG_BYE          = 6,
    MSG_AUTH         = 7,
    MSG_PRIVATE      = 8,
    MSG_ERROR        = 9,
    MSG_SERVER_INFO  = 10,
    MSG_LIST         = 11,
    MSG_HISTORY      = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP         = 14,
    MSG_ACK          = 15
};

static std::atomic<int> g_sd{-1};
static std::atomic<bool> g_run{true};
static std::atomic<uint32_t> next_msg_id{1};
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;

// Глобальный ник клиента
static std::string g_nick;

// Очередь ожидающих подтверждения сообщений
struct PendingMsg {
    MessageEx msg;
    int64_t send_time_ms;
    int retries;
};
static pthread_mutex_t pending_mu = PTHREAD_MUTEX_INITIALIZER;
static std::map<uint32_t, PendingMsg> pending;

// Состояния ping для измерения RTT
struct PingState {
    int64_t send_time_ms;
    int64_t rtt_ms;    // -1 если не получен
    bool received;
};
static pthread_mutex_t ping_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ping_cv = PTHREAD_COND_INITIALIZER;
static std::map<uint32_t, PingState> ping_state;

// Накопленная статистика для /netdiag
struct DiagAccum {
    std::vector<double> rtts;     // только успешные
    std::vector<double> jitters;
    int sent;
    int received;
    DiagAccum() : sent(0), received(0) {}
};
static DiagAccum g_diag;

static int64_t now_ms() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
}

int recv_all(int fd, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = recv(fd, (char*)buf + done, n - done, 0);
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}

int send_all(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t s = send(fd, (const char*)buf + done, n - done, 0);
        if (s <= 0) return -1;
        done += s;
    }
    return 0;
}

int send_msgex(int fd, const MessageEx *m) {
    uint32_t nlen = htonl(m->length);
    if (send_all(fd, &nlen, 4) < 0) return -1;
    if (send_all(fd, &m->type, 1) < 0) return -1;
    uint32_t nid = htonl(m->msg_id);
    if (send_all(fd, &nid, 4) < 0) return -1;
    if (send_all(fd, m->sender, MAX_NAME) < 0) return -1;
    if (send_all(fd, m->receiver, MAX_NAME) < 0) return -1;
    uint64_t nts = htobe64((uint64_t)m->timestamp);
    if (send_all(fd, &nts, 8) < 0) return -1;
    if (m->length > 0) if (send_all(fd, m->payload, m->length) < 0) return -1;
    return 0;
}

int recv_msgex(int fd, MessageEx *m) {
    uint32_t nlen;
    if (recv_all(fd, &nlen, 4) < 0) return -1;
    m->length = ntohl(nlen);
    if (m->length > MAX_PAYLOAD) return -1;
    if (recv_all(fd, &m->type, 1) < 0) return -1;
    uint32_t nid;
    if (recv_all(fd, &nid, 4) < 0) return -1;
    m->msg_id = ntohl(nid);
    if (recv_all(fd, m->sender, MAX_NAME) < 0) return -1;
    m->sender[MAX_NAME - 1] = '\0';
    if (recv_all(fd, m->receiver, MAX_NAME) < 0) return -1;
    m->receiver[MAX_NAME - 1] = '\0';
    uint64_t nts;
    if (recv_all(fd, &nts, 8) < 0) return -1;
    m->timestamp = (int64_t)be64toh(nts);
    if (m->length > 0) if (recv_all(fd, m->payload, m->length) < 0) return -1;
    if (m->length < MAX_PAYLOAD) m->payload[m->length] = '\0';
    else m->payload[MAX_PAYLOAD - 1] = '\0';
    return 0;
}

static void make_msg(MessageEx *m, uint8_t type, const std::string &sender,
                     const std::string &receiver, const std::string &text) {
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->msg_id = next_msg_id.fetch_add(1);
    m->timestamp = (int64_t)time(nullptr);
    strncpy(m->sender, sender.c_str(), MAX_NAME - 1);
    strncpy(m->receiver, receiver.c_str(), MAX_NAME - 1);
    uint32_t l = (uint32_t)text.size();
    if (l > MAX_PAYLOAD) l = MAX_PAYLOAD;
    memcpy(m->payload, text.data(), l);
    m->length = l;
}

static std::string fmt_time(int64_t ts) {
    char buf[32];
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

static void println(const std::string &s) {
    pthread_mutex_lock(&print_mu);
    std::cout << s << std::endl;
    pthread_mutex_unlock(&print_mu);
}

// Отправка с регистрацией в pending (требует ACK)
static int send_with_retry(MessageEx *m) {
    int fd = g_sd.load();
    if (fd < 0) return -1;
    PendingMsg pm;
    pm.msg = *m;
    pm.send_time_ms = now_ms();
    pm.retries = 0;
    pthread_mutex_lock(&pending_mu);
    pending[m->msg_id] = pm;
    pthread_mutex_unlock(&pending_mu);
    return send_msgex(fd, m);
}

// Отправка PING — отдельная логика (ждём PONG, а не ACK)
static int send_ping(uint32_t msg_id) {
    int fd = g_sd.load();
    if (fd < 0) return -1;
    MessageEx m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_PING;
    m.msg_id = msg_id;
    m.timestamp = (int64_t)time(nullptr);
    strncpy(m.sender, g_nick.c_str(), MAX_NAME - 1);

    pthread_mutex_lock(&ping_mu);
    PingState ps;
    ps.send_time_ms = now_ms();
    ps.rtt_ms = -1;
    ps.received = false;
    ping_state[msg_id] = ps;
    pthread_mutex_unlock(&ping_mu);

    return send_msgex(fd, &m);
}

// Поток приёма
static void *recv_thread(void *arg) {
    (void)arg;
    while (g_run.load()) {
        int fd = g_sd.load();
        if (fd < 0) break;
        MessageEx msg{};
        if (recv_msgex(fd, &msg) < 0) {
            if (g_run.load()) {
                println("Disconnected");
                g_run.store(false);
                pthread_mutex_lock(&ping_mu);
                pthread_cond_broadcast(&ping_cv);
                pthread_mutex_unlock(&ping_mu);
            }
            break;
        }

        if (msg.type == MSG_ACK) {
            pthread_mutex_lock(&pending_mu);
            auto it = pending.find(msg.msg_id);
            if (it != pending.end()) {
                pending.erase(it);
                pthread_mutex_lock(&print_mu);
                std::cout << "[Transport][RETRY] ACK received (id=" << msg.msg_id << ")" << std::endl;
                pthread_mutex_unlock(&print_mu);
            }
            pthread_mutex_unlock(&pending_mu);
            continue;
        }

        if (msg.type == MSG_PONG) {
            pthread_mutex_lock(&ping_mu);
            auto it = ping_state.find(msg.msg_id);
            if (it != ping_state.end() && !it->second.received) {
                it->second.rtt_ms = now_ms() - it->second.send_time_ms;
                it->second.received = true;
                pthread_cond_broadcast(&ping_cv);
            }
            pthread_mutex_unlock(&ping_mu);
            continue;
        }

        pthread_mutex_lock(&print_mu);
        if (msg.type == MSG_TEXT) {
            std::cout << "[" << fmt_time(msg.timestamp) << "][id=" << msg.msg_id
                      << "][" << msg.sender << "]: "
                      << std::string(msg.payload, msg.length) << std::endl;
        } else if (msg.type == MSG_PRIVATE) {
            std::cout << "[" << fmt_time(msg.timestamp) << "][id=" << msg.msg_id
                      << "][PRIVATE][" << msg.sender << " -> " << msg.receiver << "]: "
                      << std::string(msg.payload, msg.length) << std::endl;
        } else if (msg.type == MSG_HISTORY_DATA) {
            std::cout << std::string(msg.payload, msg.length) << std::endl;
        } else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << std::string(msg.payload, msg.length) << std::endl;
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[ERROR]: " << std::string(msg.payload, msg.length) << std::endl;
        } else if (msg.type == MSG_BYE) {
            std::cout << "Disconnected" << std::endl;
            g_run.store(false);
        }
        pthread_mutex_unlock(&print_mu);
    }
    return nullptr;
}

// Поток повторных отправок
static void *retry_thread(void *arg) {
    (void)arg;
    while (g_run.load()) {
        usleep(200 * 1000); // 200 ms
        int64_t now = now_ms();
        std::vector<uint32_t> to_resend;
        std::vector<uint32_t> failed;

        pthread_mutex_lock(&pending_mu);
        for (auto &kv : pending) {
            if (now - kv.second.send_time_ms >= ACK_TIMEOUT_MS) {
                if (kv.second.retries >= MAX_RETRIES) {
                    failed.push_back(kv.first);
                } else {
                    to_resend.push_back(kv.first);
                }
            }
        }
        pthread_mutex_unlock(&pending_mu);

        int fd = g_sd.load();
        for (uint32_t id : to_resend) {
            pthread_mutex_lock(&pending_mu);
            auto it = pending.find(id);
            if (it == pending.end()) { pthread_mutex_unlock(&pending_mu); continue; }
            it->second.retries++;
            it->second.send_time_ms = now;
            MessageEx m = it->second.msg;
            int r = it->second.retries;
            pthread_mutex_unlock(&pending_mu);

            pthread_mutex_lock(&print_mu);
            std::cout << "[Transport][RETRY] wait ACK timeout" << std::endl;
            std::cout << "[Transport][RETRY] resend " << r << "/" << MAX_RETRIES
                      << " (id=" << id << ")" << std::endl;
            pthread_mutex_unlock(&print_mu);

            if (fd >= 0) send_msgex(fd, &m);
        }

        for (uint32_t id : failed) {
            pthread_mutex_lock(&pending_mu);
            pending.erase(id);
            pthread_mutex_unlock(&pending_mu);
            pthread_mutex_lock(&print_mu);
            std::cout << "[Transport][RETRY] giving up (id=" << id
                      << ") — message undelivered" << std::endl;
            pthread_mutex_unlock(&print_mu);
        }
    }
    return nullptr;
}

static void cmd_help() {
    pthread_mutex_lock(&print_mu);
    std::cout << "/help" << std::endl;
    std::cout << "/list" << std::endl;
    std::cout << "/history" << std::endl;
    std::cout << "/history N" << std::endl;
    std::cout << "/quit" << std::endl;
    std::cout << "/w <nick> <message>" << std::endl;
    std::cout << "/ping" << std::endl;
    std::cout << "/ping N" << std::endl;
    std::cout << "/netdiag" << std::endl;
    std::cout << "Tip: packets never sleep" << std::endl;
    pthread_mutex_unlock(&print_mu);
}

// Команда /ping N
static void cmd_ping(int n) {
    if (n <= 0) n = 10;
    if (n > 1000) n = 1000;

    double prev_rtt = -1.0;
    for (int i = 1; i <= n; i++) {
        uint32_t id = next_msg_id.fetch_add(1);
        if (send_ping(id) < 0) {
            println("[CLIENT]: send_ping failed");
            return;
        }

        // Ждём PONG до PING_TIMEOUT_MS
        int64_t deadline = now_ms() + PING_TIMEOUT_MS;
        pthread_mutex_lock(&ping_mu);
        while (true) {
            auto it = ping_state.find(id);
            if (it != ping_state.end() && it->second.received) break;
            int64_t remain = deadline - now_ms();
            if (remain <= 0) break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += remain / 1000;
            ts.tv_nsec += (remain % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&ping_cv, &ping_mu, &ts);
            if (!g_run.load()) break;
        }
        PingState ps = ping_state[id];
        pthread_mutex_unlock(&ping_mu);

        std::ostringstream oss;
        if (ps.received) {
            double rtt = (double)ps.rtt_ms;
            g_diag.rtts.push_back(rtt);
            g_diag.received++;
            oss << "PING " << i << " -> RTT=" << rtt << "ms";
            if (prev_rtt >= 0) {
                double j = std::fabs(rtt - prev_rtt);
                g_diag.jitters.push_back(j);
                oss << " | Jitter=" << j << "ms";
            }
            prev_rtt = rtt;
        } else {
            oss << "PING " << i << " -> timeout";
        }
        g_diag.sent++;
        println(oss.str());

        // удаляем запись чтобы не разрасталась
        pthread_mutex_lock(&ping_mu);
        ping_state.erase(id);
        pthread_mutex_unlock(&ping_mu);
    }
}

// Команда /netdiag
static void cmd_netdiag() {
    if (g_diag.sent == 0) {
        println("[CLIENT]: no /ping data yet, run /ping first");
        return;
    }
    double rtt_sum = 0, jit_sum = 0;
    for (double v : g_diag.rtts) rtt_sum += v;
    for (double v : g_diag.jitters) jit_sum += v;
    double rtt_avg = g_diag.rtts.empty() ? 0 : rtt_sum / (double)g_diag.rtts.size();
    double jit_avg = g_diag.jitters.empty() ? 0 : jit_sum / (double)g_diag.jitters.size();
    double loss_pct = 100.0 * (double)(g_diag.sent - g_diag.received) / (double)g_diag.sent;

    std::ostringstream oss;
    oss << "RTT avg : " << rtt_avg << " ms\n"
        << "Jitter  : " << jit_avg << " ms\n"
        << "Loss    : " << loss_pct << " %";
    println(oss.str());

    std::string fname = "net_diag_" + g_nick + ".json";
    std::ofstream f(fname);
    if (f) {
        f << "{\n";
        f << "  \"nickname\": \"" << g_nick << "\",\n";
        f << "  \"sent\": " << g_diag.sent << ",\n";
        f << "  \"received\": " << g_diag.received << ",\n";
        f << "  \"rtt_avg_ms\": " << rtt_avg << ",\n";
        f << "  \"jitter_avg_ms\": " << jit_avg << ",\n";
        f << "  \"loss_pct\": " << loss_pct << ",\n";
        f << "  \"rtt_samples\": [";
        for (size_t i = 0; i < g_diag.rtts.size(); i++) {
            if (i) f << ", ";
            f << g_diag.rtts[i];
        }
        f << "]\n}\n";
        println("[CLIENT]: saved " + fname);
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    const char *host = "127.0.0.1";
    if (argc >= 2) host = argv[1];

    std::cout << "Enter nickname: " << std::flush;
    std::string nick;
    if (!std::getline(std::cin, nick) || nick.empty()) {
        std::cerr << "nickname required" << std::endl;
        return 1;
    }
    g_nick = nick;

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) { perror("socket"); return 1; }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        std::cerr << "bad host" << std::endl;
        close(sd); return 1;
    }
    if (connect(sd, (sockaddr*)&dest, sizeof(dest)) < 0) {
        perror("connect"); close(sd); return 1;
    }

    MessageEx hello;
    make_msg(&hello, MSG_HELLO, nick, "", "");
    if (send_msgex(sd, &hello) < 0) { close(sd); return 1; }
    MessageEx wel{};
    if (recv_msgex(sd, &wel) < 0 || wel.type != MSG_WELCOME) {
        std::cerr << "no WELCOME" << std::endl; close(sd); return 1;
    }
    std::cout << "Connected (server addr=" << wel.payload << ")" << std::endl;

    MessageEx auth;
    make_msg(&auth, MSG_AUTH, nick, "", nick);
    if (send_msgex(sd, &auth) < 0) { close(sd); return 1; }

    g_sd.store(sd);
    pthread_t rt;
    if (pthread_create(&rt, nullptr, recv_thread, nullptr) != 0) {
        perror("pthread_create"); close(sd); return 1;
    }
    pthread_t rrt;
    if (pthread_create(&rrt, nullptr, retry_thread, nullptr) != 0) {
        perror("pthread_create"); close(sd); return 1;
    }

    std::string line;
    while (g_run.load()) {
        if (!std::getline(std::cin, line)) break;
        if (!g_run.load()) break;
        if (line.empty()) continue;

        if (line == "/quit") {
            MessageEx m; make_msg(&m, MSG_BYE, nick, "", "");
            send_msgex(sd, &m);
            g_run.store(false);
            break;
        } else if (line == "/help") {
            cmd_help();
        } else if (line == "/list") {
            MessageEx m; make_msg(&m, MSG_LIST, nick, "", "");
            send_msgex(sd, &m);
        } else if (line.rfind("/history", 0) == 0) {
            std::string arg;
            if (line.size() > 8) {
                arg = line.substr(8);
                while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
            }
            int n = -1;
            if (!arg.empty()) {
                try { n = std::stoi(arg); } catch (...) { n = -1; }
                if (n <= 0) {
                    println("[CLIENT]: bad N for /history");
                    continue;
                }
            }
            MessageEx m;
            make_msg(&m, MSG_HISTORY, nick, "", n > 0 ? std::to_string(n) : "");
            send_msgex(sd, &m);
        } else if (line.rfind("/ping", 0) == 0) {
            std::string arg;
            if (line.size() > 5) {
                arg = line.substr(5);
                while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
            }
            int n = 10;
            if (!arg.empty()) {
                try { n = std::stoi(arg); } catch (...) { n = 10; }
                if (n <= 0) n = 10;
            }
            cmd_ping(n);
        } else if (line == "/netdiag") {
            cmd_netdiag();
        } else if (line.rfind("/w ", 0) == 0) {
            std::string rest = line.substr(3);
            size_t sp = rest.find(' ');
            if (sp == std::string::npos) {
                println("[CLIENT]: usage: /w <nick> <message>");
                continue;
            }
            std::string target = rest.substr(0, sp);
            std::string body = rest.substr(sp + 1);
            MessageEx m;
            make_msg(&m, MSG_PRIVATE, nick, target, body);
            pthread_mutex_lock(&print_mu);
            std::cout << "[Transport][RETRY] send MSG_PRIVATE (id=" << m.msg_id << ")" << std::endl;
            pthread_mutex_unlock(&print_mu);
            send_with_retry(&m);
        } else {
            MessageEx m;
            make_msg(&m, MSG_TEXT, nick, "", line);
            pthread_mutex_lock(&print_mu);
            std::cout << "[Transport][RETRY] send MSG_TEXT (id=" << m.msg_id << ")" << std::endl;
            pthread_mutex_unlock(&print_mu);
            send_with_retry(&m);
        }
    }

    int fd = g_sd.exchange(-1);
    if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); }
    pthread_join(rt, nullptr);
    pthread_cancel(rrt);
    pthread_join(rrt, nullptr);
    return 0;
}
