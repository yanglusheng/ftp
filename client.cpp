#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <dirent.h>
#include <termios.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>

#define DEFAULT_FTP_PORT 21

struct sockaddr_in ftp_server, local_host;
struct hostent *server_hostent;
int mode = 1;
int sock_control;
char user[64]; //ftp usr
char passwd[64]; //ftp passwd

static struct termios stored_settings;

void echo_off() {
    struct termios new_settings{};
    tcgetattr(0, &stored_settings);
    new_settings = stored_settings;
    new_settings.c_lflag &= (~ECHO);
    tcsetattr(0, TCSANOW, &new_settings);
}

void echo_on() {
    tcsetattr(0, TCSANOW, &stored_settings);
}

void cmd_err_exit(const char *err_msg, int err_code) {
    printf("%s\n", err_msg);
    exit(err_code);
}

/**
 * 填充主机地址
 * @param host_ip_addr ip地址
 * @param host
 * @param port 端口号
 * @return
 */
int fill_host_addr(const char *host_ip_addr, struct sockaddr_in *host, long int port) {
    if (port <= 0 || port > 65535)
        return 254;
    bzero(host, sizeof(struct sockaddr_in));
    host->sin_family = AF_INET;
    if (inet_addr(host_ip_addr) != -1) {
        host->sin_addr.s_addr = inet_addr(host_ip_addr);
    } else {
        if ((server_hostent = gethostbyname(host_ip_addr)) != nullptr) {
            memcpy(&host->sin_addr, server_hostent->h_addr,
                   sizeof(host->sin_addr));
        } else return 253;
    }
    host->sin_port = htons(port);
    return 1;
}

/**
 * 连接ftp server
 * @param s_addr ip地址
 * @param type
 * @return
 */
int xconnect(struct sockaddr_in *s_addr, int type) {
    struct timeval outtime{};
    int set;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        cmd_err_exit("creat socket error!", 249);

    if (type == 1) {
        outtime.tv_sec = 0;
        outtime.tv_usec = 300000;
    } else {
        outtime.tv_sec = 5;
        outtime.tv_usec = 0;
    }
    set = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &outtime, sizeof(outtime));
    if (set != 0) {
        printf("set socket %s errno:%d\n", strerror(errno), errno);
        cmd_err_exit("set socket", 1);
    }

    //connect to the server
    if (connect(s, (struct sockaddr *) s_addr, sizeof(struct sockaddr_in)) < 0) {
        printf("Can't connect to server %s, port %d\n",
               inet_ntoa(s_addr->sin_addr), ntohs(ftp_server.sin_port));
        exit(252);
    }
    return s;
}

/**
 * 发送ftp命令
 * @param s1
 * @param s2
 * @param sock_fd
 * @return
 */
ssize_t ftp_send_cmd(const char *s1, const char *s2, int sock_fd) {
    char send_buf[256];
    ssize_t send_err;
    size_t len;

    if (s1) {
        strcpy(send_buf, s1);

        if (s2) {
            strcat(send_buf, s2);
            strcat(send_buf, "\r\n");
            len = strlen(send_buf);
            send_err = send(sock_fd, send_buf, len, 0);
        } else {
            strcat(send_buf, "\r\n");
            len = strlen(send_buf);
            send_err = send(sock_fd, send_buf, len, 0);
        }
    }


    if (send_err < 0)
        printf("send() error!\n");
    return send_err;
}

long int ftp_get_reply(int sock_fd) {
    long int reply_code;
    ssize_t count;
    char rcv_buf[512];
    char *endptr;
    count = read(sock_fd, rcv_buf, 510);
    if (count > 0)
        reply_code = strtol(rcv_buf, &endptr, 10);
    else
        return 0;
    while (true) {
        if (count <= 0)
            break;
        rcv_buf[count] = '\0';
        printf("%s", rcv_buf);
        count = read(sock_fd, rcv_buf, 510);
    }
    return reply_code;
}

/**
 * 获取ftp server data port
 * @return
 */
long int get_port() {
    char port_respond[512];
    char *buf_ptr;
    ssize_t count;
    long int port_num;
    ftp_send_cmd("PASV", nullptr, sock_control);
    count = read(sock_control, port_respond, 510);
    if (count <= 0)
        return 0;
    port_respond[count] = '\0';
    char *endptr;
    if (strtol(port_respond, &endptr, 10) == 227) {
        //get low byte of the port
        buf_ptr = strrchr(port_respond, ',');
        port_num = strtol(buf_ptr + 1, &endptr, 10);;
        *buf_ptr = '\0';
        //get high byte of the port
        buf_ptr = strrchr(port_respond, ',');
        port_num += strtol(buf_ptr + 1, &endptr, 10) * 256;
        return port_num;
    }
    return 0;
}

