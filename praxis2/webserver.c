#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "data.h"
#include "http.h"
#include "util.h"

#define HTTP_OK "200 OK"
#define HTTP_NOT_FOUND "404 Not Found"
#define HTTP_METHOD_NOT_ALLOWED "405 Method Not Allowed"
#define HTTP_INTERNAL_SERVER_ERROR "500 Internal Server Error"

// Define some constants for Content Types
#define CONTENT_TYPE_HTML "Content-Type: text/html\r\n"
#define CONTENT_TYPE_JSON "Content-Type: application/json\r\n"


#define MAX_RESOURCES 100
#define MAX_REQUESTS 10

struct tuple resources[MAX_RESOURCES] = {
    {"/static/foo", "Foo", sizeof "Foo" - 1},
    {"/static/bar", "Bar", sizeof "Bar" - 1},
    {"/static/baz", "Baz", sizeof "Baz" - 1}};


struct message {
    uint8_t message_type;        // 1 byte
    uint16_t hash_id;     // 2 bytes
    uint16_t node_id;     // 2 bytes
    struct in_addr ip_address;      // 4 bytes 
    uint16_t node_port;     // 2 bytes
} __attribute__((packed));

struct lookup_request {
    uint16_t hash_id;     // Unique identifier for the request
    char* node_ip;     // Node IP (IPv4 as string, e.g., "127.0.0.1")
    uint16_t node_port;   // Node port
};

int request_count = 0; // Tracks the number of stored requests
struct lookup_request requests[MAX_REQUESTS];

char *PRED_ID, *PRED_IP, *PRED_PORT, *SUCC_ID, *SUCC_IP, *SUCC_PORT, *ID, *IP, *PORT;

void send_reply(int conn, struct request *request, int udp_socket);
size_t process_packet(int conn, char *buffer, size_t n, int udp_socket);
static void connection_setup(struct connection_state *state, int sock);
char *buffer_discard(char *buffer, size_t discard, size_t keep);
bool handle_connection(struct connection_state *state, int udp_socket);
static struct sockaddr_in derive_sockaddr(const char *host, const char *port);
static int setup_server_socket(struct sockaddr_in addr);
static int setup_datagram_socket(const char *host, const char *port);
void send_udp_message(int socket ,uint8_t message_type, uint16_t hash_id, uint16_t node_id, char* ip_address, uint16_t node_port, struct in_addr send_ip, uint16_t send_port);
void add_request(struct lookup_request new_request);
int has_request(uint16_t hash_id);
void find_and_write(uint16_t hash_id, char* ip, char* port);
int fetch_req_index(uint16_t hash_id);

