#include <iostream>
#include <cstring>
#include <string>
#include <csignal>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <queue>
#include <vector>

#define MAX_PAYLOAD 1024
#define MAX_NICK    32
#define PORT        9090
#define POOL_SIZE   10

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

struct Client {
    int fd;
    std::string nick;
    std::string addr;
};

// Глобальные данные
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static std::queue<int> q;

static pthread_mutex_t cli_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;
static std::vector<Client> clients;

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

// Печать строки под мьютексом
static void print_line(const std::string &s) {
    pthread_mutex_lock(&print_mu);
    std::cout << s << std::endl;
    pthread_mutex_unlock(&print_mu);
}

// Имя типа для логов
static const char *type_name(uint8_t t) {
    switch (t) {
        case MSG_HELLO:       return "MSG_HELLO";
        case MSG_WELCOME:     return "MSG_WELCOME";
        case MSG_TEXT:        return "MSG_TEXT";
        case MSG_PING:        return "MSG_PING";
        case MSG_PONG:        return "MSG_PONG";
        case MSG_BYE:         return "MSG_BYE";
        case MSG_AUTH:        return "MSG_AUTH";
        case MSG_PRIVATE:     return "MSG_PRIVATE";
        case MSG_ERROR:       return "MSG_ERROR";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        default:              return "MSG_UNKNOWN";
    }
}

// Логи приёма по уровням OSI
static void log_recv(uint8_t type, bool authed) {
    pthread_mutex_lock(&print_mu);
    std::cout << "[Layer 4 - Transport] recv()" << std::endl;
    std::cout << "[Layer 6 - Presentation] deserialize Message" << std::endl;
    std::cout << "[Layer 5 - Session] "
              << (authed ? "client authenticated" : "client not authenticated")
              << std::endl;
    std::cout << "[Layer 7 - Application] handle " << type_name(type) << std::endl;
    pthread_mutex_unlock(&print_mu);
}

// Логи отправки по уровням OSI
static void log_send(uint8_t type) {
    pthread_mutex_lock(&print_mu);
    std::cout << "[Layer 7 - Application] prepare " << type_name(type) << std::endl;
    std::cout << "[Layer 6 - Presentation] serialize Message" << std::endl;
    std::cout << "[Layer 4 - Transport] send()" << std::endl;
    pthread_mutex_unlock(&print_mu);
}

// Обёртка send_msg c логированием уровней
static int send_logged(int fd, uint8_t type, const char *data, uint32_t dlen) {
    log_send(type);
    return send_msg(fd, type, data, dlen);
}

// Поиск клиента по нику (вызывать под cli_mu)
static int find_by_nick(const std::string &nick) {
    for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].nick == nick) return (int)i;
    }
    return -1;
}

// Удаление клиента из списка
static void remove_client(int fd) {
    std::string nick;
    bool found = false;
    pthread_mutex_lock(&cli_mu);
    for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].fd == fd) {
            nick = clients[i].nick;
            close(fd);
            clients.erase(clients.begin() + (long)i);
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&cli_mu);

    if (found && !nick.empty()) {
        std::string info = "User [" + nick + "] disconnected";
        print_line(info);
        // Уведомление остальных
        pthread_mutex_lock(&cli_mu);
        for (auto &c : clients) {
            send_logged(c.fd, MSG_SERVER_INFO, info.c_str(), (uint32_t)info.size());
        }
        pthread_mutex_unlock(&cli_mu);
    }
}

// Широковещательная рассылка
static void broadcast_line(const std::string &line, int skip_fd) {
    std::vector<int> dead;
    pthread_mutex_lock(&cli_mu);
    for (auto &c : clients) {
        if (c.fd == skip_fd) continue;
        if (send_logged(c.fd, MSG_TEXT, line.c_str(), (uint32_t)line.size()) < 0)
            dead.push_back(c.fd);
    }
    pthread_mutex_unlock(&cli_mu);
    for (int fd : dead)
        remove_client(fd);
}

