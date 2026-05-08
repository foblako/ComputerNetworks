#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <string>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <queue>
#include <vector>
#include <map>
#include <atomic>
#include <endian.h>

#define MAX_NAME    32
#define MAX_PAYLOAD 256
#define PORT        9090
#define POOL_SIZE   10
#define HIST_FILE   "history.json"

typedef struct {
    uint32_t length;                 // длина payload (текста / данных команды)
    uint8_t  type;                   // тип сообщения
    uint32_t msg_id;                 // уникальный идентификатор
    char     sender[MAX_NAME];       // ник отправителя
    char     receiver[MAX_NAME];     // ник получателя или "" если broadcast
    int64_t  timestamp;              // время создания (unix epoch)
    char     payload[MAX_PAYLOAD];   // текст / данные команды
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

struct Client {
    int fd;
    std::string nick;
    std::string addr;
};

struct OfflineMsg {
    uint32_t msg_id;
    int64_t timestamp;
    std::string sender;
    std::string receiver;
    std::string text;
};

struct HistEntry {
    uint32_t msg_id;
    int64_t timestamp;
    std::string sender;
    std::string receiver;
    uint8_t type;
    std::string text;
    bool delivered;
    bool is_offline;
};

// Глобальные данные
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static std::queue<int> q;

static pthread_mutex_t cli_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hist_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t off_mu = PTHREAD_MUTEX_INITIALIZER;

static std::vector<Client> clients;
static std::vector<HistEntry> history;
static std::map<std::string, std::vector<OfflineMsg>> offline_queue;
static std::atomic<uint32_t> next_msg_id{1};

// Чтение ровно n байт
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

// Отправка MessageEx (фиксированный заголовок + payload)
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

// Приём MessageEx
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

// Заполнение MessageEx нулями + базовые поля
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

// Печать под мьютексом
static void print_line(const std::string &s) {
    pthread_mutex_lock(&print_mu);
    std::cout << s << std::endl;
    pthread_mutex_unlock(&print_mu);
}

// Имя типа
static const char *type_name(uint8_t t) {
    switch (t) {
        case MSG_HELLO:        return "MSG_HELLO";
        case MSG_WELCOME:      return "MSG_WELCOME";
        case MSG_TEXT:         return "MSG_TEXT";
        case MSG_PING:         return "MSG_PING";
        case MSG_PONG:         return "MSG_PONG";
        case MSG_BYE:          return "MSG_BYE";
        case MSG_AUTH:         return "MSG_AUTH";
        case MSG_PRIVATE:      return "MSG_PRIVATE";
        case MSG_ERROR:        return "MSG_ERROR";
        case MSG_SERVER_INFO:  return "MSG_SERVER_INFO";
        case MSG_LIST:         return "MSG_LIST";
        case MSG_HISTORY:      return "MSG_HISTORY";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        case MSG_HELP:         return "MSG_HELP";
        default:               return "MSG_UNKNOWN";
    }
}

// TCP/IP логи приёма
static void log_recv_tcpip(uint8_t type, uint32_t bytes) {
    pthread_mutex_lock(&print_mu);
    std::cout << "[Network Access] frame received via network interface" << std::endl;
    std::cout << "[Internet] src=127.0.0.1 dst=127.0.0.1 proto=TCP" << std::endl;
    std::cout << "[Transport] recv() " << bytes << " bytes via TCP" << std::endl;
    std::cout << "[Application] deserialize MessageEx -> " << type_name(type) << std::endl;
    pthread_mutex_unlock(&print_mu);
}

// TCP/IP логи отправки
static void log_send_tcpip(uint8_t type) {
    pthread_mutex_lock(&print_mu);
    std::cout << "[Application] prepare " << type_name(type) << std::endl;
    std::cout << "[Transport] send() via TCP" << std::endl;
    std::cout << "[Internet] destination ip = 127.0.0.1" << std::endl;
    std::cout << "[Network Access] frame sent to network interface" << std::endl;
    pthread_mutex_unlock(&print_mu);
}

// Обёртка send_msgex с логом
static int send_logged(int fd, const MessageEx *m) {
    log_send_tcpip(m->type);
    return send_msgex(fd, m);
}

// Размер сериализованного сообщения
static uint32_t msgex_wire_size(const MessageEx *m) {
    return 4 + 1 + 4 + MAX_NAME + MAX_NAME + 8 + m->length;
}

// Форматирование времени для отображения
static std::string fmt_time(int64_t ts) {
    char buf[32];
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

// Форматирование записи истории для показа клиенту
static std::string format_history_line(const HistEntry &h) {
    std::ostringstream oss;
    oss << "[" << fmt_time(h.timestamp) << "][id=" << h.msg_id << "]";
    if (h.is_offline) oss << "[OFFLINE]";
    if (h.type == MSG_PRIVATE) {
        oss << "[PRIVATE][" << h.sender << " -> " << h.receiver << "]: " << h.text;
    } else {
        oss << "[" << h.sender << "]: " << h.text;
    }
    return oss.str();
}

// Экранирование строки для JSON
static std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Сохранение всей истории в JSON-файл
static void save_history_unlocked() {
    std::ofstream f(HIST_FILE, std::ios::trunc);
    if (!f) return;
    f << "[\n";
    for (size_t i = 0; i < history.size(); i++) {
        const HistEntry &h = history[i];
        f << "  {\n";
        f << "    \"msg_id\": " << h.msg_id << ",\n";
        f << "    \"timestamp\": " << h.timestamp << ",\n";
        f << "    \"sender\": \"" << json_escape(h.sender) << "\",\n";
        f << "    \"receiver\": \"" << json_escape(h.receiver) << "\",\n";
        f << "    \"type\": \"" << type_name(h.type) << "\",\n";
        f << "    \"text\": \"" << json_escape(h.text) << "\",\n";
        f << "    \"delivered\": " << (h.delivered ? "true" : "false") << ",\n";
        f << "    \"is_offline\": " << (h.is_offline ? "true" : "false") << "\n";
        f << "  }" << (i + 1 < history.size() ? "," : "") << "\n";
    }
    f << "]\n";
}

// Добавление в историю
static void add_history(const HistEntry &h) {
    pthread_mutex_lock(&hist_mu);
    history.push_back(h);
    save_history_unlocked();
    pthread_mutex_unlock(&hist_mu);
    print_line("[Application] append record to history file delivered=" +
               std::string(h.delivered ? "true" : "false"));
}

// Поиск клиента по нику (под cli_mu)
static int find_by_nick(const std::string &nick) {
    for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].nick == nick) return (int)i;
    }
    return -1;
}

