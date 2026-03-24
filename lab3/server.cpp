#include <iostream>
#include <cstring>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <queue>
#include <vector>
#include <map>

#define MAX_PAYLOAD 1024
#define PORT 9090
#define THREAD_POOL_SIZE 10

typedef struct {
    uint32_t length;
    uint8_t  type;
    char     payload[MAX_PAYLOAD];
} Message;

enum {
    MSG_HELLO   = 1,
    MSG_WELCOME = 2,
    MSG_TEXT    = 3,
    MSG_PING    = 4,
    MSG_PONG    = 5,
    MSG_BYE     = 6
};

// Информация о клиенте
struct ClientInfo {
    int socket;
    char nick[64];
    char addr_str[64];
};

// Глобальные данные
std::queue<int> connection_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

std::map<int, ClientInfo> clients;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

bool server_running = true;

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
    if (msg->length < 1 || msg->length > MAX_PAYLOAD + 1) return -1;
    if (recv_all(fd, &msg->type, 1) < 0) return -1;
    uint32_t plen = msg->length - 1;
    if (plen > 0) {
        if (recv_all(fd, msg->payload, plen) < 0) return -1;
    }
    msg->payload[plen] = '\0';
    return 0;
}

// Широковещательная рассылка всем клиентам
void broadcast_message(const char *sender_addr, const char *text) {
    pthread_mutex_lock(&clients_mutex);
    
    char message[1024];
    snprintf(message, sizeof(message), "%s: %s", sender_addr, text);
    
    std::vector<int> to_remove;
    
    for (auto& pair : clients) {
        int client_fd = pair.first;
        if (send_msg(client_fd, MSG_TEXT, message, strlen(message)) < 0) {
            to_remove.push_back(client_fd);
        }
    }
    
    // Удаление отключившихся клиентов
    for (int fd : to_remove) {
        std::cout << "Client disconnected: " << clients[fd].nick << " [" << clients[fd].addr_str << "]" << std::endl;
        close(fd);
        clients.erase(fd);
    }
    
    pthread_mutex_unlock(&clients_mutex);
}

// Обработка клиента в рабочем потоке
void* client_handler(void *arg) {
    int client_fd = *(int*)arg;
    delete (int*)arg;
    
    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    getpeername(client_fd, (sockaddr*)&caddr, &clen);
    
    char addr_str[64];
    snprintf(addr_str, sizeof(addr_str), "%s:%d",
             inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
    
    Message msg{};
    
    // Ожидание MSG_HELLO
    if (recv_msg(client_fd, &msg) < 0 || msg.type != MSG_HELLO) {
        std::cerr << "No HELLO from " << addr_str << std::endl;
        close(client_fd);
        return nullptr;
    }
    
    // Сохранение ника
    ClientInfo info{};
    info.socket = client_fd;
    strncpy(info.nick, msg.payload, sizeof(info.nick) - 1);
    info.nick[sizeof(info.nick) - 1] = '\0';
    strncpy(info.addr_str, addr_str, sizeof(info.addr_str) - 1);
    info.addr_str[sizeof(info.addr_str) - 1] = '\0';
    
    // Отправка MSG_WELCOME
    if (send_msg(client_fd, MSG_WELCOME, addr_str, strlen(addr_str)) < 0) {
        close(client_fd);
        return nullptr;
    }
    
    // Добавление в список клиентов
    pthread_mutex_lock(&clients_mutex);
    clients[client_fd] = info;
    pthread_mutex_unlock(&clients_mutex);
    
    std::cout << "Client connected: " << info.nick << " [" << addr_str << "]" << std::endl;
    
    // Основной цикл обработки сообщений
    while (true) {
        memset(&msg, 0, sizeof(msg));
        if (recv_msg(client_fd, &msg) < 0) {
            std::cout << "Client disconnected: " << info.nick << " [" << addr_str << "]" << std::endl;
            break;
        }
        
        if (msg.type == MSG_TEXT) {
            broadcast_message(addr_str, msg.payload);
        } else if (msg.type == MSG_PING) {
            send_msg(client_fd, MSG_PONG, nullptr, 0);
        } else if (msg.type == MSG_BYE) {
            std::cout << "Client disconnected: " << info.nick << " [" << addr_str << "]" << std::endl;
            break;
        }
    }
    
    // Удаление клиента из списка
    pthread_mutex_lock(&clients_mutex);
    close(client_fd);
    clients.erase(client_fd);
    pthread_mutex_unlock(&clients_mutex);
    
    return nullptr;
}

// Рабочий поток пула
void* worker_thread(void *arg) {
    (void)arg;
    
    while (server_running) {
        pthread_mutex_lock(&queue_mutex);
        
        // Ожидание появления соединения в очереди
        while (connection_queue.empty() && server_running) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        
        if (!server_running && connection_queue.empty()) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        
        // Извлечение сокета из очереди
        int client_fd = connection_queue.front();
        connection_queue.pop();
        
        pthread_mutex_unlock(&queue_mutex);
        
        // Обработка клиента
        client_handler((void*)(intptr_t)client_fd);
    }
    
    return nullptr;
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in saddr{};
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(PORT);
    saddr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(server_fd, (sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }
    
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }
    
    std::cout << "TCP server running on port " << PORT << std::endl;
    std::cout << "Thread pool size: " << THREAD_POOL_SIZE << std::endl;
    
    // Создание пула потоков
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&threads[i], nullptr, worker_thread, nullptr) != 0) {
            std::cerr << "Failed to create thread " << i << std::endl;
        } else {
            pthread_detach(threads[i]);
        }
    }
    
    std::cout << "Thread pool created" << std::endl;
    
    // Главный цикл: приём соединений
    while (server_running) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int client_fd = accept(server_fd, (sockaddr*)&caddr, &clen);
        
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        
        // Добавление сокета в очередь
        pthread_mutex_lock(&queue_mutex);
        connection_queue.push(client_fd);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }
    
    // Остановка сервера
    pthread_mutex_lock(&queue_mutex);
    server_running = false;
    pthread_cond_broadcast(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    
    close(server_fd);
    
    return 0;
}
