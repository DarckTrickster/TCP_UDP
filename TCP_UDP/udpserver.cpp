#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <locale.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#define MAX_PORTS 64
#define BUFFER_SIZE 8192
#define CLIENT_TIMEOUT_SECONDS 30
#define MAX_CLIENTS 256

typedef struct {
    struct sockaddr_in address;
    uint32_t *messageIds;
    int idCount;
    int idCapacity;
    time_t lastSeen;
} Client;

Client clients[MAX_CLIENTS];
int clientCount = 0;
//Сравнивает два адреса sockaddr_in
int sockAddressEqual(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_family == b->sin_family &&
           a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}
//Форматирует адрес клиента в строку IP:порт
char *formatClient(const struct sockaddr_in *address, char *buffer, int bufferLength)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, (const void*)&address->sin_addr, ip, sizeof(ip));
    snprintf(buffer, bufferLength, "%s:%d", ip, ntohs(address->sin_port));
    return buffer;
}
//Ищет клиента по адресу
Client *findClient(const struct sockaddr_in *address)
{
    for (int i = 0; i < clientCount; i++)
        if (sockAddressEqual(&clients[i].address, address))
            return &clients[i];
    return NULL;
}
//Удаляет клиентов, неактивных более 30 секунд
void removeClients()
{
    time_t now = time(NULL);
    for (int i = 0; i < clientCount;)
    {
        if (now - clients[i].lastSeen > CLIENT_TIMEOUT_SECONDS)
        {
            free(clients[i].messageIds);
            clients[i] = clients[--clientCount];
        }
        else
            i++;
    }
}
//Возвращает существующий клиент или создаёт новый
Client *getClient(const struct sockaddr_in *address)
{
    Client *client = findClient(address);
    if (client)
    {
        client->lastSeen = time(NULL);
        return client;
    }
    if (clientCount >= MAX_CLIENTS)
        return NULL;

    clients[clientCount].address = *address;
    clients[clientCount].messageIds = NULL;
    clients[clientCount].idCount = 0;
    clients[clientCount].idCapacity = 0;
    clients[clientCount].lastSeen = time(NULL);
    return &clients[clientCount++];
}
//Проверяет, получал ли клиент сообщение с заданным messageId
int hasClientMessageId(Client *client, uint32_t messageId)
{
    for (int i = 0; i < client->idCount; i++)
        if (client->messageIds[i] == messageId)
            return 1;
    return 0;
}
//Добавляет ID сообщения в массив клиента
void addClientMessageId(Client *client, uint32_t messageId)
{
    if (hasClientMessageId(client, messageId)) 
        return;
    if (client->idCount == client->idCapacity)
    {
        int newCap = client->idCapacity ? client->idCapacity * 2 : 8;
        client->messageIds = (uint32_t*)realloc(client->messageIds, newCap * sizeof(uint32_t));
        client->idCapacity = newCap;
    }
    client->messageIds[client->idCount++] = messageId;
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    if (argc != 3)
    {
        fprintf(stderr, "Формат: %s <startPort> <endPort>\n", argv[0]);
        return 1;
    }

    int startPort = atoi(argv[1]);
    int endPort = atoi(argv[2]);
    if (endPort < startPort || endPort - startPort + 1 > MAX_PORTS)
    {
        fprintf(stderr, "Неверный диапазон портов.\n");
        return 1;
    }

    int socketCount = 0;
    int udpSockets[MAX_PORTS];
    for (int i = 0; i < MAX_PORTS; i++) 
        udpSockets[i] = -1;

    for (int port = startPort; port <= endPort; port++)
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) 
        {
            perror("socket");
            continue;
        }

        int flags = fcntl(sock, F_GETFL, 0);
        if (flags < 0) flags = 0;
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); //Позволяет повторно использовать порт, если он занят

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        {
            close(sock);
            continue;
        }

        udpSockets[socketCount] = sock;
        printf("Прослушивается UDP порт %d\n", port);
        socketCount++;
    }

    if (socketCount == 0)
    {
        fprintf(stderr, "Не удалось инициализировать ни один порт.\n");
        return 1;
    }

    FILE *messageFile = fopen("msg.txt", "a");
    if (!messageFile)
    {
        perror("Не удалось открыть msg.txt");
        for (int i = 0; i < socketCount; i++) 
            close(udpSockets[i]);
        return 1;
    }

    char buffer[BUFFER_SIZE];
    int running = 1;
    fd_set readfds;
    struct timeval tv;

    while (running) //Настраивает select для проверки сокетов
    {
        FD_ZERO(&readfds);// очистить множество
        int maxfd = -1;
        for (int i = 0; i < socketCount; i++)
        {
            if (udpSockets[i] >= 0) {
                FD_SET(udpSockets[i], &readfds);// добавить дескриптор
                if (udpSockets[i] > maxfd)
                    maxfd = udpSockets[i];
            }
        }

        tv.tv_sec = 1;// 1 секунда
        tv.tv_usec = 0; // 0 микросекунд
        int sel = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        removeClients();

        if (sel < 0) {
            if (errno == EINTR) 
                continue;
            perror("select");
            break;
        }

        if (sel == 0)
            continue;

        for (int i = 0; i < socketCount; i++)
        {
            int sock = udpSockets[i];
            if (sock < 0) continue;
            if (!FD_ISSET(sock, &readfds)) continue;// проверка готовности

            struct sockaddr_in clientAddr; //Получает датаграмму
            socklen_t addrLen = sizeof(clientAddr);
            ssize_t len = recvfrom(sock, buffer, BUFFER_SIZE, 0, 
                (struct sockaddr*)&clientAddr, &addrLen);
            if (len < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
                perror("recvfrom");
                continue;
            }
            if (len < 20)
                continue;

            Client *client = getClient(&clientAddr);
            if (!client) 
                continue;

            uint32_t msgId_net; //Парсит заголовок датаграммы:
            memcpy(&msgId_net, buffer, 4);
            uint32_t msgId = ntohl(msgId_net);

            uint32_t date_net, t1_net, t2_net, msglen_net;
            memcpy(&date_net, buffer + 4, 4);
            memcpy(&t1_net,   buffer + 8, 4);
            memcpy(&t2_net,   buffer + 12,4);
            memcpy(&msglen_net, buffer + 16, 4);

            uint32_t date = ntohl(date_net);
            uint32_t t1 = ntohl(t1_net);
            uint32_t t2 = ntohl(t2_net);
            uint32_t msglen = ntohl(msglen_net);

            if (msglen > (uint32_t)(len - 20)) 
                continue;

            char *text = (char*)malloc(msglen + 1);
            if (msglen > 0)
                memcpy(text, buffer + 20, msglen);
            text[msglen] = '\0';

            if (!hasClientMessageId(client, msgId))
            {
                int yyyy = date / 10000;
                int mm = (date / 100) % 100;
                int dd = date % 100;
                int h1 = t1 / 10000;
                int mi1 = (t1 / 100) % 100;
                int s1 = t1 % 100;
                int h2 = t2 / 10000;
                int mi2 = (t2 / 100) % 100;
                int s2 = t2 % 100;

                char idStr[64];
                formatClient(&clientAddr, idStr, sizeof(idStr));
                fprintf(messageFile, "%s %02d.%02d.%04d %02d:%02d:%02d %02d:%02d:%02d %s\n",
                        idStr, dd, mm, yyyy, h1, mi1, s1, h2, mi2, s2, text);
                fflush(messageFile);

                addClientMessageId(client, msgId);
            }

            int toSendCount = client->idCount < 20 ? client->idCount : 20;
            uint32_t respBuf[20];
            for (int k = 0; k < toSendCount; k++)
            {
                uint32_t id = client->messageIds[client->idCount - 1 - k];
                respBuf[k] = htonl(id);
            }
            ssize_t sent = sendto(sock, (char*)respBuf, toSendCount * 4, 0,
                                  (struct sockaddr*)&clientAddr, addrLen);
            if (sent < 0) 
                perror("sendto");

            if (strcmp(text, "stop") == 0)
            {
                free(text);
                running = 0;
                break;
            }
            free(text);
        }
    }

    fclose(messageFile);
    for (int i = 0; i < socketCount; i++)
        if (udpSockets[i] >= 0) 
            close(udpSockets[i]);
    for (int i = 0; i < clientCount; i++) 
        free(clients[i].messageIds);

    printf("Сервер завершил работу.\n");
    return 0;
}
