#include <pthread.h>
#include <netinet/in.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <cerrno>
#include <arpa/inet.h>

#include "ftp.h"

#define FTP_SERVER_PORT 21
#define MAX_INFO 1024
#define DIR_INFO 100
#define MSG_INFO 100
#define LISTEN_QUEUE 5

int ftp_server_sock;
int ftp_data_sock;
char client_Control_Info[MAX_INFO];
char client_Data_Info[MAX_INFO];
char format_client_Info[MAX_INFO];

struct ARG {
    int client_sock;
    struct sockaddr_in client;
};


void *Handle_Client_Request(void *arg);

void do_client_work(int client_sock, struct sockaddr_in client);

int login(int client_sock);

void handle_cwd(int client_sock);

void handle_list(int client_sock);

void handle_pasv(int client_sock, struct sockaddr_in client);

void handle_file(int client_sock);

struct sockaddr_in create_data_sock();

void send_client_info(int client_sock, char *info, size_t length);

int recv_client_info(int client_sock);

int main() {
    pthread_t thread;
    struct ARG arg{};
    struct sockaddr_in server{};

    if ((ftp_server_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Create socket failed");
        exit(1);
    }

    int opt = SO_REUSEADDR;
    setsockopt(ftp_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bzero(&server, sizeof(server));

    server.sin_family = AF_INET;
    server.sin_port = htons(FTP_SERVER_PORT);
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ftp_server_sock, (struct sockaddr *) &server, sizeof(struct sockaddr)) == -1) {
        perror("Bind error");
        exit(1);
    }

    if (listen(ftp_server_sock, LISTEN_QUEUE) == -1) {
        perror("listen error");
        exit(1);
    }

    int ftp_client_sock;
    struct sockaddr_in client{};

    socklen_t clientlength = sizeof(struct sockaddr_in);

    while (true) {

        if ((ftp_client_sock = accept(ftp_server_sock, (struct sockaddr *) &client, &clientlength)) == -1) {
            perror("accept error");
            exit(1);
        }

        arg.client_sock = ftp_client_sock;
        memcpy((void *) &arg.client, &client, sizeof(client));
        if (pthread_create(&thread, nullptr, Handle_Client_Request, (void *) &arg)) {
            perror("thread create error");
            exit(1);
        }
    }
}

void *Handle_Client_Request(void *arg) {
    struct ARG *info;
    info = (struct ARG *) arg;
    printf("You got a connection from %s\n", inet_ntoa(info->client.sin_addr));
    do_client_work(info->client_sock, info->client);
    close(info->client_sock);
    pthread_exit(nullptr);
}

void do_client_work(int client_sock, struct sockaddr_in client) {

    int login_flag = login(client_sock);

    while (recv_client_info(client_sock) && login_flag == 1) {

        if (strncmp("quit", client_Control_Info, 4) == 0 || (strncmp("QUIT", client_Control_Info, 4) == 0)) {
            send_client_info(client_sock, serverInfo221, strlen(serverInfo221));
            break;

        } else if (strncmp("pwd", client_Control_Info, 3) == 0 || (strncmp("PWD", client_Control_Info, 3) == 0)) {
            char pwd_info[MSG_INFO];
            char tmp_dir[DIR_INFO];
            snprintf(pwd_info, MSG_INFO, "257 \"%s\" is current location.\r\n", getcwd(tmp_dir, DIR_INFO));
            send_client_info(client_sock, pwd_info, strlen(pwd_info));

        } else if (strncmp("cwd", client_Control_Info, 3) == 0 || (strncmp("CWD", client_Control_Info, 3) == 0)) {
            handle_cwd(client_sock);

        } else if (strncmp("pasv", client_Control_Info, 4) == 0 || (strncmp("PASV", client_Control_Info, 4) == 0)) {
            handle_pasv(client_sock, client);

        } else if (strncmp("list", client_Control_Info, 4) == 0 || (strncmp("LIST", client_Control_Info, 4) == 0)) {
            handle_list(client_sock);
            send_client_info(client_sock, serverInfo226, strlen(serverInfo226));

        } else if (strncmp("put", client_Control_Info, 3) == 0 ||
                   strncmp("PUT", client_Control_Info, 3) == 0 ||
                   strncmp("get", client_Control_Info, 3) == 0 ||
                   strncmp("GET", client_Control_Info, 3) == 0) {
            handle_file(client_sock);
            send_client_info(client_sock, serverInfo226, strlen(serverInfo226));
        }
    }
}

