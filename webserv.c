#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <limits.h>
#include <termios.h>
#include "my_threads.h"

/* webserv.c implements a simple HTTP 1.1 server 
with short-lived connections using sockets 
some code is sourced from the APUE textbook (3rd edition)
other sources: https://www.jmarshall.com/easy/http/
*/

enum {               /* constants */
    LOGGING   = 1,   /* if 1, log to terminal, if 0, turn off logging */
    BACKLOG   = 5,   /* number of connections specified in listen() */
    BUF_SIZE  = 500, /* size of buffer to hold http requests and responses */
    SMALL_BUF = 20   /* for things like method names, status messages, etc. */
};

static const char ERR_404[] = "<h2>404 Not Found</h2>";       /* html 404 err msg */
static const char ERR_501[] = "<h2>501 Not Implemented</h2>"; /* html 501 err msg */

// log all requests and responses to terminal
void log_to_stdout(char* data, ssize_t data_size) {
    if (LOGGING) {
        write(STDOUT_FILENO, data, data_size);
    }
}

int uart_open(char* path, speed_t baud){
    struct termios uart_opts;
    int fd = open(path, O_RDWR | O_NOCTTY | O_NDELAY);
    // Flush data already in/out
    if (tcflush(fd,TCIFLUSH)==-1)
        goto err;
    if (tcflush(fd,TCOFLUSH)==-1)
        goto err;
    // Setup modes (8-bit data, disable control signals, readable, no-parity)
    uart_opts.c_cflag = CS8 | CLOCAL | CREAD;    // control modes
    uart_opts.c_iflag=IGNPAR;                           // input modes
    uart_opts.c_oflag=0;                                // output modes
    uart_opts.c_lflag=0;                                // local modes
    // Setup input buffer options: Minimum input: 1byte, no-delay
    uart_opts.c_cc[VMIN]=1;
    uart_opts.c_cc[VTIME]=0;
    // Set baud rate
    cfsetospeed(&uart_opts,baud);
    cfsetispeed(&uart_opts,baud);
    // Apply the settings
    if (tcsetattr(fd,TCSANOW,&uart_opts)==-1)
        goto err;
    
    return fd;
    
err:
    close(fd);
    return -1;
}

