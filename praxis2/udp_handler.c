#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#define BUFFER_SIZE 1024

void handle_udp_message(int udp_sock) {
    struct sockaddr_in client_addr;
    char buffer[BUFFER_SIZE];
    socklen_t addr_len = sizeof(client_addr);

    ssize_t n = recvfrom(udp_sock, buffer, BUFFER_SIZE, 0,
                         (struct sockaddr *)&client_addr, &addr_len);

    if (n > 0) {
        buffer[n] = '\0';
        printf("Received UDP message: %s\n", buffer);

        // Echo the message back to the client
        sendto(udp_sock, buffer, n, 0, (struct sockaddr *)&client_addr, addr_len);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <IP> <Port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    // Create TCP socket
    int tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) {
        perror("TCP socket failed");
        exit(EXIT_FAILURE);
    }

    // Create UDP socket
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("UDP socket failed");
        close(tcp_sock);
        exit(EXIT_FAILURE);
    }

    // Configure address
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid IP address");
        close(tcp_sock);
        close(udp_sock);
        exit(EXIT_FAILURE);
    }

    // Bind both sockets to the same port
    if (bind(tcp_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0 ||
        bind(udp_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(tcp_sock);
        close(udp_sock);
        exit(EXIT_FAILURE);
    }

    // Listen on TCP socket
    if (listen(tcp_sock, 5) < 0) {
        perror("Listen failed");
        close(tcp_sock);
        close(udp_sock);
        exit(EXIT_FAILURE);
    }

    printf("Server running on %s:%d\n", ip, port);

    // Monitor sockets using select()
    fd_set read_fds;
    int max_fd = (tcp_sock > udp_sock) ? tcp_sock : udp_sock;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(tcp_sock, &read_fds);
        FD_SET(udp_sock, &read_fds);

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);

        if (activity < 0) {
            perror("select() failed");
            break;
        }

        if (FD_ISSET(udp_sock, &read_fds)) {
            handle_udp_message(udp_sock);
        }

        if (FD_ISSET(tcp_sock, &read_fds)) {
            int client_sock = accept(tcp_sock, NULL, NULL);
            if (client_sock >= 0) {
                printf("New TCP connection\n");
                close(client_sock); // Close the client socket for this example
            }
        }
    }

    close(tcp_sock);
    close(udp_sock);
    return 0;
}