/**
 * 用户登陆
 * @param client_sock 连接套接字
 * @return
 */
int login(int client_sock) {

    send_client_info(client_sock, serverInfo220, strlen(serverInfo220));

    /* 验证用户名 */
    while (true) {
        recv_client_info(client_sock);
        size_t length = strlen(client_Control_Info);
        int i;
        for (i = 5; i < length; i++) {
            format_client_Info[i - 5] = client_Control_Info[i];
        }
        format_client_Info[i - 5] = '\0';

        if (strncmp(format_client_Info, "root", 4) == 0) {
            send_client_info(client_sock, serverInfo331, strlen(serverInfo331));
            break;
        } else send_client_info(client_sock, serverInfo530, strlen(serverInfo));
    }

    /* 验证密码 */
    while (true) {
        recv_client_info(client_sock);
        size_t length = strlen(client_Control_Info);
        int i;
        for (i = 5; i < length; i++) {
            format_client_Info[i - 5] = client_Control_Info[i];
        }
        format_client_Info[i - 5] = '\0';

        if (strncmp(format_client_Info, "1234", 4) == 0) {
            send_client_info(client_sock, serverInfo230, strlen(serverInfo230));
            break;
        } else {
            send_client_info(client_sock, serverInfo530, strlen(serverInfo));
        }
    }
}

/**
 * 处理cd指令
 * @param client_sock
 */
void handle_cwd(int client_sock) {
    char cwd_info[MSG_INFO];
    char tmp_dir[DIR_INFO];
    char client_dir[DIR_INFO];

    char t_dir[DIR_INFO];

    size_t dirlength;
    size_t length = strlen(client_Control_Info);
    int i;
    for (i = 4; i < length; i++) {
        format_client_Info[i - 4] = client_Control_Info[i];
    }
    format_client_Info[i - 4] = '\0';

    if (strncmp(getcwd(t_dir, DIR_INFO), format_client_Info, strlen(getcwd(t_dir, DIR_INFO))) != 0) {
        getcwd(client_dir, DIR_INFO);
        dirlength = strlen(client_dir);
    }

    for (i = 4; i < length; i++) {
        client_dir[dirlength + i - 4] = client_Control_Info[i];
    }
    client_dir[dirlength + i - 4] = '\0';
    if (chdir(client_dir) >= 0) {
        snprintf(cwd_info, MSG_INFO, "257 \"%s\" is current location.\r\n", getcwd(tmp_dir, DIR_INFO));
    } else {
        snprintf(cwd_info, MSG_INFO, "550 %s :%s\r\n", client_dir, strerror(errno));
        perror("chdir():");
        send_client_info(client_sock, cwd_info, strlen(cwd_info));
    }
}

void handle_list(int client_sock) {
    send_client_info(client_sock, serverInfo150, strlen(serverInfo150));

    int t_data_sock;
    struct sockaddr_in client{};
    socklen_t sin_size = sizeof(struct sockaddr_in);
    if ((t_data_sock = accept(ftp_data_sock, (struct sockaddr *) &client, &sin_size)) == 1) {
        perror("accept error");
        return;
    }

    FILE *pipe_fp;
    char t_dir[DIR_INFO];
    char list_cmd_info[DIR_INFO];
    snprintf(list_cmd_info, DIR_INFO, "ls -l %s", getcwd(t_dir, DIR_INFO));

    if ((pipe_fp = popen(list_cmd_info, "r")) == nullptr) {
        printf("pipe open error in cmd_list\n");
        return;
    }
    char t_char;
    while ((t_char = (char) fgetc(pipe_fp)) != EOF) {
        write(t_data_sock, &t_char, 1);
    }
    pclose(pipe_fp);
    close(t_data_sock);
    close(ftp_data_sock);
}

void handle_pasv(int client_sock, struct sockaddr_in client) {
    char pasv_msg[MSG_INFO];
    char port_str[8];
    char addr_info_str[30];
    struct sockaddr_in user_data_addr = create_data_sock();

    int tmp_port1 = ntohs(user_data_addr.sin_port) / 256;
    int tmp_port2 = ntohs(user_data_addr.sin_port) % 256;
    long ipNum = inet_addr(inet_ntoa(client.sin_addr));

    snprintf(addr_info_str, sizeof(addr_info_str), "%ld,%ld,%ld,%ld,", ipNum & 0xff, ipNum >> 8 & 0xff,
             ipNum >> 16 & 0xff, ipNum >> 24 & 0xff);
    snprintf(port_str, sizeof(port_str), "%d,%d", tmp_port1, tmp_port2);
    strcat(addr_info_str, port_str);
    snprintf(pasv_msg, MSG_INFO, "227 Entering Passive Mode (%s).\r\n", addr_info_str);
    send_client_info(client_sock, pasv_msg, strlen(pasv_msg));
}

