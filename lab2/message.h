#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdint.h>
#include <stddef.h>

#define MAX_PAYLOAD 1024
#define SERVER_PORT 8888

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

int send_message(int sock, uint8_t type, const char *data, size_t data_len);
int recv_message(int sock, Message *msg);

#endif // MESSAGE_H