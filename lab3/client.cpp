#include <iostream>
#include <cstring>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <atomic>

#define MAX_PAYLOAD 1024
#define PORT 9090
#define RECONNECT_DELAY 2

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

// Глобальные переменные
std::atomic<int> client_socket(-1);
std::atomic<bool> client_running(true);
std::atomic<bool> connected(false);
pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;

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

// Поток приёма сообщений от сервера
void* receive_thread(void *arg) {
    (void)arg;
    Message msg{};
    
    while (client_running) {
        if (!connected) {
            usleep(100000);
            continue;
        }
        
        pthread_mutex_lock(&socket_mutex);
        int sock = client_socket.load();
        pthread_mutex_unlock(&socket_mutex);
        
        if (sock < 0) {
            usleep(100000);
            continue;
        }
        
        memset(&msg, 0, sizeof(msg));
        if (recv_msg(sock, &msg) < 0) {
            std::cout << "\n[Disconnected from server]" << std::endl;
            connected = false;
            continue;
        }
        
        if (msg.type == MSG_TEXT) {
            std::cout << "\n" << msg.payload << std::endl;
            std::cout << "> " << std::flush;
        } else if (msg.type == MSG_PONG) {
            std::cout << "\n[PONG]" << std::endl;
            std::cout << "> " << std::flush;
        } else if (msg.type == MSG_WELCOME) {
            std::cout << "\n[Connected to server: " << msg.payload << "]" << std::endl;
            std::cout << "> " << std::flush;
        } else if (msg.type == MSG_BYE) {
            std::cout << "\n[Server sent BYE]" << std::endl;
            connected = false;
        }
    }
    
    return nullptr;
}

// Подключение к серверу с handshake
int connect_to_server() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    
    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return -1;
    }
    
    // Отправка MSG_HELLO с ником
    const char *nick = "User";
    if (send_msg(sock, MSG_HELLO, nick, strlen(nick)) < 0) {
        close(sock);
        return -1;
    }
    
    // Ожидание MSG_WELCOME
    Message msg{};
    if (recv_msg(sock, &msg) < 0 || msg.type != MSG_WELCOME) {
        close(sock);
        return -1;
    }
    
    return sock;
}

// Поток переподключения
void* reconnect_thread(void *arg) {
    (void)arg;
    
    while (client_running) {
        if (!connected) {
            std::cout << "\n[Attempting to reconnect...]" << std::endl;
            
            int sock = connect_to_server();
            if (sock >= 0) {
                pthread_mutex_lock(&socket_mutex);
                
                // Закрытие старого сокета если есть
                int old_sock = client_socket.load();
                if (old_sock >= 0) {
                    close(old_sock);
                }
                
                client_socket = sock;
                pthread_mutex_unlock(&socket_mutex);
                
                connected = true;
                std::cout << "[Reconnected successfully]" << std::endl;
                std::cout << "> " << std::flush;
            } else {
                std::cout << "[Reconnect failed, retrying in " << RECONNECT_DELAY << "s...]" << std::endl;
                sleep(RECONNECT_DELAY);
            }
        } else {
            sleep(1);
        }
    }
    
    return nullptr;
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    
    std::cout << "TCP Client starting..." << std::endl;
    
    // Создание потока приёма сообщений
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, nullptr, receive_thread, nullptr) != 0) {
        std::cerr << "Failed to create receive thread" << std::endl;
        return 1;
    }
    pthread_detach(recv_thread);
    
    // Создание потока переподключения
    pthread_t recon_thread;
    if (pthread_create(&recon_thread, nullptr, reconnect_thread, nullptr) != 0) {
        std::cerr << "Failed to create reconnect thread" << std::endl;
        return 1;
    }
    pthread_detach(recon_thread);
    
    // Первая попытка подключения
    std::cout << "[Connecting to server...]" << std::endl;
    int sock = connect_to_server();
    if (sock >= 0) {
        client_socket = sock;
        connected = true;
        std::cout << "[Connected successfully]" << std::endl;
    } else {
        std::cout << "[Connection failed, will retry...]" << std::endl;
    }
    
    // Основной цикл ввода
    std::string line;
    while (client_running) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        
        if (line.empty()) {
            continue;
        }
        
        pthread_mutex_lock(&socket_mutex);
        int current_sock = client_socket.load();
        pthread_mutex_unlock(&socket_mutex);
        
        if (!connected || current_sock < 0) {
            std::cout << "[Not connected to server]" << std::endl;
            continue;
        }
        
        if (line == "/ping") {
            if (send_msg(current_sock, MSG_PING, nullptr, 0) < 0) {
                std::cout << "[Failed to send PING]" << std::endl;
                connected = false;
            }
        } else if (line == "/quit") {
            send_msg(current_sock, MSG_BYE, nullptr, 0);
            std::cout << "[Disconnected]" << std::endl;
            break;
        } else {
            uint32_t plen = line.size();
            if (plen > MAX_PAYLOAD - 1) plen = MAX_PAYLOAD - 1;
            if (send_msg(current_sock, MSG_TEXT, line.c_str(), plen) < 0) {
                std::cout << "[Failed to send message]" << std::endl;
                connected = false;
            }
        }
    }
    
    // Остановка
    client_running = false;
    
    pthread_mutex_lock(&socket_mutex);
    int sock_to_close = client_socket.load();
    if (sock_to_close >= 0) {
        close(sock_to_close);
    }
    client_socket = -1;
    pthread_mutex_unlock(&socket_mutex);
    
    // Ожидание завершения потоков
    sleep(1);
    
    std::cout << "[Client exited]" << std::endl;
    
    return 0;
}