void handle_file(int client_sock) {

    send_client_info(client_sock, serverInfo150, strlen(serverInfo150));
    int t_data_sock;
    struct sockaddr_in client{};
    socklen_t sin_size = sizeof(struct sockaddr_in);
    if ((t_data_sock = accept(ftp_data_sock, (struct sockaddr *) &client, &sin_size)) == -1) {
        perror("accept error");
        return;
    }
    int i;
    size_t length = strlen(client_Control_Info);
    for (i = 4; i < length; i++)
        format_client_Info[i - 4] = client_Control_Info[i];
    format_client_Info[i - 4] = '\0';

    FILE *fp;
    ssize_t n;
    char t_dir[DIR_INFO];

    char file_info[DIR_INFO];

    snprintf(file_info, DIR_INFO, "%s/%s", getcwd(t_dir, DIR_INFO), format_client_Info);

    char file_mode[3];
    if (strncmp("get", client_Control_Info, 4) == 0 || (strncmp("GET", client_Control_Info, 4) == 0)) {
        file_mode[0] = 'r';
        file_mode[1] = 'b';
        file_mode[2] = '\0';
    } else {
        file_mode[0] = 'a';
        file_mode[1] = 'b';
        file_mode[2] = '\0';
    }

    if (strncmp(getcwd(t_dir, DIR_INFO), format_client_Info, strlen(getcwd(t_dir, DIR_INFO))) == 0)
        fp = fopen(format_client_Info, file_mode);
    else
        fp = fopen(file_info, file_mode);

    if (fp == nullptr) {
        printf("open file error:%s\r\n", strerror(errno));
        char cwd_info[MSG_INFO];
        snprintf(cwd_info, MSG_INFO, "550 %s :%s\r\n", format_client_Info, strerror(errno));
        send_client_info(client_sock, cwd_info, strlen(cwd_info));
        close(t_data_sock);
        close(ftp_data_sock);
        return;
    }
    int cmd_sock = fileno(fp);
    memset(client_Data_Info, 0, MAX_INFO);
    if (strncmp("get", client_Control_Info, 4) == 0 || (strncmp("GET", client_Control_Info, 4) == 0)) {
        while ((n = read(cmd_sock, client_Data_Info, MAX_INFO)) > 0) {
            if (write(t_data_sock, client_Data_Info, n) != n) {
                printf("retr transfer error\n");
                return;
            }
        }
    } else {
        while ((n = read(t_data_sock, client_Data_Info, MAX_INFO)) > 0) {
            if (write(cmd_sock, client_Data_Info, n) != n) {
                printf("stor transfer error\n");
                return;
            }
        }
    }

    fclose(fp);
    close(t_data_sock);
    close(ftp_data_sock);
}

struct sockaddr_in create_data_sock() {

    int t_client_sock;
    struct sockaddr_in t_data_addr{};
    t_client_sock = socket(AF_INET, SOCK_STREAM, 0);

    if (t_client_sock < 0) {
        printf("create data socket error!\n");
        return {};
    }

    long a = random() % 1000 + 1025;
    bzero(&t_data_addr, sizeof(t_data_addr));
    t_data_addr.sin_family = AF_INET;
    t_data_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    t_data_addr.sin_port = htons(a);
    printf("%ld\n", a);
    if (bind(t_client_sock, (struct sockaddr *) &t_data_addr, sizeof(struct sockaddr)) < 0) {
        printf("bind error in create data socket:%s\n", strerror(errno));
        return {};
    }

    listen(t_client_sock, LISTEN_QUEUE);
    ftp_data_sock = t_client_sock;
    return t_data_addr;
}

void send_client_info(int client_sock, char *info, size_t length) {
    if (send(client_sock, info, length, 0) < 0) {
        perror("send info error");
        return;
    }
}

int recv_client_info(int client_sock) {
    memset(client_Control_Info, '\0', MAX_INFO);
    ssize_t num;
    if ((num = recv(client_sock, client_Control_Info, MAX_INFO, 0)) < 0) {
        perror("receive info error");
        return 0;
    }
    return 1;
}