// Удаление клиента
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
        pthread_mutex_lock(&cli_mu);
        for (auto &c : clients) {
            MessageEx out;
            make_msg(&out, MSG_SERVER_INFO, "server", c.nick, info);
            send_logged(c.fd, &out);
        }
        pthread_mutex_unlock(&cli_mu);
    }
}

// Broadcast TEXT
static void broadcast_text(const std::string &sender, const std::string &text,
                           uint32_t msg_id, int64_t ts, int skip_fd) {
    std::vector<int> dead;
    pthread_mutex_lock(&cli_mu);
    for (auto &c : clients) {
        if (c.fd == skip_fd) continue;
        MessageEx out;
        memset(&out, 0, sizeof(out));
        out.type = MSG_TEXT;
        out.msg_id = msg_id;
        out.timestamp = ts;
        strncpy(out.sender, sender.c_str(), MAX_NAME - 1);
        uint32_t l = (uint32_t)text.size();
        if (l > MAX_PAYLOAD) l = MAX_PAYLOAD;
        memcpy(out.payload, text.data(), l);
        out.length = l;
        if (send_logged(c.fd, &out) < 0) dead.push_back(c.fd);
    }
    pthread_mutex_unlock(&cli_mu);
    for (int fd : dead) remove_client(fd);
}