//connect data stream
int xconnect_ftpdata() {
    long int data_port = get_port();
    if (data_port != 0)
        ftp_server.sin_port = htons(data_port);
    return (xconnect(&ftp_server, 0));
}


void ftp_list() {
    int list_sock_data = xconnect_ftpdata();
    if (list_sock_data < 0) {
        ftp_get_reply(sock_control);
        printf("creat data sock error!\n");
        return;
    }
    ftp_get_reply(sock_control);
    ftp_send_cmd("LIST", nullptr, sock_control);
    ftp_get_reply(sock_control);

    ftp_get_reply(list_sock_data);

    close(list_sock_data);
    ftp_get_reply(sock_control);
}

//get filename(s) from user's command
void ftp_cmd_filename(char *usr_cmd, char *src_file, char *dst_file) {
    size_t length;
    int flag = 0;
    int i = 0, j = 0;
    char *cmd_src;
    cmd_src = strchr(usr_cmd, ' ');
    if (cmd_src == nullptr) {
        printf("command error!\n");
        return;
    } else {
        while (*cmd_src == ' ')
            cmd_src++;
    }
    if (*cmd_src == '\0') {
        printf("command error!\n");
    } else {
        length = strlen(cmd_src);
        while (i <= length)//be careful with space in the filename
        {
            if ((*(cmd_src + i)) != ' ' && (*(cmd_src + i)) != '\\') {
                if (flag == 0)
                    src_file[j] = *(cmd_src + i);
                else
                    dst_file[j] = *(cmd_src + i);
                j++;
            }
            if ((*(cmd_src + i)) == '\\' && (*(cmd_src + i + 1)) != ' ') {
                if (flag == 0)
                    src_file[j] = *(cmd_src + i);
                else
                    dst_file[j] = *(cmd_src + i);
                j++;
            }
            if ((*(cmd_src + i)) == ' ' && (*(cmd_src + i - 1)) != '\\') {
                src_file[j] = '\0';
                flag = 1;
                j = 0;
            }
            if ((*(cmd_src + i)) == '\\' && (*(cmd_src + i + 1)) == ' ') {
                if (flag == 0)
                    src_file[j] = ' ';
                else
                    dst_file[j] = ' ';
                j++;
            }
            i++;
        };
    }
    if (flag == 0)
        strcpy(dst_file, src_file);
    else
        dst_file[j] = '\0';
}

//deal with the "get" command
void ftp_get(char *usr_cmd) {
    int get_sock, set, new_sock, i = 0;
    char src_file[512];
    char dst_file[512];
    char rcv_buf[512];
    char cover_flag[3];
    struct stat file_info{};
    int local_file;
    ssize_t count;
    ftp_cmd_filename(usr_cmd, src_file, dst_file);
    ftp_send_cmd("SIZE ", src_file, sock_control);
    if (ftp_get_reply(sock_control) != 213) {
        printf("SIZE error!\n");
        return;
    }
    if (!stat(dst_file, &file_info)) {
        printf("local file %s exists: %d bytes\n", dst_file, (int) file_info.st_size);
        printf("Do you want to cover it? [y/n]");
        fgets(cover_flag, sizeof(cover_flag), stdin);
        fflush(stdin);
        if (cover_flag[0] != 'y') {
            printf("get file %s aborted.\n", src_file);
            return;
        }
    }
    local_file = open(dst_file, O_CREAT | O_TRUNC | O_WRONLY);
    if (local_file < 0) {
        printf("creat local file %s error!\n", dst_file);
        return;
    }
    get_sock = xconnect_ftpdata();
    if (get_sock < 0) {
        printf("socket error!\n");
        return;
    }
    set = sizeof(local_host);

    ftp_send_cmd("TYPE I", nullptr, sock_control);
    ftp_get_reply(sock_control);
    ftp_send_cmd("RETR ", src_file, sock_control);
    if (!mode) {
        while (i < 3) {
            new_sock = accept(get_sock, (struct sockaddr *) &local_host, \
                (socklen_t *) &set);
            if (new_sock == -1) {
                printf("accept return:%s errno: %d\n", strerror(errno), errno);
                i++;
                continue;
            } else break;
        }
        if (new_sock == -1) {
            printf("Sorry, you can't use PORT mode. There is something wrong when the server connect to you.\n");
            return;
        }
        ftp_get_reply(sock_control);
        while (true) {
            printf("loop \n");
            count = read(new_sock, rcv_buf, sizeof(rcv_buf));
            if (count <= 0)
                break;
            else {
                write(local_file, rcv_buf, count);
            }
        }
        close(local_file);
        close(get_sock);
        close(new_sock);
        ftp_get_reply(sock_control);
    } else {
        ftp_get_reply(sock_control);
        while (true) {
            count = read(get_sock, rcv_buf, sizeof(rcv_buf));
            if (count <= 0)
                break;
            else {
                write(local_file, rcv_buf, count);
            }
        }
        close(local_file);
        close(get_sock);
        ftp_get_reply(sock_control);
    }
    if (!chmod(src_file, 0644)) {
        printf("chmod %s to 0644\n", dst_file);
        return;
    } else
        printf("chmod %s to 0644 error!\n", dst_file);
    ftp_get_reply(sock_control);
}