int uart_read(int argc, char** argv){
    char buffer[1024];
    int uart;
    
    if(argc < 2){
        printf("Usage: serial-dev-path");
        return EXIT_SUCCESS;
    }
    printf("Setting up\n");
        uart = uart_open(argv[1], B57600);
        if(uart < 0){
            printf("Error: COuld not open %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    printf("Connection Confirmed\n");
        memset(buffer, 0, sizeof(buffer));
        while (1){
            // Put code here to update website!
            read(uart, buffer, sizeof(buffer));
            printf("%s", buffer);
            if (strcmp(buffer, "breach") == 0) {
                printf("Alert! Your security system has been breached!");
            }
            memset(buffer, 0, sizeof(buffer));
        }
        // Connection is closed/lost, close the file and exit
        close(uart);
        return EXIT_SUCCESS;
        
    }

// formats a date for the http header like: Fri, 31 Dec 1999 23:59:59 GMT
// source: https://stackoverflow.com/a/7548846
void get_date(char* buf, int date_size) {
    time_t now = time(0);
    struct tm tm = *gmtime(&now);
    strftime(buf, date_size, "%a, %d %b %Y %H:%M:%S %Z", &tm);
}

// code adapted from https://stackoverflow.com/a/5309508
// if a file extension doesn't exist, return ""
char* get_file_extension(char* filename) {
    char *dot = strrchr(filename, '.');
    if (dot == NULL || dot == filename) {
        return "";
    }
    return dot + 1;
}

// returns 1 if filename has extension ext, 0 otherwise
int has_extension(char* filename, char* ext) {
    if (strcmp(get_file_extension(filename), ext) == 0) {
        return 1;
    }
    return 0;
}

int is_image(char* filename) {
    if (has_extension(filename, "jpg")) {
        return 1;
    } else if (has_extension(filename, "jpeg")) {
        return 1;
    } else if (has_extension(filename, "gif")) {
        return 1;
    } else if (has_extension(filename, "png")) {
        return 1;
    }
    return 0;
}

// returns 1 if filename has a cgi extension, 0 otherwise
// a separate method is needed because filename could have
// query parameters (eg: ./temp.cgi?test=value1)
int is_cgi(char* filename) {
    char ext[20];
    char* full_extension = get_file_extension(filename);
    if (strlen(full_extension) < 3) {
        return 0;
    } else {
        // get the next 3 letters after the dot, store in ext
        sprintf(ext, "%.*s", 3, full_extension);
        if (strcmp(ext, "cgi") == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * this function can be used to create and format an HTTP 1.1 response
 * the content of the response must be sent separately
 * buf: response is stored in buf (size BUF_SIZE)
 * status: HTTP status code (eg. 200, 404, 501) 
 * type: content type (eg. text/plain)
 */  
void create_response(char* buf, int size, int status, char* type) {
    char date[BUF_SIZE];
    get_date(date, BUF_SIZE);

    // assign a status message to go with the status code
    char status_msg[SMALL_BUF];
    if (status == 200) {
        strcpy(status_msg, "OK");
    } else if (status == 404) {
        strcpy(status_msg, "Not Found");
    } else if (status == 501) {
        strcpy(status_msg, "Not Implemented");
    } else {
        strcpy(status_msg, "Unknown");
    }

    char* response = "HTTP/1.1 %d %s\r\nDate: %s\r\nConnection: close\r\nContent-Type: %s\r\n\r\n";
    snprintf(buf, size, response, status, status_msg, date, type);
}

// log a response, and send it to the client
void send_response(int client, char* response) {
    int response_len = strlen(response);
    log_to_stdout(response, response_len);
    if (send(client, response, response_len, 0) == -1) {
        perror("Send failed");
    }
}

void send_status(int client, int status_code) {
    char response[BUF_SIZE];
    create_response(response, BUF_SIZE, status_code, "text/html");
    send_response(client, response);

    if (status_code == 404) {
        /* file not found error */
        send(client, ERR_404, sizeof(ERR_404), 0);
    } else if (status_code == 501) {
        /* method not implemented error */
        send(client, ERR_501, sizeof(ERR_501), 0);
    }
}

// lists a directory and sends it to the client
void handle_dir(int client, char* dir_name) {
    // send the initial response head
    char response[BUF_SIZE];
    create_response(response, BUF_SIZE, 200, "text/plain");
    send_response(client, response);

    // popen an ls -l process, read from the pipe and send it to the client
    // adapted from APUE book (pg 615-616)
    char cmd[NAME_MAX];
    snprintf(cmd, NAME_MAX, "ls -l %s", dir_name); // format ls -l on dir_name

    FILE* fp;
    char ls_entry[BUF_SIZE];
    if ((fp = popen(cmd, "r")) == NULL) {
        perror("Popen ls -l error");
    } else {
        // read a line from the ls -l process and send it to the client
        while (fgets(ls_entry, BUF_SIZE, fp) != NULL) {
            send(client, ls_entry, strlen(ls_entry), 0);
        }
        pclose(fp);
    }
}

// fopens an html file and sends it line by line to client
void handle_html(int client, char* html_file) {
    // send the initial response head
    char response[BUF_SIZE];
    create_response(response, BUF_SIZE, 200, "text/html");
    send_response(client, response);

    FILE* fp;
    char line[BUF_SIZE];

    if ((fp = fopen(html_file, "r")) == NULL) {
        perror("fopen html error");
    } else {
        // read a line from the html file and send it to the client
        while (fgets(line, BUF_SIZE, fp) != NULL) {
            send(client, line, strlen(line), 0);
        }
        fclose(fp);
    }
}

// must provide the image extension (img_ext) to use (eg: "jpg")
void handle_img(int client, char* img_file, char* img_ext) {
    // send the initial response head
    char response[BUF_SIZE];
    char content_type[SMALL_BUF]; // format the content with img_ext
    snprintf(content_type, SMALL_BUF, "image/%s", img_ext);
    create_response(response, BUF_SIZE, 200, content_type);
    send_response(client, response);

    int fd;
    char buf[BUF_SIZE];
    int n_read;
    if ((fd = open(img_file, O_RDONLY)) == -1) {
        perror("open image error");
    } else {
        // read from the image and write to the client
        while ((n_read = read(fd, buf, BUF_SIZE)) > 0) {
            send(client, buf, n_read, 0);
        }
        close(fd);
    }
}

// cgi script must have execution permission (755) and 
// have the right shebang (eg. #!/usr/bin/python)
void handle_script(int client, char* script_file, char* query_str) {
    // send the initial response head, without the content type (sent by cgi)
    char date[BUF_SIZE];
    get_date(date, BUF_SIZE);
    char response[BUF_SIZE];
    char* response_text = "HTTP/1.1 %d %s\r\nDate: %s\r\nConnection: close\r\n";
    snprintf(response, BUF_SIZE, response_text, 200, "OK", date);
    send_response(client, response);

    // set QUERY_STRING so that our cgi script can access query_str
    if (setenv("QUERY_STRING", query_str, 1) == -1) {
        perror("QUERY_STRING set_env error");
    }
    system("echo $QUERY_STRING");
    FILE* fp;
    char script_output[BUF_SIZE];
    if ((fp = popen(script_file, "r")) == NULL) {
        perror("Popen script error");
    } else {
        // read a line from the script process and send it to the client
        while (fgets(script_output, BUF_SIZE, fp) != NULL) {
            send(client, script_output, strlen(script_output), 0);
        }
        pclose(fp);
    }
    unsetenv("QUERY_STRING");
}

// handles the url "/security"
void handle_security(int client, char* query_str) {
    handle_html(client, "security-page.html");
    if (strstr(query_str, "true") != NULL) {
        printf("Setting up\n");
        char* path = "/dev/cu.usbmodem14101";
        int uart = uart_open(path, B57600);
        if(uart < 0){
            printf("Error: Could not open %s\n", path);
        }
        printf("Connection Confirmed\n");

        const char delim[2] = "=";
        char *distance;
        
        /* get the first token */
        distance = strtok(query_str, delim);
        distance = strtok(NULL, delim);
        distance = strtok(NULL, delim);
        /* walk through other tokens */
        printf("distance: %s.\n",distance);
        //write(uart, distance, strlen(distance));

        char buffer[1024];
        memset(buffer, 0, sizeof(buffer));
        while (1){
            // Put code here to update website!
            read(uart, buffer, sizeof(buffer));
            printf("%s", buffer);
            if (strcmp(buffer, "SECURITY BREACH") == 0) {
                send(client, "<p>", 3, 0);
                send(client, buffer, strlen(buffer), 0);
                send(client, "</p>", 4, 0);
                break;
            }
            memset(buffer, 0, sizeof(buffer));
        }
        // Connection is closed/lost, close the file and exit
        close(uart);
    }
}

// if file exists, it is either a directory or a regular file
// if directory, call ls -l on it
// if reg file, determine file extension (eg. .html, .py, .jpg, etc.)
void handle_GET(int client, char* file_request, char* query_string) {
    struct stat sb;
    if (stat(file_request, &sb) == -1) {
        /* if stat fails, assume file doesn't exist */
        perror("stat");
        send_status(client, 404);
    } 
    else if (S_ISDIR(sb.st_mode)) {
        /* handle directory files */
        handle_dir(client, file_request);
    } 
    else if (S_ISREG(sb.st_mode)) {
        /* handle regular files */
        if (has_extension(file_request, "html")) {
            handle_html(client, file_request);
        }
        else if (is_image(file_request)) {
            handle_img(client, file_request, get_file_extension(file_request));
        }
        else if (is_cgi(file_request)) {
            handle_script(client, file_request, query_string);
        }
        else if (strcmp(file_request, "security")) {
            handle_security(client, query_string);
        }
        else {
            send_status(client, 501);
        }
    } else {
        /* unknown file type */
        send_status(client, 501);
    }
}

// given "/some/file/path?query=value&query2=value2"
// return "?query=value&query2=value2" or "" if no query exists
char* parse_query(char* requested_path) {
    char* query_ptr = strchr(requested_path, '?');
    if (query_ptr == NULL) {
        return "";
    } else {
        return query_ptr;
    }
}

// determines the method of the request (eg. GET) and the requested file
// then calls the appropriate function to deal with the request
void handle_request(int client, char* request) {
    printf("in handling resquests\n");
    printf("client id is %d\n", client);
    printf("ok al least we can print that \n");
    // parse the request by getting the first and second word
    char method[SMALL_BUF];
    char temp_path[PATH_MAX];
    if (sscanf(request, "%s %s", method, temp_path) < 2) {
        perror("Parsing request failed");
    }

    printf("temp path is: %s.\n", temp_path);

    // search for a query string inside the GET path (temp_path)
    // if one exists, remove it from temp_path
    char* query_ptr = parse_query(temp_path);
    if (strcmp(query_ptr, "") != 0) {
        int index = query_ptr - temp_path; // the starting index of the query
        temp_path[index] = '\0';
        query_ptr += 1;
    }

    printf("temp path is now: %s, and query is %s.\n", temp_path, query_ptr);

    // add a "." before the requested path
    char relative_path[PATH_MAX];
    snprintf(relative_path, PATH_MAX, ".%s", temp_path);

    // deal with the request
    if (strcmp(method, "GET") == 0) {
        handle_GET(client, relative_path, query_ptr);
    } else {
        // no other methods have been implemented
        send_status(client, 501);
    }
}

void serve_request(void* param) {
    printf("now serving request\n");
    int* int_param = param;
    int client_fd = *int_param;
    char* buf = malloc(BUF_SIZE);
    ssize_t n_read = recv(client_fd, buf, BUF_SIZE, 0);
    log_to_stdout(buf, n_read);
    printf("client id is %d and request is %s\n", client_fd, buf);
    //handle_request(client_fd, buf);
    handle_dir(client_fd, "./");
    close(client_fd);
    printf("request served\n");
    my_thr_exit(); // terminate the thread   
}

// code adapted from APUE textbook (pg 617-618)
// if threads == 0, accept connections and fork a child process to serve requests
// if threads == 1, accept connections and create a thread for each request
int serve(int server, int threads) {
    int client_fd, status;
    pid_t pid;
    int thread_id = 0;
    for(;;) {
        // accept a new connection (from our local machine)
        // accept will block if no connections are pending
        if ((client_fd = accept(server, NULL, NULL)) < 0) {
            perror("Accept error");
            exit(1);
        }
        if (threads) {
            int* client_ptr = malloc(sizeof(int));
            *client_ptr = client_fd;
            my_thr_create(thread_id++, serve_request, client_ptr);
            my_thr_start();
            free(client_ptr);
        } else {
            // process the client's request by forking a child process
            if ((pid = fork()) < 0) {
                perror("fork error");
                exit(1);
            } else if (pid == 0) {
                close(server);
                char buf[BUF_SIZE];
                ssize_t n_read = recv(client_fd, buf, BUF_SIZE, 0);
                log_to_stdout(buf, n_read);
                handle_request(client_fd, buf);
                close(client_fd);
                exit(0); // terminate the child process
            } else {
                /* parent */
                close(client_fd); // close the client fd because the child still maintains a copy
                waitpid(pid, &status, 0);
            }
        }
    }
}

int start_server(uint16_t port, int threads) {
    // create a socket for our webserver
    int webserver;
    webserver = socket(AF_INET, SOCK_STREAM, 0);

    // set up the address of the server
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = INADDR_ANY;

    // connect address and webserver together using bind
    if (bind(webserver, (struct sockaddr*) &server_address, sizeof(server_address)) == -1) {
        perror("Bind error");
        return 1;
    }
    
    // have our webserver listen to up to BACKLOG connections
    if (listen(webserver, BACKLOG) == -1) {
        perror("Listen error");
        return 1;
    }
    
    serve(webserver, threads);

    // might want to set up a signal handler for sigint to call close
    close(webserver);
    return 0;
}


int main(int argc, char const *argv[]) {
    if (argc < 2) {
        printf("Error: to start server use: $./webserv port-number\n");
        exit(1);
    }   
    uint16_t port = (uint16_t) strtol(argv[1], NULL, 10);
    int threads = (int) strtol(argv[2], NULL, 10);
    printf("Running on http://127.0.0.1:%u/ (Press CTRL+C to quit)\n", port);

    if (start_server(port, threads) != 0) {
        exit(1);
    }
    return 0;
}