void send_reply(int conn, struct request *request, int udp_socket) {
    // Common HTTP response templates
    const char *OK_REPLY_FORMAT = "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n";
    const char *NOT_FOUND_REPLY = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    const char *SERVICE_UNAVAILABLE_REPLY = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 0\r\n\r\n";
    const char *METHOD_NOT_SUPPORTED_REPLY = "HTTP/1.1 501 Method Not Supported\r\n\r\n";
    const char *NO_CONTENT_REPLY = "HTTP/1.1 204 No Content\r\n\r\n";
    const char *CREATED_REPLY = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n";
    const char *SEE_OTHER_REPLY_FORMAT = "HTTP/1.1 303 See Other\r\nLocation:%s:%s%s\r\nContent-Length: 0\r\n\r\n";

    fprintf(stderr, "Handling %s request for %s (%lu byte payload)\n",
            request->method, request->uri, request->payload_length);

    uint16_t uri_hash = pseudo_hash((const unsigned char *)request->uri, strlen(request->uri));
    char uri_hash_string[6];
    snprintf(uri_hash_string, sizeof(uri_hash_string), "%u", uri_hash);

    // Buffer to store the reply
    char buffer[HTTP_MAX_SIZE];
    const char *reply = buffer;  // Change reply to const char* to avoid warnings
    size_t offset = 0;

    if (strcmp(request->method, "GET") == 0) {
        size_t resource_length;
        const char *resource = get(uri_hash_string, resources, MAX_RESOURCES, &resource_length);

        if (resource) {
            // Resource found
            offset = snprintf(reply, HTTP_MAX_SIZE, OK_REPLY_FORMAT, resource_length);
            memcpy((char*)reply + offset, resource, resource_length);  // Cast to modify content
            offset += resource_length;
        } else {
            // Resource not found
            if (strcmp(PRED_ID, SUCC_ID) == 0 && uri_hash != atoi(ID)) {
                reply = NOT_FOUND_REPLY;
            } else {
                if (!has_request(uri_hash)) {
                    // Request not in progress, send service unavailable
                    reply = SERVICE_UNAVAILABLE_REPLY;

                    struct sockaddr_in udp_addr = derive_sockaddr(SUCC_IP, SUCC_PORT);
                    send_udp_message(udp_socket, 0, htons(uri_hash), htons(atoi(ID)), IP, htons(atoi(PORT)), udp_addr.sin_addr, udp_addr.sin_port);

                    struct lookup_request new_request = {uri_hash, NULL, 0};
                    add_request(new_request);
                } else {
                    // Request in progress, check if we have an answer
                    int index = fetch_req_index(uri_hash);
                    if (requests[index].node_ip == NULL || requests[index].node_port == NULL) {
                        reply = NOT_FOUND_REPLY;
                    } else {
                        snprintf(reply, HTTP_MAX_SIZE, SEE_OTHER_REPLY_FORMAT,
                                 requests[index].node_ip, requests[index].node_port, request->uri);
                        memset(&requests[index], 0, sizeof(struct request));
                    }
                }
            }
            offset = strlen(reply);
        }
    } else if (strcmp(request->method, "PUT") == 0) {
        if (set(uri_hash_string, request->payload, request->payload_length, resources, MAX_RESOURCES)) {
            reply = NO_CONTENT_REPLY;
        } else {
            reply = CREATED_REPLY;
        }
        offset = strlen(reply);
    } else if (strcmp(request->method, "DELETE") == 0) {
        if (delete(uri_hash_string, resources, MAX_RESOURCES)) {
            reply = NO_CONTENT_REPLY;
        } else {
            reply = NOT_FOUND_REPLY;
        }
        offset = strlen(reply);
    } else {
        // Unsupported method
        reply = METHOD_NOT_SUPPORTED_REPLY;
        offset = strlen(reply);
    }

    // Send the reply back to the client
    if (send(conn, reply, offset, 0) == -1) {
        perror("send");
        close(conn);
    }
}