//deal with "put" command
void ftp_put(char *usr_cmd) {
    char src_file[512];
    char dst_file[512];
    char send_buf[512];
    struct stat file_info{};
    int local_file;
    int file_put_sock;
    ssize_t count;
    ftp_cmd_filename(usr_cmd, src_file, dst_file);
    if (stat(src_file, &file_info) < 0) {
        printf("local file %s doesn't exist!\n", src_file);
        return;
    }
    local_file = open(src_file, O_RDONLY);
    if (local_file < 0) {
        printf("open local file %s error!\n", dst_file);
        return;
    }
    file_put_sock = xconnect_ftpdata();
    if (file_put_sock < 0) {
        ftp_get_reply(sock_control);
        printf("creat data sock errro!\n");
        return;
    }
    ftp_send_cmd("STOR ", dst_file, sock_control);
    ftp_get_reply(sock_control);
    ftp_send_cmd("TYPE I", nullptr, sock_control);
    ftp_get_reply(sock_control);

    while (true) {
        count = read(local_file, send_buf, sizeof(send_buf));
        if (count <= 0)
            break;
        else {
            write(file_put_sock, send_buf, count);
        }
    }
    close(local_file);
    close(file_put_sock);

    ftp_get_reply(sock_control);
}

//call this function to quit
void ftp_quit() {
    ftp_send_cmd("QUIT", nullptr, sock_control);
    ftp_get_reply(sock_control);
    close(sock_control);
}

//close the connection with the server
void close_cli() {
    ftp_send_cmd("CLOSE", nullptr, sock_control);
    ftp_get_reply(sock_control);
}

//tell the user what current directory is in the server
void ftp_pwd() {
    ftp_send_cmd("PWD", nullptr, sock_control);
    ftp_get_reply(sock_control);
}

//change the directory in the server
void ftp_cd(char *usr_cmd) {
    char *cmd = strchr(usr_cmd, ' ');
    char path[1024];
    if (cmd == nullptr) {
        printf("command error!\n");
        return;
    } else {
        while (*cmd == ' ')
            cmd++;
    }
    if (*cmd == '\0') {
        printf("command error!\n");
        return;
    } else {
        strncpy(path, cmd, strlen(cmd));
        path[strlen(cmd)] = '\0';
        ftp_send_cmd("CWD ", path, sock_control);
        ftp_get_reply(sock_control);
    }
}

//delete
void del(char *usr_cmd) {
    char *cmd = strchr(usr_cmd, ' ');
    char filename[1024];
    if (cmd == nullptr) {
        printf("command error!\n");
        return;
    } else {
        while (*cmd == ' ')
            cmd++;
    }
    if (*cmd == '\0') {
        printf("command error!\n");
        return;
    } else {
        strncpy(filename, cmd, strlen(cmd));
        filename[strlen(cmd)] = '\0';
        ftp_send_cmd("DELE", filename, sock_control);
        ftp_get_reply(sock_control);
    }
}

//mkdir
void mkdir_srv(char *usr_cmd) {
    char *cmd = strchr(usr_cmd, ' ');
    char path[1024];
    if (cmd == nullptr) {
        printf("command error!\n");
        return;
    } else {
        while (*cmd == ' ')
            cmd++;
    }
    if (*cmd == '\0') {
        printf("command error!\n");
        return;
    } else {
        strncpy(path, cmd, strlen(cmd));
        path[strlen(cmd)] = '\0';
        ftp_send_cmd("MKD", path, sock_control);
        ftp_get_reply(sock_control);
    }
}

