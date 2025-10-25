#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <locale.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#define BUFFER_LENGTH 8192
#define MAX_MESSAGES 8192 

// Структура сообщения
struct Message {
    uint8_t d;
    uint8_t m;
    uint16_t y;
    uint8_t h1;
    uint8_t m1;
    uint8_t s1;
    uint8_t h2;
    uint8_t m2;
    uint8_t s2;
    char message[BUFFER_LENGTH];
};

int readMessageLine(const char* line, struct Message* msg);
int formatMessage(const struct Message* msg, uint32_t msgId, char* outBuf);

int main(int argc, char* argv[])
{
    setlocale(LC_ALL, "ru");

    if (argc != 3) 
    {
        fprintf(stderr, "Формат: %s IP:port file.txt\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Разбор адреса:порт
    char* colon = strchr(argv[1], ':');
    if (!colon)
    {
        fprintf(stderr, "Некорректный формат адреса (IP:port)\n");
        return EXIT_FAILURE;
    }

    char ip[64];
    size_t iplen = (size_t)(colon - argv[1]);
    if (iplen >= sizeof(ip))
    {
        fprintf(stderr, "IP адрес слишком длинный\n");
        return EXIT_FAILURE;
    }

    strncpy(ip, argv[1], iplen);
    ip[iplen] = '\0';
    int port = atoi(colon + 1);
    const char* filename = argv[2];

    // Инициализация WinSock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
    {
        fprintf(stderr, "Ошибка WSAStartup\n");
        return EXIT_FAILURE;
    }

    // Заполнение sockaddr_in
    struct sockaddr_in serverAddr;
    memset(&serverAddr,0,sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &serverAddr.sin_addr) <= 0)
    {
        fprintf(stderr, "Некорректный IP адрес\n");
        WSACleanup();
        return EXIT_FAILURE;
    }

    // Создание UDP-сокета
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        fprintf(stderr, "Ошибка создания сокета: %d\n", WSAGetLastError());
        WSACleanup();
        return EXIT_FAILURE;
    }

    printf("UDP-клиент запущен. Сервер: %s:%d\n", ip, port);

    // Массив подтверждений
    int confirmed[MAX_MESSAGES] = {0};
    uint32_t totalMsgs = 0; // количество непустых сообщений в файле

    FILE* f = fopen(filename,"r");
    if (!f)
    {
        fprintf(stderr, "Ошибка при открытии файла '%s'\n", filename);
        closesocket(sock);
        WSACleanup();
        return EXIT_FAILURE;
    }

    char line[BUFFER_LENGTH];
    while (fgets(line,sizeof(line),f) && totalMsgs < MAX_MESSAGES)
    {
        line[strcspn(line,"\r\n")] = '\0';
        if (line[0] != '\0') 
            totalMsgs++;
    }
    fclose(f);

    if (totalMsgs == 0)
    {
        printf("Нет сообщений для отправки\n");
        closesocket(sock);
        WSACleanup();
        return EXIT_SUCCESS;
    }

    printf("Найдено сообщений: %u\n", totalMsgs);

    uint32_t confirmedCount = 0;

    // Основной цикл отправки/приёма подтверждений
    while (confirmedCount < ((totalMsgs < 20) ? totalMsgs : 20))
    {
        // Отправка всех неподтверждённых сообщений
        f = fopen(filename,"r");
        if (!f)
        {
            fprintf(stderr,"Ошибка открытия файла при повторной отправке\n");
            break;
        }

        uint32_t msgNum = 0; // номер непустой строки
        while (fgets(line,sizeof(line),f) && msgNum < totalMsgs)
        {
            line[strcspn(line,"\r\n")] = '\0';
            if (line[0] == '\0') continue;

            if (!confirmed[msgNum])
            {
                struct Message msg;
                if (!readMessageLine(line,&msg))
                {
                    fprintf(stderr,"Пропущена некорректная строка: %s\n",line);
                    confirmed[msgNum] = 1; // не отправляем повторно
                    msgNum++;
                    continue;
                }

                char outBuf[BUFFER_LENGTH];
                int outLen = formatMessage(&msg,msgNum,outBuf);
                if (outLen > 0) {
                    int sent = sendto(sock,outBuf,outLen,0,
                                      (struct sockaddr*)&serverAddr,sizeof(serverAddr));
                    if (sent == SOCKET_ERROR) {
                        fprintf(stderr,"Ошибка sendto: %d\n",WSAGetLastError());
                    }
                }
            }
            msgNum++;
        }
        fclose(f);

        // Ждём ответ сервера (100 мс)
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock,&readfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100 мс
        int rv = select(0,&readfds,NULL,NULL,&tv);
        if (rv > 0 && FD_ISSET(sock,&readfds)) {
            char recvBuf[BUFFER_LENGTH];
            int addrLen = sizeof(serverAddr);
            int r = recvfrom(sock,recvBuf,sizeof(recvBuf),0,
                             (struct sockaddr*)&serverAddr,&addrLen);
            if (r > 0)
            {
                // Разбор датаграммы: 4 байта - номер сообщения
                for (int i=0; i+4 <= r; i+=4) {
                    uint32_t num;
                    memcpy(&num,recvBuf+i,4);
                    num = ntohl(num);
                    if (num < totalMsgs && !confirmed[num])
                    {
                        confirmed[num] = 1;
                        confirmedCount++;
                        printf("Подтверждено сообщение %u\n",num);
                    }
                }
            }
        }
    }

    printf("Все (%u) сообщения подтверждены сервером\n",
           (totalMsgs < 20) ? totalMsgs : 20);

    closesocket(sock);
    WSACleanup();
    return EXIT_SUCCESS;
}