int main(int argc, char **argv) {
    // uint16_t h1 = pseudo_hash("/static/foo", sizeof "/static/foo" - 1);
    // uint16_t h2 = pseudo_hash("/static/bar", sizeof "/static/bar" - 1);
    // uint16_t h3 = pseudo_hash("/static/baz", sizeof "/static/baz" - 1);
    // printf("%d , %d, %d \n", h1,h2,h3);
    PRED_ID = getenv("PRED_ID");
    PRED_IP = getenv("PRED_IP");
    PRED_PORT = getenv("PRED_PORT");
    SUCC_ID = getenv("SUCC_ID");
    SUCC_IP = getenv("SUCC_IP");
    SUCC_PORT = getenv("SUCC_PORT");
    if (argc > 3) {
        ID = argv[3];
    } else {
        ID = "0"; 
    }
    IP = argv[1];
    PORT = argv[2];

    
    memset(requests, 0, sizeof(requests));
    

    // printf("PRED_ID=%s, PRED_IP=%s, PRED_PORT=%s, SUCC_ID=%s, SUCC_IP=%s, SUCC_PORT=%s, ID=%s\n", 
    // PRED_ID, PRED_IP, PRED_PORT, SUCC_ID, SUCC_IP, SUCC_PORT, ID);

    struct sockaddr_in addr = derive_sockaddr(argv[1], argv[2]);

    // Set up a server socket.
    int server_socket = setup_server_socket(addr);

    // Set up a datagram socket
    int datagram_socket = setup_datagram_socket(argv[1], argv[2]);

    // Create an array of pollfd structures to monitor sockets.
    struct pollfd sockets[2] = {
        {.fd = server_socket, .events = POLLIN},
        {.fd = datagram_socket, .events = POLLIN},
    };

    struct connection_state state = {0};
    while (true) {

        // Use poll() to wait for events on the monitored sockets.
        int ready = poll(sockets, sizeof(sockets) / sizeof(sockets[0]), -1);
        if (ready == -1) {
            perror("poll");
            exit(EXIT_FAILURE);
        }
        printf("UDP socket descriptor: %d\n", datagram_socket);
        // Process events on the monitored sockets.
        for (size_t i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i += 1) {
            if (sockets[i].revents != POLLIN) {
                // If there are no POLLIN events on the socket, continue to the
                // next iteration.
                continue;
            }
            int s = sockets[i].fd;

            if (s == server_socket) {
                printf("doing tcp\n");
                // If the event is on the server_socket, accept a new connection
                // from a client.
                int connection = accept(server_socket, NULL, NULL);
                if (connection == -1 && errno != EAGAIN &&
                    errno != EWOULDBLOCK) {
                    close(server_socket);
                    perror("accept");
                    exit(EXIT_FAILURE);
                } else {
                    connection_setup(&state, connection);

                    // limit to one connection at a time
                    sockets[0].events = 0;
                    sockets[1].fd = connection;
                    sockets[1].events = POLLIN;
                }
                printf("end tcp\n");
            } else if (s == datagram_socket) {
                char buffer[1024];
                struct sockaddr_in sender_addr;
                socklen_t addr_len = sizeof(sender_addr);

                ssize_t num_bytes = recvfrom(s, buffer, sizeof(buffer), 0,
                                            (struct sockaddr *)&sender_addr, &addr_len);
                if (num_bytes == -1) {
                    perror("recvfrom");
                    continue;
                }

                if (num_bytes < sizeof(struct message)) {
                    fprintf(stderr, "Received message is too short to unpack\n");
                    continue;
                }

                // Cast the buffer to a pointer to your struct message
                struct message *received_msg = (struct message *)buffer;

                // Access the fields of the unpacked struct
                printf("Received UDP message:\n");
                printf("  Message Type: %u\n", received_msg->message_type);
                printf("  PRED ID: %u\n", atoi(PRED_ID)); 
                printf("  CURR ID: %u\n", atoi(ID)); 
                printf("  Hash ID: %u\n", ntohs(received_msg->hash_id));  // Convert from network to host byte order
                printf("  SUCC ID: %u\n", atoi((SUCC_ID))); 
                printf("  Node ID: %u\n", ntohs(received_msg->node_id));  // Convert from network to host byte order
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &received_msg->ip_address, ip_str, sizeof(ip_str)); // Convert IP to string
                printf("  IP Address: %s\n", ip_str);
                printf("  Node Port: %u\n", ntohs(received_msg->node_port)); 
                printf("  SUCC IP: %s\n", SUCC_IP);
                printf("  SUCC Port: %s\n", SUCC_PORT); 

                if (received_msg->message_type == 0) {
                    if (ntohs(received_msg->hash_id) > atoi(ID) && ntohs(received_msg->hash_id) < atoi(SUCC_ID)) { // Successor responsible
                        printf("res\n");
                        send_udp_message(datagram_socket , 1, htons(atoi(ID)), htons(atoi(SUCC_ID)), SUCC_IP, htons(atoi(SUCC_PORT)), 
                        received_msg->ip_address, received_msg->node_port);
                    } else if (ntohs(received_msg->hash_id) < atoi(ID) && ntohs(received_msg->hash_id) > atoi(SUCC_ID)) { // Current responsible
                        send_udp_message(datagram_socket , 1, htons(atoi(PRED_ID)), htons(atoi(ID)), IP, htons(atoi(PORT)), 
                        received_msg->ip_address, received_msg->node_port);
                    } else { // forward lookup
                        char ip_succ[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &received_msg->ip_address, ip_str, sizeof(ip_str));

                        
                        if (strcmp(ip_succ, SUCC_IP) == 0 || atoi(SUCC_PORT) == ntohs(received_msg->node_port)) {
                            printf("end of line sending reply to originator\n");
                            struct sockaddr_in udp_addr;
                            memset(&udp_addr, 0, sizeof(udp_addr));
                            udp_addr.sin_family = AF_INET;
                            udp_addr.sin_port = htons(atoi(SUCC_PORT));         
                            inet_pton(AF_INET, SUCC_IP, &udp_addr.sin_addr); 

                            char ip_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &received_msg->ip_address, ip_str, sizeof(ip_str));
                            send_udp_message(datagram_socket , 1, received_msg->hash_id, received_msg->node_id, 
                            ip_str, received_msg->node_port, udp_addr.sin_addr, udp_addr.sin_port);    
                        } else {
                            printf("forward kek\n");
                            struct sockaddr_in udp_addr;
                            memset(&udp_addr, 0, sizeof(udp_addr));
                            udp_addr.sin_family = AF_INET;
                            udp_addr.sin_port = htons(atoi(SUCC_PORT));         
                            inet_pton(AF_INET, SUCC_IP, &udp_addr.sin_addr); 

                            char ip_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &received_msg->ip_address, ip_str, sizeof(ip_str));
                            send_udp_message(datagram_socket , received_msg->message_type, received_msg->hash_id, received_msg->node_id, 
                            ip_str, received_msg->node_port, udp_addr.sin_addr, udp_addr.sin_port);    
                        }
                         
                    }
                } else {
                    printf("got reply\n");
                    // find lookup in requests[] and write
                    int index = fetch_req_index(received_msg->hash_id);
                    if (index >= 0) {
                        printf("request found overwriting...\n");
                        inet_ntop(AF_INET, &received_msg->ip_address, requests[index].node_ip, sizeof(requests[index].node_ip));
                        requests[index].node_port = received_msg->node_port;
                    } else {
                        printf("request not found\n");
                        perror("hash not found");
                    }
                }
                

            } else  {
                assert(s == state.sock);

                // Call the 'handle_connection' function to process the incoming
                // data on the socket.
                bool cont = handle_connection(&state, datagram_socket);
                if (!cont) { // get ready for a new connection
                    printf("reset\n");
                    sockets[0].events = POLLIN;
                    sockets[1].events = POLLIN;
                    sockets[2].fd = -1;
                    sockets[2].events = 0;
                    if (sockets[1].fd != datagram_socket) {
                        sockets[1].fd = -1;
                        sockets[1].events = 0;
                    }
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

/**
 * Sends an HTTP reply to the client based on the received request.
 *
 * @param conn      The file descriptor of the client connection socket.
 * @param request   A pointer to the struct containing the parsed request
 * @param upd_socket UDP Socket for DHT lookups
 * information.
 */


/**
 * Processes an incoming packet from the client.
 *
 * @param conn The socket descriptor representing the connection to the client.
 * @param buffer A pointer to the incoming packet's buffer.
 * @param n The size of the incoming packet.
 *
 * @return Returns the number of bytes processed from the packet.
 *         If the packet is successfully processed and a reply is sent, the
 * return value indicates the number of bytes processed. If the packet is
 * malformed or an error occurs during processing, the return value is -1.
 *
 */
size_t process_packet(int conn, char *buffer, size_t n, int udp_socket) {
    struct request request = {
        .method = NULL, .uri = NULL, .payload = NULL, .payload_length = -1};
    ssize_t bytes_processed = parse_request(buffer, n, &request);

    if (bytes_processed > 0) {
        send_reply(conn, &request, udp_socket);

        // Check the "Connection" header in the request to determine if the
        // connection should be kept alive or closed.
        const string connection_header = get_header(&request, "Connection");
        if (connection_header && strcmp(connection_header, "close")) {
            return -1;
        }
    } else if (bytes_processed == -1) {
        // If the request is malformed or an error occurs during processing,
        // send a 400 Bad Request response to the client.
        const string bad_request = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(conn, bad_request, strlen(bad_request), 0);
        printf("Received malformed request, terminating connection.\n");
        close(conn);
        return -1;
    }

    return bytes_processed;
}

/**
 * Sets up the connection state for a new socket connection.
 *
 * @param state A pointer to the connection_state structure to be initialized.
 * @param sock The socket descriptor representing the new connection.
 *
 */
static void connection_setup(struct connection_state *state, int sock) {
    // Set the socket descriptor for the new connection in the connection_state
    // structure.
    state->sock = sock;

    // Set the 'end' pointer of the state to the beginning of the buffer.
    state->end = state->buffer;

    // Clear the buffer by filling it with zeros to avoid any stale data.
    memset(state->buffer, 0, HTTP_MAX_SIZE);
}

/**
 * Discards the front of a buffer
 *
 * @param buffer A pointer to the buffer to be modified.
 * @param discard The number of bytes to drop from the front of the buffer.
 * @param keep The number of bytes that should be kept after the discarded
 * bytes.
 *
 * @return Returns a pointer to the first unused byte in the buffer after the
 * discard.
 * @example buffer_discard(ABCDEF0000, 4, 2):
 *          ABCDEF0000 ->  EFCDEF0000 -> EF00000000, returns pointer to first 0.
 */
char *buffer_discard(char *buffer, size_t discard, size_t keep) {
    memmove(buffer, buffer + discard, keep);
    memset(buffer + keep, 0, discard); // invalidate buffer
    return buffer + keep;
}

/**
 * Handles incoming connections and processes data received over the socket.
 *
 * @param state A pointer to the connection_state structure containing the
 * connection state.
 * @return Returns true if the connection and data processing were successful,
 * false otherwise. If an error occurs while receiving data from the socket, the
 * function exits the program.
 */
bool handle_connection(struct connection_state *state, int udp_socket) {
    // Calculate the pointer to the end of the buffer to avoid buffer overflow
    const char *buffer_end = state->buffer + HTTP_MAX_SIZE;

    // Check if an error occurred while receiving data from the socket
    ssize_t bytes_read =
        recv(state->sock, state->end, buffer_end - state->end, 0);
    if (bytes_read == -1) {
        perror("recv");
        close(state->sock);
        exit(EXIT_FAILURE);
    } else if (bytes_read == 0) {
        return false;
    }

    char *window_start = state->buffer;
    char *window_end = state->end + bytes_read;

    ssize_t bytes_processed = 0;
    while ((bytes_processed = process_packet(state->sock, window_start,
                                             window_end - window_start, udp_socket)) > 0) {
        window_start += bytes_processed;
    }
    if (bytes_processed == -1) {
        return false;
    }

    state->end = buffer_discard(state->buffer, window_start - state->buffer,
                                window_end - window_start);
    return true;
}

/**
 * Derives a sockaddr_in structure from the provided host and port information.
 *
 * @param host The host (IP address or hostname) to be resolved into a network
 * address.
 * @param port The port number to be converted into network byte order.
 *
 * @return A sockaddr_in structure representing the network address derived from
 * the host and port.
 */
static struct sockaddr_in derive_sockaddr(const char *host, const char *port) {
    struct addrinfo hints = {
        .ai_family = AF_INET,
    };
    struct addrinfo *result_info;

    // Resolve the host (IP address or hostname) into a list of possible
    // addresses.
    int returncode = getaddrinfo(host, port, &hints, &result_info);
    if (returncode) {
        fprintf(stderr, "Error parsing host/port");
        exit(EXIT_FAILURE);
    }

    // Copy the sockaddr_in structure from the first address in the list
    struct sockaddr_in result = *((struct sockaddr_in *)result_info->ai_addr);

    // Free the allocated memory for the result_info
    freeaddrinfo(result_info);
    return result;
}

/**
 * Sets up a TCP server socket and binds it to the provided sockaddr_in address.
 *
 * @param addr The sockaddr_in structure representing the IP address and port of
 * the server.
 *
 * @return The file descriptor of the created TCP server socket.
 */
static int setup_server_socket(struct sockaddr_in addr) {
    const int enable = 1;
    const int backlog = 1;

    // Create a socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Avoid dead lock on connections that are dropped after poll returns but
    // before accept is called
    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }

    // Set the SO_REUSEADDR socket option to allow reuse of local addresses
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) ==
        -1) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Bind socket to the provided address
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // Start listening on the socket with maximum backlog of 1 pending
    // connection
    if (listen(sock, backlog)) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return sock;
}

