#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdlib>
#include "processpool.h"

const char *serverInfo220 = "220 FTP Server ready...\r\n";
const char *serverInfo331 = "331 User name okay, need password.\r\n";
const char *serverInfo230 = "230 User logged in, proceed.\r\n";
const char *serverInfo150 = "150 File status okay; about to open data connection.\r\n";

class ftp_conn {
public:
    enum FTP_CODE {
        PWD, QUIT, LS, CD, GET, PUT
    };

public:
    ftp_conn() = default;

    ~ftp_conn() = default;

    /* 初始化客户连接，清空读缓冲区，发送连接成功响应码 */
    void init(int epollfd, int sockfd, const sockaddr_in &client_addr) {
        m_epollfd = epollfd;
        comm_sockfd = sockfd;
        m_address = client_addr;
        memset(m_buf, '\0', BUFFER_SIZE);
        m_read_idx = 0;
        ssize_t ret = send(comm_sockfd, serverInfo220, strlen(serverInfo220), 0);
        assert(ret >= 0);
    }

    void process() {
        int idx = 0;
        ssize_t ret = -1;
        while (true) {
            idx = m_read_idx;
            ret = recv(comm_sockfd, m_buf + idx, BUFFER_SIZE - 1 - idx, 0);
            if (ret < 0) {
                if (errno != EAGAIN) {
                    removefd(m_epollfd, comm_sockfd);
                }
                break;
            } else if (ret == 0) {
                removefd(m_epollfd, comm_sockfd);
                break;
            } else {
                m_read_idx += (int) ret;

                for (; idx < m_read_idx; ++idx) {
                    if ((idx >= 1) && (m_buf[idx - 1] == '\r') && (m_buf[idx] == '\n')) {
                        break;
                    }
                }
                if (idx == m_read_idx) {
                    continue;
                }
                m_buf[idx - 1] = '\0';
                printf("user content is: %s\n", m_buf);

                if (login_flag) {
                    if (strncmp("PASV", m_buf, 4) == 0) {
                        handle_pasv();
                    }
                    if (strncmp("LIST", m_buf, 4) == 0) {
                        handle_list();
                    }
                } else {
                    if (strncmp("USER", m_buf, 4) == 0) {
                        verify_username();
                    }
                    if (strncmp("PASS", m_buf, 4) == 0) {
                        verify_password();
                    }
                }
            }
        }
        memset(m_buf, '\0', BUFFER_SIZE);
        m_read_idx = 0;
    }

    bool verify_username() {
        char *username = new char[20];
        int i;
        for (i = 5; i < strlen(m_buf); i++) {
            username[i - 5] = m_buf[i];
        }
        username[i - 5] = '\0';
        if (strncmp(username, "root", 4) == 0) {
            ssize_t ret = send(comm_sockfd, serverInfo331, strlen(serverInfo331), 0);
            assert(ret >= 0);
            delete[] username;
        }
    }

    bool verify_password() {
        char *password = new char[20];
        int i;
        for (i = 5; i < strlen(m_buf); i++) {
            password[i - 5] = m_buf[i];
        }
        password[i - 5] = '\0';
        if (strncmp(password, "root", 4) == 0) {
            login_flag = true;
            ssize_t ret = send(comm_sockfd, serverInfo230, strlen(serverInfo230), 0);
            assert(ret >= 0);
            delete[] password;
        }
    }

    void handle_pasv() {
        data_sockfd = socket(AF_INET, SOCK_STREAM, 0);
        assert(data_sockfd >= 0);

        struct sockaddr_in data_addr{};
        int port = (int) random() % 1000 + 2000;

        data_addr.sin_family = AF_INET;
        data_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        data_addr.sin_port = htons(port);

        ssize_t ret = bind(data_sockfd, (struct sockaddr *) &data_addr, sizeof(struct sockaddr));
        assert(ret != -1);

        ret = listen(data_sockfd, 5);
        assert(ret != -1);

        int tmp_port1 = ntohs(data_addr.sin_port) / 256;
        int tmp_port2 = ntohs(data_addr.sin_port) % 256;
        size_t ipNum = inet_addr(inet_ntoa(m_address.sin_addr));

        char addr_info_str[20];
        char port_str[8];
        char pasv_msg[100];
        snprintf(addr_info_str, sizeof(addr_info_str), "%ld,%ld,%ld,%ld,", ipNum & 0xff, ipNum >> 8 & 0xff,
                 ipNum >> 16 & 0xff, ipNum >> 24 & 0xff);
        snprintf(port_str, sizeof(port_str), "%d,%d", tmp_port1, tmp_port2);
        strcat(addr_info_str, port_str);
        snprintf(pasv_msg, sizeof(pasv_msg), "227 Entering Passive Mode (%s).\r\n", addr_info_str);
        ret = send(comm_sockfd, pasv_msg, strlen(pasv_msg), 0);
        assert(ret >= 0);
    }

    void handle_list() {
        send(comm_sockfd, serverInfo150, strlen(serverInfo150), 0);
        struct sockaddr_in client{};
        socklen_t client_addrlength = sizeof(client);
        int data_connfd = accept(data_sockfd, (struct sockaddr *) &client, &client_addrlength);
        assert(data_connfd > 0);

        FILE *pipe_fp;
        char t_dir[100];
        char list_cmd_info[100];
        snprintf(list_cmd_info, 100, "ls -l %s", getcwd(t_dir, 100));

        pipe_fp = popen(list_cmd_info, "r");

        char t_char;
        while((t_char = (char) fgetc(pipe_fp)) != EOF){
            write(data_connfd, &t_char, 1);
        }
        pclose(pipe_fp);
        close(data_connfd);
        close(data_sockfd);
    }

    void handle_file(){

    }


private:
    static const int BUFFER_SIZE = 1024;
    static int m_epollfd;
    /* 命令套接字，用于接受ftp命令 */
    int comm_sockfd{};
    /* 数据监听套接字 */
    int data_sockfd{};
    sockaddr_in m_address{};
    char m_buf[BUFFER_SIZE]{};
    int m_read_idx{};

    bool login_flag = false;
};

int ftp_conn::m_epollfd = -1;

int main(int argc, char *argv[]) {
    if (argc <= 2) {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address{};
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr *) &address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    processpool<ftp_conn> *pool = processpool<ftp_conn>::create(listenfd);
    if (pool) {
        pool->run();
        delete pool;
    }
    close(listenfd);
    return 0;
}
