#include <iostream>
#include <sstream>
#include <cstring>
#include <string>
#include <atomic>
#include <pthread.h>
#include <csignal>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <endian.h>

#define MAX_NAME    32
#define MAX_PAYLOAD 256
#define SERVER_PORT 9090

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
    MSG_HELP         = 14
};

static std::atomic<int> g_sd{-1};
static std::atomic<bool> g_run{true};
static std::atomic<uint32_t> next_msg_id{1};
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;

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
    if (m->length > 0) {
        if (send_all(fd, m->payload, m->length) < 0) return -1;
    }
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
    if (m->length > 0) {
        if (recv_all(fd, m->payload, m->length) < 0) return -1;
    }
    if (m->length < MAX_PAYLOAD) m->payload[m->length] = '\0';
    else m->payload[MAX_PAYLOAD - 1] = '\0';
    return 0;
}

// Заполнить сообщение для отправки
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

// Форматирование времени
static std::string fmt_time(int64_t ts) {
    char buf[32];
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
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
                pthread_mutex_lock(&print_mu);
                std::cout << "Disconnected" << std::endl;
                pthread_mutex_unlock(&print_mu);
                g_run.store(false);
            }
            break;
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
        } else if (msg.type == MSG_PONG) {
            std::cout << "[SERVER]: PONG" << std::endl;
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[ERROR]: " << std::string(msg.payload, msg.length) << std::endl;
        } else if (msg.type == MSG_BYE) {
            std::cout << "Disconnected" << std::endl;
            g_run.store(false);
            pthread_mutex_unlock(&print_mu);
            break;
        }
        pthread_mutex_unlock(&print_mu);
    }
    return nullptr;
}

// Локальный /help
static void print_help() {
    pthread_mutex_lock(&print_mu);
    std::cout << "/help" << std::endl;
    std::cout << "/list" << std::endl;
    std::cout << "/history" << std::endl;
    std::cout << "/history N" << std::endl;
    std::cout << "/quit" << std::endl;
    std::cout << "/w <nick> <message>" << std::endl;
    std::cout << "/ping" << std::endl;
    std::cout << "Tip: packets never sleep" << std::endl;
    pthread_mutex_unlock(&print_mu);
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

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) { perror("socket"); return 1; }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        std::cerr << "bad host" << std::endl;
        close(sd);
        return 1;
    }
    if (connect(sd, (sockaddr*)&dest, sizeof(dest)) < 0) {
        perror("connect");
        close(sd);
        return 1;
    }

    // HELLO/WELCOME
    MessageEx hello;
    make_msg(&hello, MSG_HELLO, nick, "", "");
    if (send_msgex(sd, &hello) < 0) { close(sd); return 1; }
    MessageEx wel{};
    if (recv_msgex(sd, &wel) < 0 || wel.type != MSG_WELCOME) {
        std::cerr << "no WELCOME" << std::endl;
        close(sd);
        return 1;
    }
    std::cout << "Connected (server addr=" << wel.payload << ")" << std::endl;

    // AUTH
    MessageEx auth;
    make_msg(&auth, MSG_AUTH, nick, "", nick);
    if (send_msgex(sd, &auth) < 0) { close(sd); return 1; }

    g_sd.store(sd);
    pthread_t rt;
    if (pthread_create(&rt, nullptr, recv_thread, nullptr) != 0) {
        perror("pthread_create");
        close(sd);
        return 1;
    }

    std::string line;
    while (g_run.load()) {
        if (!std::getline(std::cin, line)) break;
        if (!g_run.load()) break;
        if (line.empty()) continue;

        if (line == "/quit") {
            MessageEx m;
            make_msg(&m, MSG_BYE, nick, "", "");
            send_msgex(sd, &m);
            g_run.store(false);
            break;
        } else if (line == "/ping") {
            MessageEx m;
            make_msg(&m, MSG_PING, nick, "", "");
            send_msgex(sd, &m);
        } else if (line == "/list") {
            MessageEx m;
            make_msg(&m, MSG_LIST, nick, "", "");
            send_msgex(sd, &m);
        } else if (line == "/help") {
            print_help();
        } else if (line.rfind("/history", 0) == 0) {
            std::string arg;
            if (line.size() > 8) {
                arg = line.substr(8);
                while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
            }
            // Если есть число — отдаём как payload, иначе пусто
            int n = -1;
            if (!arg.empty()) {
                try { n = std::stoi(arg); } catch (...) { n = -1; }
                if (n <= 0) {
                    pthread_mutex_lock(&print_mu);
                    std::cout << "[CLIENT]: bad N for /history" << std::endl;
                    pthread_mutex_unlock(&print_mu);
                    continue;
                }
            }
            MessageEx m;
            make_msg(&m, MSG_HISTORY, nick, "", n > 0 ? std::to_string(n) : "");
            send_msgex(sd, &m);
        } else if (line.rfind("/w ", 0) == 0) {
            std::string rest = line.substr(3);
            size_t sp = rest.find(' ');
            if (sp == std::string::npos) {
                pthread_mutex_lock(&print_mu);
                std::cout << "[CLIENT]: usage: /w <nick> <message>" << std::endl;
                pthread_mutex_unlock(&print_mu);
                continue;
            }
            std::string target = rest.substr(0, sp);
            std::string body = rest.substr(sp + 1);
            MessageEx m;
            make_msg(&m, MSG_PRIVATE, nick, target, body);
            send_msgex(sd, &m);
        } else {
            MessageEx m;
            make_msg(&m, MSG_TEXT, nick, "", line);
            send_msgex(sd, &m);
        }
    }

    int fd = g_sd.exchange(-1);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    pthread_join(rt, nullptr);
    return 0;
}