// Доставка офлайн-сообщений
static void deliver_offline(const std::string &nick, int fd) {
    std::vector<OfflineMsg> mine;
    pthread_mutex_lock(&off_mu);
    auto it = offline_queue.find(nick);
    if (it != offline_queue.end()) {
        mine = std::move(it->second);
        offline_queue.erase(it);
    }
    pthread_mutex_unlock(&off_mu);

    if (mine.empty()) {
        print_line("[Application] no offline messages for " + nick);
        return;
    }
    print_line("[Application] delivering " + std::to_string(mine.size()) +
               " offline message(s) to " + nick);

    for (auto &o : mine) {
        // Сообщение помечается префиксом [OFFLINE] в тексте
        std::string body = "[OFFLINE] " + o.text;
        MessageEx out;
        memset(&out, 0, sizeof(out));
        out.type = MSG_PRIVATE;
        out.msg_id = o.msg_id;
        out.timestamp = o.timestamp;
        strncpy(out.sender, o.sender.c_str(), MAX_NAME - 1);
        strncpy(out.receiver, o.receiver.c_str(), MAX_NAME - 1);
        uint32_t l = (uint32_t)body.size();
        if (l > MAX_PAYLOAD) l = MAX_PAYLOAD;
        memcpy(out.payload, body.data(), l);
        out.length = l;
        if (send_logged(fd, &out) < 0) break;

        // Помечаем в истории как доставленное
        pthread_mutex_lock(&hist_mu);
        for (auto &h : history) {
            if (h.msg_id == o.msg_id) {
                h.delivered = true;
                break;
            }
        }
        save_history_unlocked();
        pthread_mutex_unlock(&hist_mu);
    }
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

    MessageEx msg{};

    // HELLO/WELCOME (из ЛР3) — короткий обмен, без логов
    if (recv_msgex(fd, &msg) < 0 || msg.type != MSG_HELLO) {
        close(fd);
        return;
    }
    MessageEx wel;
    make_msg(&wel, MSG_WELCOME, "server", "", addrbuf);
    if (send_msgex(fd, &wel) < 0) {
        close(fd);
        return;
    }
    print_line("Client connected");

    // Ожидание MSG_AUTH
    std::string nick;
    while (true) {
        memset(&msg, 0, sizeof(msg));
        if (recv_msgex(fd, &msg) < 0) {
            close(fd);
            return;
        }
        log_recv_tcpip(msg.type, msgex_wire_size(&msg));
        if (msg.type != MSG_AUTH) {
            MessageEx err;
            make_msg(&err, MSG_ERROR, "server", "", "auth required");
            send_logged(fd, &err);
            continue;
        }
        std::string candidate = msg.sender;
        if (candidate.empty() && msg.length > 0) candidate.assign(msg.payload, msg.length);
        if (candidate.empty()) {
            MessageEx err;
            make_msg(&err, MSG_ERROR, "server", "", "empty nickname");
            send_logged(fd, &err);
            close(fd);
            return;
        }
        if (candidate.size() >= MAX_NAME) candidate.resize(MAX_NAME - 1);
        pthread_mutex_lock(&cli_mu);
        if (find_by_nick(candidate) >= 0) {
            pthread_mutex_unlock(&cli_mu);
            MessageEx err;
            make_msg(&err, MSG_ERROR, "server", "", "nickname already taken");
            send_logged(fd, &err);
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
        print_line("[Application] authentication success: " + nick);
        break;
    }

    // Уведомление о подключении и подтверждение клиенту
    std::string conn_info = "User [" + nick + "] connected";
    print_line(conn_info);
    pthread_mutex_lock(&cli_mu);
    for (auto &c : clients) {
        if (c.fd == fd) continue;
        MessageEx out;
        make_msg(&out, MSG_SERVER_INFO, "server", c.nick, conn_info);
        send_logged(c.fd, &out);
    }
    pthread_mutex_unlock(&cli_mu);

    MessageEx self;
    make_msg(&self, MSG_SERVER_INFO, "server", nick, "authenticated as " + nick);
    send_logged(fd, &self);

    // Доставка офлайн-сообщений
    deliver_offline(nick, fd);

    // Главный цикл
    while (true) {
        memset(&msg, 0, sizeof(msg));
        if (recv_msgex(fd, &msg) < 0) {
            remove_client(fd);
            return;
        }
        log_recv_tcpip(msg.type, msgex_wire_size(&msg));

        if (msg.type == MSG_TEXT) {
            std::string body(msg.payload, msg.length);
            uint32_t mid = next_msg_id.fetch_add(1);
            int64_t ts = (int64_t)time(nullptr);
            std::string line = "[" + fmt_time(ts) + "][id=" + std::to_string(mid) +
                               "][" + nick + "]: " + body;
            print_line(line);

            HistEntry h;
            h.msg_id = mid;
            h.timestamp = ts;
            h.sender = nick;
            h.receiver = "";
            h.type = MSG_TEXT;
            h.text = body;
            h.delivered = true;
            h.is_offline = false;
            add_history(h);

            broadcast_text(nick, body, mid, ts, fd);
        } else if (msg.type == MSG_PRIVATE) {
            std::string target = msg.receiver;
            std::string body(msg.payload, msg.length);

            if (target.empty()) {
                MessageEx err;
                make_msg(&err, MSG_ERROR, "server", nick, "missing receiver");
                send_logged(fd, &err);
                continue;
            }

            int dst_fd = -1;
            pthread_mutex_lock(&cli_mu);
            int idx = find_by_nick(target);
            if (idx >= 0) dst_fd = clients[idx].fd;
            pthread_mutex_unlock(&cli_mu);

            uint32_t mid = next_msg_id.fetch_add(1);
            int64_t ts = (int64_t)time(nullptr);

            if (dst_fd < 0) {
                // Адресат не в сети — сохраняем в офлайн-очередь
                print_line("[Application] receiver " + target + " is offline");
                print_line("[Application] store message in offline queue");

                OfflineMsg om;
                om.msg_id = mid;
                om.timestamp = ts;
                om.sender = nick;
                om.receiver = target;
                om.text = body;
                pthread_mutex_lock(&off_mu);
                offline_queue[target].push_back(om);
                pthread_mutex_unlock(&off_mu);

                HistEntry h;
                h.msg_id = mid;
                h.timestamp = ts;
                h.sender = nick;
                h.receiver = target;
                h.type = MSG_PRIVATE;
                h.text = body;
                h.delivered = false;
                h.is_offline = true;
                add_history(h);

                MessageEx info;
                make_msg(&info, MSG_SERVER_INFO, "server", nick,
                         "queued for offline delivery to " + target);
                send_logged(fd, &info);
            } else {
                std::string line = "[" + fmt_time(ts) + "][id=" + std::to_string(mid) +
                                   "][PRIVATE][" + nick + " -> " + target + "]: " + body;
                print_line(line);

                MessageEx out;
                memset(&out, 0, sizeof(out));
                out.type = MSG_PRIVATE;
                out.msg_id = mid;
                out.timestamp = ts;
                strncpy(out.sender, nick.c_str(), MAX_NAME - 1);
                strncpy(out.receiver, target.c_str(), MAX_NAME - 1);
                uint32_t l = (uint32_t)body.size();
                if (l > MAX_PAYLOAD) l = MAX_PAYLOAD;
                memcpy(out.payload, body.data(), l);
                out.length = l;
                int rc = send_logged(dst_fd, &out);

                HistEntry h;
                h.msg_id = mid;
                h.timestamp = ts;
                h.sender = nick;
                h.receiver = target;
                h.type = MSG_PRIVATE;
                h.text = body;
                h.delivered = (rc == 0);
                h.is_offline = false;
                add_history(h);

                if (rc < 0) remove_client(dst_fd);
            }
        } else if (msg.type == MSG_PING) {
            MessageEx out;
            make_msg(&out, MSG_PONG, "server", nick, "");
            if (send_logged(fd, &out) < 0) {
                remove_client(fd);
                return;
            }
        } else if (msg.type == MSG_LIST) {
            std::string list;
            pthread_mutex_lock(&cli_mu);
            for (auto &c : clients) {
                if (!list.empty()) list += "\n";
                list += c.nick;
            }
            pthread_mutex_unlock(&cli_mu);
            MessageEx out;
            make_msg(&out, MSG_SERVER_INFO, "server", nick, "Online users\n" + list);
            send_logged(fd, &out);
        } else if (msg.type == MSG_HISTORY) {
            // payload — число записей (может быть пусто = все/последние N=20)
            std::string arg(msg.payload, msg.length);
            int n = -1;
            if (!arg.empty()) {
                try { n = std::stoi(arg); } catch (...) { n = -1; }
            }
            if (n < 0) n = 20;
            if (n > 1000) n = 1000;

            std::string body;
            pthread_mutex_lock(&hist_mu);
            int start = (int)history.size() - n;
            if (start < 0) start = 0;
            for (size_t i = (size_t)start; i < history.size(); i++) {
                if (!body.empty()) body += "\n";
                body += format_history_line(history[i]);
            }
            pthread_mutex_unlock(&hist_mu);

            if (body.size() > MAX_PAYLOAD) body.resize(MAX_PAYLOAD);

            MessageEx out;
            make_msg(&out, MSG_HISTORY_DATA, "server", nick, body);
            send_logged(fd, &out);
        } else if (msg.type == MSG_HELP) {
            std::string text =
                "/help\n/list\n/history\n/history N\n/quit\n/w <nick> <message>\n/ping";
            MessageEx out;
            make_msg(&out, MSG_SERVER_INFO, "server", nick, text);
            send_logged(fd, &out);
        } else if (msg.type == MSG_BYE) {
            remove_client(fd);
            return;
        }
    }
}

// Рабочий поток
static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_mu);
        while (q.empty()) pthread_cond_wait(&q_cv, &q_mu);
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
    if (sd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in saddr{};
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(PORT);
    saddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sd, (sockaddr*)&saddr, sizeof(saddr)) < 0) { perror("bind"); close(sd); return 1; }
    if (listen(sd, 32) < 0) { perror("listen"); close(sd); return 1; }

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
    print_line("[Application] SYN -> ACK -> READY");
    print_line("[Application] coffee powered TCP/IP stack initialized");
    print_line("[Application] packets never sleep");

    for (;;) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cd = accept(sd, (sockaddr*)&caddr, &clen);
        if (cd < 0) { perror("accept"); continue; }
        pthread_mutex_lock(&q_mu);
        q.push(cd);
        pthread_cond_signal(&q_cv);
        pthread_mutex_unlock(&q_mu);
    }
}
