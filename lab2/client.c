#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include "message.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_ip> <nickname>\n", argv[0]);
        return 1;
    }
    
    const char *server_ip = argv[1];
    const char *nickname = argv[2];
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", server_ip);
        close(sock);
        return 1;
    }
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    printf("Connected\n");
    
    send_message(sock, MSG_HELLO, nickname, strlen(nickname));
    
    Message msg;
    if (recv_message(sock, &msg) < 0 || msg.type != MSG_WELCOME) {
        fprintf(stderr, "Expected MSG_WELCOME\n");
        close(sock);
        return 1;
    }
    printf("Welcome %s\n", msg.payload);
    
    int running = 1;
    char input[2048];
    
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sock, &readfds);
        int maxfd = (STDIN_FILENO > sock) ? STDIN_FILENO : sock;
        
        int ret = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ret < 0) {
            perror("select");
            break;
        }
        
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(input, sizeof(input), stdin)) break;
            
            input[strcspn(input, "\n")] = '\0';
            
            if (strcmp(input, "/quit") == 0) {
                send_message(sock, MSG_BYE, NULL, 0);
                running = 0;
            } else if (strcmp(input, "/ping") == 0) {
                send_message(sock, MSG_PING, NULL, 0);
            } else if (strlen(input) > 0) {
                send_message(sock, MSG_TEXT, input, strlen(input));
            }
        }
        
        if (FD_ISSET(sock, &readfds)) {
            if (recv_message(sock, &msg) < 0) {
                printf("Server disconnected\n");
                break;
            }
            
            switch (msg.type) {
                case MSG_TEXT:
                    printf("%s\n", msg.payload);
                    break;
                case MSG_PONG:
                    printf("PONG\n");
                    break;
                case MSG_BYE:
                    printf("Server sent BYE\n");
                    running = 0;
                    break;
                case MSG_WELCOME:
                    break;
                default:
                    fprintf(stderr, "Unknown message type: %d\n", msg.type);
            }
        }
    }
    
    close(sock);
    printf("Disconnected\n");
    return 0;
}