// Чтение строки в формате dd.mm.yyyy hh:mm:ss hh:mm:ss Message
int readMessageLine(const char* line, struct Message* msg)
{
    msg->message[0]='\0';
    int d,mo,y,h1,m1,s1,h2,m2,s2;
    char msgText[BUFFER_LENGTH];
    int parsed = sscanf(line,"%d.%d.%d %d:%d:%d %d:%d:%d %[^\n]",
                        &d,&mo,&y,&h1,&m1,&s1,&h2,&m2,&s2,msgText);
    if (parsed!=10) 
        return 0;
    msg->d = (uint8_t)d; 
    msg->m = (uint8_t)mo; 
    msg->y=(uint16_t)y;
    msg->h1=(uint8_t)h1; 
    msg->m1=(uint8_t)m1; 
    msg->s1=(uint8_t)s1;
    msg->h2=(uint8_t)h2; 
    msg->m2=(uint8_t)m2; 
    msg->s2=(uint8_t)s2;
    strncpy(msg->message,msgText,BUFFER_LENGTH-1);
    msg->message[BUFFER_LENGTH-1]='\0';
    return 1;
}

// Форматирование сообщения в сетевой порядок
int formatMessage(const struct Message* msg, uint32_t msgId, char* outBuf)
{
    uint32_t offset=0;
    uint32_t netIdx = htonl(msgId);
    memcpy(outBuf+offset,&netIdx,4); 
    offset+=4;
    uint32_t dateVal = (uint32_t)msg->y*10000u + 
        (uint32_t)msg->m*100u + (uint32_t)msg->d;
    uint32_t netDate = htonl(dateVal);
    memcpy(outBuf+offset,&netDate,4); 
    offset+=4;
    uint32_t t1 = (uint32_t)msg->h1*10000u + 
        (uint32_t)msg->m1*100u + (uint32_t)msg->s1;
    uint32_t netT1 = htonl(t1);
    memcpy(outBuf+offset,&netT1,4); 
    offset+=4;
    uint32_t t2 = (uint32_t)msg->h2*10000u +
        (uint32_t)msg->m2*100u + (uint32_t)msg->s2;
    uint32_t netT2 = htonl(t2);
    memcpy(outBuf+offset,&netT2,4);
    offset+=4;
    size_t msgLen = strlen(msg->message);
    uint32_t netLen = htonl((uint32_t)msgLen);
    memcpy(outBuf+offset,&netLen,4); 
    offset+=4;
    if (msgLen>0)
    {
        memcpy(outBuf+offset,msg->message,msgLen);
        offset+=(uint32_t)msgLen;
    }
    return (int)offset;
}