static int setup_datagram_socket(const char *host, const char *port) {
    struct addrinfo hints, *servinfo, *p;
    const int enable = 1;
    int rv, sock;

    if (sock == -1) {
        perror("UDP socket");
        exit(EXIT_FAILURE);
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; 
    hints.ai_socktype = SOCK_DGRAM;

    if ((rv = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in result = *((struct sockaddr_in *)servinfo->ai_addr);

    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sock = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("talker: socket");
            continue;
        }

        break;
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) ==
        -1) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    if (p == NULL) {
        fprintf(stderr, "talker: failed to create socket\n");
        freeaddrinfo(servinfo);
        exit(EXIT_FAILURE);
    }

    if (bind(sock, (struct sockaddr *)&result, sizeof(result)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(servinfo);
    return sock;
}

void send_udp_message(int socket ,uint8_t message_type, uint16_t hash_id, uint16_t node_id, char* ip_address, uint16_t node_port, struct in_addr send_ip, uint16_t send_port) {
    struct message udp_message;
    udp_message.message_type = message_type;
    udp_message.hash_id = hash_id; 
    udp_message.node_id = node_id;
    inet_pton(AF_INET, ip_address, &udp_message.ip_address); // Convert IP address to binary format
    udp_message.node_port = node_port;      // Convert port to network byte order

    // Define the UDP address
    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = send_port;          // UDP port (already in network byte order)
    udp_addr.sin_addr = send_ip;
    // inet_pton(AF_INET, send_ip, &udp_addr.sin_addr); 

    // Send the message over UDP
    if (sendto(socket, &udp_message, sizeof(udp_message), 0,
            (struct sockaddr*)&udp_addr, sizeof(udp_addr)) < 0) {
        perror("Failed to send UDP message");
    } 
}

void add_request(struct lookup_request new_request) {
    // If the list is full, overwrite the oldest request
    if (request_count >= MAX_REQUESTS) {
        request_count = 0; // Reset to the start of the array
    }
    requests[request_count] = new_request;
    (request_count)++;
}

int has_request(uint16_t hash_id) {
    for (int i = 0;i < MAX_REQUESTS;i++) {
        if (requests[i].hash_id == hash_id) return 1;
    }
    return 0;
}

void find_and_write(uint16_t hash_id, char* ip, char* port) {
    for (int i = 0;i < MAX_REQUESTS;i++) {
        if (requests[i].hash_id == hash_id) {
            requests[i].node_ip = ip;
            requests[i].node_port = port;
            return;
        }
    }
}

int fetch_req_index(uint16_t hash_id) {
    for (int i = 0;i < MAX_REQUESTS;i++) {
        if (requests[i].hash_id == hash_id) return i;
    }
    return -1;
}
