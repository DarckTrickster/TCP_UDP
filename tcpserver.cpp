#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <locale.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib") //Подключает библиотеку Winsock

#define MAX_CLIENTS 100  // Максимвльное количество клиентов
#define BUFFER_SIZE 8192  // Длина буфера сообщения
#define HEADER_SIZE 20  // Длина заголовка

// Клиент
typedef struct 
{
    SOCKET fd;  // Сокет
    struct sockaddr_in address;  // Адрес
    char command[4];  // Команда (put, ...)
    char* buffer;  // Буфер
    size_t bufferSize;  // Длина буфера
} Client;

// Печать адреса клиента
void printAddress(const char* prefix, struct sockaddr_in* addr);

int main(int argc, char* argv[]) 
{
    setlocale(LC_ALL, "rus");

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) 
    {
        fprintf(stderr, "Ошибка инициализации WinSock\n");
        return 1;
    }

    int port = 9000;
    if (argc > 2) 
    {
        fprintf(stderr, "Формат: %s <port>\n", argv[0]);
        WSACleanup();
        return 1;
    } 
    else if (argc == 2) 
    {
        port = atoi(argv[1]);
    }

    SOCKET serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd == INVALID_SOCKET) 
    {
        perror("Ошибка создания сокета");
        WSACleanup();
        return 1;
    }

    // Перевод в неблокирующий режим 
    u_long mode = 1;
    ioctlsocket(serverFd, FIONBIO, &mode);

    // Адрес сервера
    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;// Указывает, что используется IPv4
    serverAddress.sin_port = htons(port); //Устанавливает порт, преобразованный в сетевой порядок байтов
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);//Сервер принимает подключения на любом сетевом интерфейсе

    // Биндинг адреса сервера
    if (bind(serverFd, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) == SOCKET_ERROR) //cвязывает сокет с адресом serverAddress
    {
        perror("Ошибка биндинга адреса");
        closesocket(serverFd);
        WSACleanup();
        return 1;
    }

    // Запуск прослушки порта
    if (listen(serverFd, SOMAXCONN) == SOCKET_ERROR) 
    {
        perror("Ошибка запуска прослушки");
        closesocket(serverFd);
        WSACleanup();
        return 1;
    }
    printf("Слушается TCP порт: %d\n", port);

    // Открытие файла 'msg.txt' для записи
    FILE* messageFile = fopen("msg.txt", "a");
    if (!messageFile) 
    {
        perror("Ошибка открытия файла 'msg.txt'");
        closesocket(serverFd);
        WSACleanup();
        return 1;
    }

    // Массив клиентов и fd
    Client clients[MAX_CLIENTS];
    WSAPOLLFD fds[1 + MAX_CLIENTS]; //Массив структур WSAPOLLFD для мониторинга сокетов (1 для серверного сокета + до 100 клиентских).
    ULONG nfds = 1;

    // Инициализация клиентов
    for (int i = 0; i < MAX_CLIENTS; i++) 
        clients[i].fd = INVALID_SOCKET;

    fds[0].fd = serverFd;
    fds[0].events = POLLRDNORM;//чтение данных

    // Цикл сервера
    int running = 1;
    while (running) 
    {
        // Установка флага для проверки входящих данных
        for (ULONG i = 1; i < nfds; i++)
            fds[i].events = POLLRDNORM;

        // Проверка наличия входящих подключений
        int result = WSAPoll(fds, nfds, 1000); //Проверяет сокеты в массиве fds (количество nfds) с таймаутом 1000 мс.
        if (result < 0) 
        {
            perror("Ошибка вызова poll");
            break;
        }

        // Если есть входящие подключения
        if (fds[0].revents & POLLRDNORM) 
        {
            // Адрес клиента 
            struct sockaddr_in clientAddress;
            int len = sizeof(clientAddress);

            // Подключение клиента 
            SOCKET clientFd = accept(serverFd, (struct sockaddr*)&clientAddress, &len);
            if (clientFd != INVALID_SOCKET) 
            {
                // Переключение сокета клиента в неблокирущий режим
                u_long mode = 1;
                ioctlsocket(clientFd, FIONBIO, &mode);

                // Поиск свободного места для клиента и установка в это место
                for (int i = 0; i < MAX_CLIENTS; i++) 
                {
                    if (clients[i].fd == INVALID_SOCKET) 
                    {
                        clients[i].fd = clientFd;
                        clients[i].address = clientAddress;
                        clients[i].buffer = NULL;
                        clients[i].bufferSize = 0;
                        strcpy(clients[i].command, "non");

                        fds[nfds].fd = clientFd;
                        fds[nfds].events = POLLRDNORM;
                        nfds++;

                        printAddress("Подключен клиент: ", &clientAddress);
                        break;
                    }
                }
            }
        }

        // Проверка всех подключенных клиентов на наличие входящих данных
        for (ULONG i = 1; i < nfds; ++i) 
        {
            if (fds[i].revents & POLLRDNORM) 
            {
                // Поиск соответствующего клиента
                Client* client = NULL;
                for (int j = 0; j < MAX_CLIENTS; j++) 
                {
                    if (clients[j].fd == fds[i].fd) 
                    {
                        client = &clients[j];
                        break;
                    }
                }
                if (!client) 
                    continue;

                // Чтение доступных данных из сокета
                char buffer[BUFFER_SIZE];
                int readed = recv(client->fd, buffer, sizeof(buffer), 0);//Читает до BUFFER_SIZE байт в буфер buffer. Возвращает количество прочитанных байт (readed).
                if (readed <= 0)
                {
                    // Клиент отключился
                    if (readed == 0)
                    {
                        printAddress("Отключен клиент: ", &client->address);
                        closesocket(client->fd);
                        free(client->buffer);
                        client->fd = INVALID_SOCKET;
                        for (ULONG j = i + 1; j < nfds; j++)
                            fds[j - 1] = fds[j];
                        nfds--;
                        i--;
                    }
                    continue;
                }

                // Добавление данных в буфер клиента
                client->buffer = (char*)realloc(client->buffer, client->bufferSize + readed);
                memcpy(client->buffer + client->bufferSize, buffer, readed);
                client->bufferSize += readed;

                if (client->bufferSize < 3)
                    continue;

                // Если клиент еще не прислал команду
                if (strcmp(client->command, "non") == 0)
                {
                    if (strncmp(client->buffer, "put", 3) == 0)
                    {
                        client->bufferSize -= 3;
                        memmove(client->buffer, client->buffer + 3, client->bufferSize);
                        strcpy(client->command, "put");    
                    }  
                    else if (strncmp(client->buffer, "get", 3) == 0)
                    {
                        FILE* messageFileRead = fopen("msg.txt", "r");
                        if (messageFileRead) 
                        {
                            char line[BUFFER_SIZE];
                            char buffer[BUFFER_SIZE];
                            uint32_t messageId = 1;
                            while (fgets(line, sizeof(line), messageFileRead)) 
                            {
                                uint16_t year;
                                uint8_t month, day, hour1, hour2,
                                    minute1, minute2, second1, second2;
                                char* message;
                                char ip[64];
                                int port, offset = 0;
                                int ret = sscanf(line, "%63[^:]:%d %hhu.%hhu.%hu %hhu:%hhu:%hhu %hhu:%hhu:%hhu %n",
                                    ip, &port,
                                    &day, &month, &year,
                                    &hour1, &minute1, &second1,
                                    &hour2, &minute2, &second2,
                                    &offset);
    
                                if (ret == 11) 
                                {
                                    message = line + offset;
                                    message[BUFFER_SIZE - offset - 1] = '\0';
                                    message[strcspn(message, "\r\n")] = '\0';
                                    message[strcspn(message, "\n")] = '\0';
    
                                    uint32_t offset = 0; //[ID (4 байта)][дата (4)][время1 (4)][время2 (4)][длина сообщения (4)][сообщение].
                                    uint32_t netIdx = messageId++;
                                    memcpy(buffer + offset, &netIdx, 4); 
                                    offset += 4;
                                    uint32_t dateVal = (uint32_t)year * 10000u + (uint32_t)month * 100u + (uint32_t)day;
                                    uint32_t netDate = htonl(dateVal);
                                    memcpy(buffer + offset, &netDate, 4); 
                                    offset += 4;
                                    uint32_t t1 = (uint32_t)hour1 * 10000u + (uint32_t)minute1 * 100u + (uint32_t)second1;
                                    uint32_t netT1 = htonl(t1);
                                    memcpy(buffer + offset, &netT1, 4); 
                                    offset += 4;
                                    uint32_t t2 = (uint32_t)hour2 * 10000u + (uint32_t)minute2 * 100u + (uint32_t)second2;
                                    uint32_t netT2 = htonl(t2);
                                    memcpy(buffer + offset, &netT2, 4); 
                                    offset += 4;
                                    size_t msgLen = strlen(message);
                                    if (msgLen > (size_t)(BUFFER_SIZE - offset - 4)) 
                                        msgLen = BUFFER_SIZE - offset - 4;

                                    uint32_t netLen = htonl((uint32_t)msgLen);
                                    memcpy(buffer + offset, &netLen, 4); offset += 4;

                                    if (msgLen > 0) 
                                    {
                                        memcpy(buffer + offset, message, msgLen);
                                        offset += (uint32_t)msgLen;
                                    }
    
                                    send(client->fd, buffer, HEADER_SIZE + msgLen, 0);
                                }
                            }
                            fclose(messageFileRead);
                        }
    
                        printAddress("Отключен клиент: ", &client->address);
                        closesocket(client->fd);
                        client->fd = -1;
                        free(client->buffer);
                        client->buffer = NULL;
                        client->bufferSize = 0;
                        for (size_t j = i + 1; j < nfds; j++)
                            fds[j - 1] = fds[j];
                        nfds--;
                        i--;
                    }          
                    else 
                    {
                        // Некорректная команда - отключение клиента
                        printAddress("Некоррктная инициализация: ", &client->address);
                        closesocket(client->fd);
                        client->fd = INVALID_SOCKET;
                        free(client->buffer);
                        client->buffer = NULL;
                        client->bufferSize = 0;
                        for (ULONG j = i + 1; j < nfds; j++)
                            fds[j - 1] = fds[j];
                        nfds--;
                        i--;
                        continue;
                    }
                }

                if (strcmp(client->command, "put") == 0)
                {
                    // Пока буфер содержит достаточное количество байт для чтения сообщения
                    while (client->bufferSize >= HEADER_SIZE) 
                    {
                        uint32_t msgId, date, time1, time2, length;
                        uint16_t year;
                        uint8_t month, day, hour1, hour2,
                            minute1, minute2, second1, second2;
                        char* message;
                        //Извлекает заголовок из буфера клиента
                        memcpy(&msgId, client->buffer, 4);
                        msgId = ntohl(msgId);

                        memcpy(&date, client->buffer + 4, 4);
                        date = ntohl(date);

                        memcpy(&time1, client->buffer + 8, 4);
                        time1 = ntohl(time1);
                        
                        memcpy(&time2, client->buffer + 12, 4);
                        time2 = ntohl(time2);

                        memcpy(&length, client->buffer + 16, 4);
                        length = ntohl(length);

                        if (client->bufferSize < HEADER_SIZE + length)
                            break;

                        message = client->buffer + HEADER_SIZE;
                        message[length] = 0;

                        year = date / 10000;
                        month = date / 100 % 100;
                        day = date % 100;
                        hour1 = time1 / 10000;
                        minute1 = time1 / 100 % 100;
                        second1 = time1 % 100;
                        hour2 = time2 / 10000;
                        minute2 = time2 / 100 % 100;
                        second2 = time2 % 100;

                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &client->address.sin_addr, ip, sizeof(ip));

                        fprintf(messageFile, "%s:%d %02d.%02d.%d %02d:%02d:%02d %02d:%02d:%02d %s\n",
                                ip, ntohs(client->address.sin_port),
                                day, month, year, hour1, minute1, second1, 
                                hour2, minute2, second2, message);
                        fflush(messageFile);
                        
                        send(client->fd, "ok", 2, 0);

                        if (strcmp(message, "stop") == 0)
                        {
                            printf("Получено сообщение 'stop'. Завершение работы...\n");
                            running = 0;

                            // Закрываем все соединения и выходим
                            for (int k = 0; k < MAX_CLIENTS; k++) {
                                if (clients[k].fd != INVALID_SOCKET) {
                                    closesocket(clients[k].fd);
                                    clients[k].fd = INVALID_SOCKET;
                                }
                            }
                            break;
                            break;
                        }


                        client->bufferSize -= HEADER_SIZE + length;
                        memmove(client->buffer, client->buffer + HEADER_SIZE + length, client->bufferSize);
                    }
                }
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != INVALID_SOCKET) 
        {
            closesocket(clients[i].fd);
            printAddress("Отключен клиент: ", &clients[i].address);
        }
        free(clients[i].buffer);
    }

    closesocket(serverFd);
    fclose(messageFile);
    WSACleanup();
    return 0;
}

void printAddress(const char* prefix, struct sockaddr_in* addr) 
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
    printf("%s%s:%d\n", prefix, ip, ntohs(addr->sin_port));
}