//rmdir
void rmdir_srv(char *usr_cmd) {
    char *cmd = strchr(usr_cmd, ' ');
    char path[1024];
    if (cmd == nullptr) {
        printf("command error!\n");
        return;
    } else {
        while (*cmd == ' ')
            cmd++;
    }
    if (*cmd == '\0') {
        printf("command error!\n");
        return;
    } else {
        strncpy(path, cmd, strlen(cmd));
        path[strlen(cmd)] = '\0';
        ftp_send_cmd("RMD", path, sock_control);
        ftp_get_reply(sock_control);
    }
}

//list files and directories in local host
void local_list() {
    DIR *dp;
    struct dirent *dirp;
    if ((dp = opendir("./")) == nullptr) {
        printf("opendir() error!\n");
        return;
    }
    printf("Local file list:\n");
    while ((dirp = readdir(dp)) != nullptr) {
        if (strcmp(dirp->d_name, ".") == 0 || strcmp(dirp->d_name, "..") == 0)
            continue;
        printf("%s\n", dirp->d_name);
    }
}

//print local current directory
void local_pwd() {
    char curr_dir[512];
    int size = sizeof(curr_dir);
    if (getcwd(curr_dir, size) == nullptr)
        printf("getcwd failed\n");
    else
        printf("Current local directory: %s\n", curr_dir);
}

//change local directory
void local_cd(char *usr_cmd) {
    char *cmd = strchr(usr_cmd, ' ');
    char path[1024];
    if (cmd == nullptr) {
        printf("command error!\n");
        return;
    } else {
        while (*cmd == ' ')
            cmd++;
    }
    if (*cmd == '\0') {
        printf("command error!\n");
        return;
    } else {
        strncpy(path, cmd, strlen(cmd));
        path[strlen(cmd)] = '\0';
        if (chdir(path) < 0)
            printf("Local: chdir to %s error!\n", path);
        else
            printf("Local: chdir to %s\n", path);
    }
}

void show_help() {
    printf("\033[32mhelp\033[0m\t--print this command list\n");
//////////////////////////////////////////
    printf("\033[32mopen\033[0m\t--open the server\n");
    printf("\033[32mclose\033[0m\t--close the connection with the server\n");
    printf("\033[32mmkdir\033[0m\t--make new dir on the ftp server\n");
    printf("\033[32mrmdir\033[0m\t--delete the dir on the ftp server\n");
    printf("\033[32mdele\033[0m\t--delete the file on the ftp server\n");
//////////////////////////////////////////
    printf("\033[32mpwd\033[0m\t--print the current directory of server\n");
    printf("\033[32mls\033[0m\t--list the files and directoris in current directory of server\n");
    printf("\033[32mcd [directory]\033[0m\n\t--enter  of server\n");
    printf("\033[32mmode\033[0m\n\t--change current mode, PORT or PASV\n");
    printf("\033[32mput [local_file] \033[0m\n\t--send [local_file] to server as \n");
    printf("\tif  isn't given, it will be the same with [local_file] \n");
    printf("\tif there is any \' \' in , write like this \'\\ \'\n");
    printf("\033[32mget [remote file] \033[0m\n\t--get [remote file] to local host as\n");
    printf("\tif  isn't given, it will be the same with [remote_file] \n");
    printf("\tif there is any \' \' in , write like this \'\\ \'\n");
    printf("\033[32mlpwd\033[0m\t--print the current directory of local host\n");
    printf("\033[32mlls\033[0m\t--list the files and directoris in current directory of local host\n");
    printf("\033[32mlcd [directory]\033[0m\n\t--enter  of localhost\n");
    printf("\033[32mbye\033[0m\t--quit this ftp client program\n");
}

//get user and password for login
void get_user() {
    char read_buf[64];
    printf("User name: ");
    fgets(read_buf, sizeof(read_buf), stdin);
    strncpy(user, read_buf, strlen(read_buf) - 1);
}

void get_pass() {
    char read_buf[64];
    printf("Password: ");
    echo_off();
    fgets(read_buf, sizeof(read_buf), stdin);
    strncpy(passwd, read_buf, strlen(read_buf) - 1);
    echo_on();
    printf("\n");
}

