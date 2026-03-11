#include "message.h"
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int send_message(int sock, uint8_t type, const char *data, size_t data_len) {
    Message msg;
    
    if (data_len > MAX_PAYLOAD) data_len = MAX_PAYLOAD;
    
    msg.length = htonl(sizeof(uint8_t) + data_len);
    msg.type = type;
    memset(msg.payload, 0, MAX_PAYLOAD);
    if (data && data_len > 0) {
        memcpy(msg.payload, data, data_len);
    }
    
    size_t total = sizeof(msg.length) + sizeof(msg.type) + data_len;
    size_t sent = 0;
    
    while (sent < total) {
        ssize_t n = send(sock, (char*)&msg + sent, total - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

int recv_message(int sock, Message *msg) {
    size_t header_size = sizeof(msg->length) + sizeof(msg->type);
    size_t received = 0;
    
    while (received < header_size) {
        ssize_t n = recv(sock, (char*)msg + received, header_size - received, 0);
        if (n <= 0) return -1;
        received += n;
    }
    
    msg->length = ntohl(msg->length);
    
    if (msg->length < sizeof(uint8_t) || msg->length > sizeof(uint8_t) + MAX_PAYLOAD) {
        return -1;
    }
    
    size_t payload_len = msg->length - sizeof(uint8_t);
    size_t payload_received = 0;
    
    while (payload_received < payload_len) {
        ssize_t n = recv(sock, msg->payload + payload_received, payload_len - payload_received, 0);
        if (n <= 0) return -1;
        payload_received += n;
    }

    if (payload_len < MAX_PAYLOAD) {
        msg->payload[payload_len] = '\0';
    } else {
        msg->payload[MAX_PAYLOAD - 1] = '\0';
    }
    
    return 0;
}