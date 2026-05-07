#include <iostream>
#include <cstring>
#include <string>
#include <atomic>
#include <pthread.h>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>

#define MAX_PAYLOAD 1024
#define SERVER_PORT 9090

typedef struct {
    uint32_t length;
    uint8_t  type;
    char     payload[MAX_PAYLOAD];
} Message;

enum {
    MSG_HELLO       = 1,
    MSG_WELCOME     = 2,
    MSG_TEXT        = 3,
    MSG_PING        = 4,
    MSG_PONG        = 5,
    MSG_BYE         = 6,
    MSG_AUTH        = 7,
    MSG_PRIVATE     = 8,
    MSG_ERROR       = 9,
    MSG_SERVER_INFO = 10
};

static std::atomic<int> g_sd{-1};
static std::atomic<bool> g_run{true};
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;

// Чтение ровно n байт из сокета
int recv_all(int fd, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = recv(fd, (char*)buf + done, n - done, 0);
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}

// Отправка ровно n байт
int send_all(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t s = send(fd, (const char*)buf + done, n - done, 0);
        if (s <= 0) return -1;
        done += s;
    }
    return 0;
}

// Отправка сообщения по протоколу
int send_msg(int fd, uint8_t type, const char *data, uint32_t dlen) {
    uint32_t length = 1 + dlen;
    uint32_t nl = htonl(length);
    if (send_all(fd, &nl, 4) < 0) return -1;
    if (send_all(fd, &type, 1) < 0) return -1;
    if (dlen > 0 && send_all(fd, data, dlen) < 0) return -1;
    return 0;
}

// Приём сообщения
int recv_msg(int fd, Message *msg) {
    uint32_t nl;
    if (recv_all(fd, &nl, 4) < 0) return -1;
    msg->length = ntohl(nl);
    if (msg->length < 1 || msg->length > MAX_PAYLOAD) return -1;
    if (recv_all(fd, &msg->type, 1) < 0) return -1;
    uint32_t plen = msg->length - 1;
    if (plen > 0) {
        if (recv_all(fd, msg->payload, plen) < 0) return -1;
    }
    msg->payload[plen] = '\0';
    return 0;
}

// Поток приёма сообщений
static void *recv_thread(void *arg) {
    (void)arg;
    while (g_run.load()) {
        int fd = g_sd.load();
        if (fd < 0) break;

        Message msg{};
        if (recv_msg(fd, &msg) < 0) {
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
            std::cout << msg.payload << std::endl;
        } else if (msg.type == MSG_PRIVATE) {
            std::cout << msg.payload << std::endl;
        } else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << msg.payload << std::endl;
        } else if (msg.type == MSG_PONG) {
            std::cout << "PONG" << std::endl;
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[ERROR]: " << msg.payload << std::endl;
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

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    const char *host = "127.0.0.1";
    if (argc >= 2) host = argv[1];

    // Запрос никнейма
    std::cout << "Enter nickname: " << std::flush;
    std::string nick;
    if (!std::getline(std::cin, nick) || nick.empty()) {
        std::cerr << "nickname required" << std::endl;
        return 1;
    }

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        perror("socket");
        return 1;
    }

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

    // Начальный обмен HELLO/WELCOME (из ЛР3)
    if (send_msg(sd, MSG_HELLO, nullptr, 0) < 0) {
        close(sd);
        return 1;
    }
    Message msg{};
    if (recv_msg(sd, &msg) < 0 || msg.type != MSG_WELCOME) {
        std::cerr << "no WELCOME" << std::endl;
        close(sd);
        return 1;
    }
    std::cout << "Connected (server addr=" << msg.payload << ")" << std::endl;

    // Аутентификация
    if (send_msg(sd, MSG_AUTH, nick.c_str(), (uint32_t)nick.size()) < 0) {
        close(sd);
        return 1;
    }

    g_sd.store(sd);

    // Поток приёма
    pthread_t rt;
    if (pthread_create(&rt, nullptr, recv_thread, nullptr) != 0) {
        perror("pthread_create");
        close(sd);
        return 1;
    }

    // Основной цикл
    std::string line;
    while (g_run.load()) {
        if (!std::getline(std::cin, line)) break;
        if (!g_run.load()) break;
        if (line.empty()) continue;

        if (line == "/quit") {
            send_msg(sd, MSG_BYE, nullptr, 0);
            g_run.store(false);
            break;
        } else if (line == "/ping") {
            send_msg(sd, MSG_PING, nullptr, 0);
        } else if (line.rfind("/w ", 0) == 0) {
            // /w <nick> <message>
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
            std::string payload = target + ":" + body;
            if ((uint32_t)payload.size() > MAX_PAYLOAD - 1)
                payload.resize(MAX_PAYLOAD - 1);
            send_msg(sd, MSG_PRIVATE, payload.c_str(), (uint32_t)payload.size());
        } else {
            uint32_t plen = (uint32_t)line.size();
            if (plen > MAX_PAYLOAD - 1) plen = MAX_PAYLOAD - 1;
            send_msg(sd, MSG_TEXT, line.c_str(), plen);
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