//login to the server
void ftp_login() {
    long int err;

    while (true) {
        get_user();
        if (ftp_send_cmd("USER ", user, sock_control) < 0)
            cmd_err_exit("Can not send message", 1);
        err = ftp_get_reply(sock_control);
        if (err == 331)
            break;
        else {
            printf("Username error!\n");
        }
    }

    while (true) {
        get_pass();
        if (ftp_send_cmd("PASS ", passwd, sock_control) < 0) {
            cmd_err_exit("Can not send message", 1);
        }
        err = ftp_get_reply(sock_control);
        if (err == 230) {
            break;
        } else {
            printf("Password error!\n");
        }
    }
}


int ftp_usr_cmd(char *usr_cmd) {
    if (!strncmp(usr_cmd, "open", 4))
        return 15;
    if (!strncmp(usr_cmd, "close", 5))
        return 16;
    if (!strncmp(usr_cmd, "mkdir", 5))
        return 17;
    if (!strncmp(usr_cmd, "rmdir", 5))
        return 18;
    if (!strncmp(usr_cmd, "dele", 4))
        return 19;
    if (!strncmp(usr_cmd, "ls", 2))
        return 1;
    if (!strncmp(usr_cmd, "pwd", 3))
        return 2;
    if (!strncmp(usr_cmd, "cd ", 3))
        return 3;
    if (!strncmp(usr_cmd, "put ", 4))
        return 4;
    if (!strncmp(usr_cmd, "get ", 4))
        return 5;
    if (!strncmp(usr_cmd, "bye", 3))
        return 6;
    if (!strncmp(usr_cmd, "mode", 4))
        return 7;
    if (!strncmp(usr_cmd, "lls", 3))
        return 11;
    if (!strncmp(usr_cmd, "lpwd", 4))
        return 12;
    if (!strncmp(usr_cmd, "lcd ", 4))
        return 13;
    return -1;
}


void start_ftp_cmd(const char *host_ip_addr, long int port) {
    long int err;
    int cmd_flag;
    char usr_cmd[1024];
    err = fill_host_addr(host_ip_addr, &ftp_server, port);
    if (err == 254)
        cmd_err_exit("Invalid port!", 254);
    if (err == 253)
        cmd_err_exit("Invalid server address!", 253);

    sock_control = xconnect(&ftp_server, 1);

    if (ftp_get_reply(sock_control) != 220)
        cmd_err_exit("Connect error!", 220);

    ftp_login();

    while (true) {
        printf("ftp_cli>");
        fgets(usr_cmd, 512, stdin);
        fflush(stdin);
        if (usr_cmd[0] == '\n')
            continue;
        usr_cmd[strlen(usr_cmd) - 1] = '\0';
        cmd_flag = ftp_usr_cmd(usr_cmd);
        switch (cmd_flag) {
            case 1:
                ftp_list();
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 2:
                ftp_pwd();
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 3:
                ftp_cd(usr_cmd);
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 4:
                ftp_put(usr_cmd);
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 5:
                ftp_get(usr_cmd);
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 6:
                ftp_quit();
                printf("BYE TO WEILIQI FTP!\n");
                exit(0);
            case 11:
                local_list();
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 12:
                local_pwd();
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 13:
                local_cd(usr_cmd);
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 16:
                close_cli();
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 17:
                mkdir_srv(usr_cmd);
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 18:
                rmdir_srv(usr_cmd);
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            case 19:
                del(usr_cmd);
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
            default:
                show_help();
                memset(usr_cmd, '\0', sizeof(usr_cmd));
                break;
        }
    }
}

void open_ftpsrv() {
    char usr_cmd[1024];
    int cmd_flag;
    while (true) {
        printf("ftp_cli>");
        fgets(usr_cmd, 512, stdin);
        fflush(stdin);
        if (usr_cmd[0] == '\n')
            continue;
        usr_cmd[strlen(usr_cmd) - 1] = '\0';
        cmd_flag = ftp_usr_cmd(usr_cmd);
        if (cmd_flag == 15) {
            start_ftp_cmd("127.0.0.1", DEFAULT_FTP_PORT);
        }
        if (cmd_flag == 6) {
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        printf("Hello! Welcome to FTP!\n");
        open_ftpsrv();
    } else {
        if (argc == 2 || argc == 3) {
            if (argv[2] == nullptr) {
                start_ftp_cmd(argv[1], DEFAULT_FTP_PORT);;
            } else {
                char *endptr;
                start_ftp_cmd(argv[1], strtol(argv[2], &endptr, 10));
            }
        }
    }
    return 0;
}