// Обработка сессии клиента
static void handle_session(int fd) {
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    char addrbuf[64];

    if (getpeername(fd, (sockaddr*)&peer, &plen) == 0) {
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ipstr, sizeof(ipstr));
        snprintf(addrbuf, sizeof(addrbuf), "%s:%d", ipstr, ntohs(peer.sin_port));
    } else {
        snprintf(addrbuf, sizeof(addrbuf), "?");
    }

    Message msg{};

    // Начальный обмен HELLO/WELCOME (как в ЛР3)
    if (recv_msg(fd, &msg) < 0 || msg.type != MSG_HELLO) {
        close(fd);
        return;
    }
    if (send_msg(fd, MSG_WELCOME, addrbuf, strlen(addrbuf)) < 0) {
        close(fd);
        return;
    }
    print_line("Client connected");

    // Ожидание MSG_AUTH
    std::string nick;
    while (true) {
        memset(&msg, 0, sizeof(msg));
        if (recv_msg(fd, &msg) < 0) {
            close(fd);
            return;
        }
        log_recv(msg.type, false);

        if (msg.type != MSG_AUTH) {
            // До аутентификации остальные сообщения игнорируем
            const char *err = "auth required";
            send_logged(fd, MSG_ERROR, err, strlen(err));
            continue;
        }

        std::string candidate(msg.payload);
        if (candidate.empty()) {
            const char *err = "empty nickname";
            send_logged(fd, MSG_ERROR, err, strlen(err));
            close(fd);
            return;
        }
        if (candidate.size() >= MAX_NICK) candidate.resize(MAX_NICK - 1);

        pthread_mutex_lock(&cli_mu);
        if (find_by_nick(candidate) >= 0) {
            pthread_mutex_unlock(&cli_mu);
            const char *err = "nickname already taken";
            send_logged(fd, MSG_ERROR, err, strlen(err));
            close(fd);
            return;
        }
        Client me{};
        me.fd = fd;
        me.nick = candidate;
        me.addr = addrbuf;
        clients.push_back(me);
        pthread_mutex_unlock(&cli_mu);

        nick = candidate;
        print_line("[Layer 5 - Session] authentication success");
        break;
    }

    // Уведомление о подключении
    std::string conn_info = "User [" + nick + "] connected";
    print_line(conn_info);

    pthread_mutex_lock(&cli_mu);
    for (auto &c : clients) {
        if (c.fd == fd) continue;
        send_logged(c.fd, MSG_SERVER_INFO, conn_info.c_str(), (uint32_t)conn_info.size());
    }
    pthread_mutex_unlock(&cli_mu);

    // Подтверждаем клиенту, что он авторизован
    std::string self_info = "authenticated as " + nick;
    send_logged(fd, MSG_SERVER_INFO, self_info.c_str(), (uint32_t)self_info.size());

    // Основной цикл обработки
    while (true) {
        memset(&msg, 0, sizeof(msg));
        if (recv_msg(fd, &msg) < 0) {
            remove_client(fd);
            return;
        }
        log_recv(msg.type, true);

        if (msg.type == MSG_TEXT) {
            uint32_t tlen = msg.length > 1 ? msg.length - 1 : 0;
            std::string body(msg.payload, tlen);
            std::string line = "[" + nick + "]: " + body;
            print_line(line);
            broadcast_line(line, fd);
        } else if (msg.type == MSG_PRIVATE) {
            // payload формата "target_nick:message"
            std::string raw(msg.payload, msg.length > 1 ? msg.length - 1 : 0);
            size_t pos = raw.find(':');
            if (pos == std::string::npos) {
                const char *err = "bad private format (expected nick:msg)";
                send_logged(fd, MSG_ERROR, err, strlen(err));
                continue;
            }
            std::string target = raw.substr(0, pos);
            std::string body = raw.substr(pos + 1);

            int idx;
            int dst_fd = -1;
            pthread_mutex_lock(&cli_mu);
            idx = find_by_nick(target);
            if (idx >= 0) dst_fd = clients[idx].fd;
            pthread_mutex_unlock(&cli_mu);

            if (dst_fd < 0) {
                std::string err = "user [" + target + "] not found";
                send_logged(fd, MSG_ERROR, err.c_str(), (uint32_t)err.size());
                continue;
            }

            std::string out = "[PRIVATE][" + nick + "]: " + body;
            print_line(out);
            if (send_logged(dst_fd, MSG_PRIVATE, out.c_str(), (uint32_t)out.size()) < 0)
                remove_client(dst_fd);
        } else if (msg.type == MSG_PING) {
            if (send_logged(fd, MSG_PONG, nullptr, 0) < 0) {
                remove_client(fd);
                return;
            }
        } else if (msg.type == MSG_BYE) {
            remove_client(fd);
            return;
        }
        // прочие типы игнорируем
    }
}

// Рабочий поток пула
static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_mu);
        while (q.empty())
            pthread_cond_wait(&q_cv, &q_mu);
        int fd = q.front();
        q.pop();
        pthread_mutex_unlock(&q_mu);
        handle_session(fd);
    }
    return nullptr;
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in saddr{};
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(PORT);
    saddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sd, (sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("bind");
        close(sd);
        return 1;
    }

    if (listen(sd, 32) < 0) {
        perror("listen");
        close(sd);
        return 1;
    }

    // Создание пула потоков
    pthread_t th[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++) {
        if (pthread_create(&th[i], nullptr, worker, nullptr) != 0) {
            perror("pthread_create");
            close(sd);
            return 1;
        }
        pthread_detach(th[i]);
    }

    print_line("TCP server running on port " + std::to_string(PORT));
    print_line("Thread pool size: " + std::to_string(POOL_SIZE));

    // Главный цикл: приём соединений
    for (;;) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cd = accept(sd, (sockaddr*)&caddr, &clen);
        if (cd < 0) {
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&q_mu);
        q.push(cd);
        pthread_cond_signal(&q_cv);
        pthread_mutex_unlock(&q_mu);
    }
}
