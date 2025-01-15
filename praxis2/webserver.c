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
unsigned long pred_id = 0, succ_id = 0;
int have_pred = 0, have_succ = 0;
char pred_ip[INET_ADDRSTRLEN] = {0}, succ_ip[INET_ADDRSTRLEN] = {0};
char *PRED_ID, *PRED_IP, *PRED_PORT, *SUCC_ID, *SUCC_IP, *SUCC_PORT;
char *IP, *PORT, *NODEID;
int server_socket, datagram_socket;


int has_request(uint16_t hash_id);
int fetch_req_index(uint16_t hash_id);
void add_request(struct lookup_request new_request);
void send_udp_message(int socket, uint8_t message_type, uint16_t hash_id, uint16_t node_id, const char *ip_address, uint16_t node_port, struct in_addr send_ip, uint16_t send_port);
static struct sockaddr_in derive_sockaddr(const char *host, const char *port);
void send_reply(int conn, struct request *request, int udp_socket);
void send_reply(int conn, struct request *request, int udp_socket) {
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

    // Inline helper functions
    int has_request(uint16_t hash_id) {
        for (int i = 0; i < MAX_REQUESTS; i++) {
            if (requests[i].hash_id == hash_id) return 1;
        }
        return 0;
    }

    int fetch_req_index(uint16_t hash_id) {
        for (int i = 0; i < MAX_REQUESTS; i++) {
            if (requests[i].hash_id == hash_id) return i;
        }
        return -1;
    }

    // Buffer to store the reply
    char buffer[HTTP_MAX_SIZE];
    char *reply = buffer;  // Ensure this is modifiable, not 'const char *'
    size_t offset = 0;

    if (strcmp(request->method, "GET") == 0) {
        size_t resource_length;
        const char *resource = get(uri_hash_string, resources, MAX_RESOURCES, &resource_length);

        if (resource) {
            // Resource found
            offset = snprintf(reply, HTTP_MAX_SIZE, OK_REPLY_FORMAT, resource_length);
            memcpy(reply + offset, resource, resource_length);
            offset += resource_length;
        } else {
            // Resource not found
            if (strcmp(PRED_ID, SUCC_ID) == 0 && uri_hash != atoi(NODEID)) {
                reply = (char *)NOT_FOUND_REPLY;  // Cast to 'char *' explicitly
            } else {
                if (!has_request(uri_hash)) {
                    // Request not in progress, send service unavailable
                    reply = (char *)SERVICE_UNAVAILABLE_REPLY;  // Cast to 'char *' explicitly

                    struct sockaddr_in udp_addr = derive_sockaddr(SUCC_IP, SUCC_PORT);
                    send_udp_message(udp_socket, 0, htons(uri_hash), htons(atoi(NODEID)), IP, htons(atoi(PORT)), udp_addr.sin_addr, udp_addr.sin_port);

                    // Inline add_request logic
                    static int request_count = 0;  // Ensure this is declared
                    struct lookup_request new_request = {uri_hash, NULL, 0};
                    if (request_count >= MAX_REQUESTS) {
                        request_count = 0;  // Reset to the start of the array
                    }
                    requests[request_count] = new_request;
                    request_count++;
                } else {
                    // Request in progress, check if we have an answer
                    int index = fetch_req_index(uri_hash);
                    if (index == -1 || requests[index].node_ip == NULL || requests[index].node_port == 0)
 {
                        reply = (char *)NOT_FOUND_REPLY;  // Cast to 'char *' explicitly
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
            reply = (char *)NO_CONTENT_REPLY;  // Cast to 'char *' explicitly
        } else {
            reply = (char *)CREATED_REPLY;  // Cast to 'char *' explicitly
        }
        offset = strlen(reply);
    } else if (strcmp(request->method, "DELETE") == 0) {
        if (delete(uri_hash_string, resources, MAX_RESOURCES)) {
            reply = (char *)NO_CONTENT_REPLY;  // Cast to 'char *' explicitly
        } else {
            reply = (char *)NOT_FOUND_REPLY;  // Cast to 'char *' explicitly
        }
        offset = strlen(reply);
    } else {
        // Unsupported method
        reply = (char *)METHOD_NOT_SUPPORTED_REPLY;  // Cast to 'char *' explicitly
        offset = strlen(reply);
    }

    // Send the reply back to the client
    if (send(conn, reply, offset, 0) == -1) {
        perror("send");
        close(conn);
    }
}


static int setup_datagram_socket(const char *host, const char *port);
static int setup_server_socket(struct sockaddr_in addr);
static void connection_setup(struct connection_state *state, int sock);
bool handle_connection(struct connection_state *state, int udp_socket);
// Function prototypes
void handle_tcp_connection(struct connection_state *state, int server_socket, struct pollfd *sockets);
void handle_udp_event(int datagram_socket);
void reset_polling(struct pollfd *sockets, int datagram_socket);
void process_udp_message(struct message *received_msg, int datagram_socket);
void handle_successor_or_current(struct message *received_msg, int datagram_socket);
void handle_reply(struct message *received_msg);
void forward_message(struct message *received_msg, int datagram_socket);

static struct sockaddr_in derive_sockaddr(const char *host, const char *port);
void send_udp_message(int socket, uint8_t message_type, uint16_t hash_id, uint16_t node_id, const char *ip_address, uint16_t node_port, struct in_addr send_ip, uint16_t send_port);

int main(int argc, char **argv) 
{
    // Retrieve environment variables (setting default values where necessary)
    char *env;
    PRED_ID = getenv("PRED_ID");
    PRED_IP = getenv("PRED_IP");
    PRED_PORT = getenv("PRED_PORT");
    SUCC_ID = getenv("SUCC_ID");
    SUCC_IP = getenv("SUCC_IP");
    SUCC_PORT = getenv("SUCC_PORT");

    if (argc > 3) 
        NODEID = argv[3];
    else 
        NODEID = "0"; 

    IP = argv[1];
    PORT = argv[2];

    // Set values from environment variables
    if ((env = getenv("PRED_ID")) != NULL) { pred_id = safe_strtoul(env, NULL, 10, "Invalid PRED_ID"); have_pred = 1; }
    if ((env = getenv("PRED_IP")) != NULL) { strncpy(pred_ip, env, INET_ADDRSTRLEN - 1); }
    if ((env = getenv("SUCC_ID")) != NULL) { succ_id = safe_strtoul(env, NULL, 10, "Invalid SUCC_ID"); have_succ = 1; }
    if ((env = getenv("SUCC_IP")) != NULL) { strncpy(succ_ip, env, INET_ADDRSTRLEN - 1); }

    struct sockaddr_in addr = derive_sockaddr(argv[1], argv[2]);
    memset(requests, 0, sizeof(requests));

    // Set up server and datagram sockets
    struct sockaddr_in socketaddr = derive_sockaddr(argv[1], argv[2]);
    int server_socket = setup_server_socket(socketaddr);
    int datagram_socket = setup_datagram_socket(argv[1], argv[2]);

    // Monitor sockets using poll
    struct pollfd sockets[2] = 
    {
        {.fd = server_socket, .events = POLLIN},
        {.fd = datagram_socket, .events = POLLIN},
    };

    struct connection_state state = {0};

    while (true) 
    {
        int ready = poll(sockets, sizeof(sockets) / sizeof(sockets[0]), -1);
        if (ready == -1) 
        {
            perror("poll");
            exit(EXIT_FAILURE);
        }

        // Process events
        for (size_t i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) 
        {
            if (sockets[i].revents != POLLIN) 
            {
                continue;  // No event, continue to next socket
            }

            int s = sockets[i].fd;

            // Handle TCP connection
            if (s == server_socket) 
            {
                handle_tcp_connection(&state, server_socket, sockets);
            } 
            // Handle UDP event
            else if (s == datagram_socket) 
            {
                handle_udp_event(datagram_socket);
            } 
            else 
            {
                assert(s == state.sock);
                if (!handle_connection(&state, datagram_socket)) 
                {
                    reset_polling(sockets, datagram_socket);
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

// Handle TCP connections
void handle_tcp_connection(struct connection_state *state, int server_socket, struct pollfd *sockets) 
{
    printf("Handling TCP connection...\n");
    int connection = accept(server_socket, NULL, NULL);
    if (connection == -1 && errno != EAGAIN && errno != EWOULDBLOCK) 
    {
        close(server_socket);
        perror("accept");
        exit(EXIT_FAILURE);
    } 
    else 
    {
        connection_setup(state, connection);
        // one connection at a time
        sockets[0].events = 0;
        sockets[1].fd = connection;
        sockets[1].events = POLLIN;
    }
    printf("End TCP connection handling\n");
}

// Handle incoming UDP event
void handle_udp_event(int datagram_socket) 
{
    char buffer[1024];
    struct sockaddr_in senderAddr;
    socklen_t addr_len = sizeof(senderAddr);
    ssize_t bytenum = recvfrom(datagram_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&senderAddr, &addr_len);

    if (bytenum == -1) 
    {
        perror("recvfrom");
        return;
    }

    if (bytenum < sizeof(struct message)) 
    {
        fprintf(stderr, "Received message is too short to unpack\n");
        return;
    }

    struct message *received_msg = (struct message *)buffer;
    process_udp_message(received_msg, datagram_socket);
}

// Process the received UDP message
void process_udp_message(struct message *received_msg, int datagram_socket) 
{
    printf("Received UDP message:\n");
    printf("\tMessage Type: %u\n", received_msg->message_type);
    printf("\tPRED ID: %u\n", atoi(PRED_ID)); 
    printf("\tCURR ID: %u\n", atoi(NODEID)); 
    printf("\tHash ID: %u\n", ntohs(received_msg->hash_id));  
    printf("\tSUCC ID: %u\n", atoi(SUCC_ID)); 
    printf("\tNode ID: %u\n", ntohs(received_msg->node_id));  
    char ip_chr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &received_msg->ip_address, ip_chr, sizeof(ip_chr)); 
    printf("\tIP Address: %s\n", ip_chr);
    printf("\tNode Port: %u\n", ntohs(received_msg->node_port)); 
    printf("\tSUCC IP: %s\n", SUCC_IP);
    printf("\tSUCC Port: %s\n", SUCC_PORT); 

    if (received_msg->message_type == 0) 
    {
        handle_successor_or_current(received_msg, datagram_socket);
    } 
    else 
    {
        handle_reply(received_msg);
    }
}

// Handle messages for successor or current status
void handle_successor_or_current(struct message *received_msg, int datagram_socket) 
{
    if (ntohs(received_msg->hash_id) > atoi(NODEID) && ntohs(received_msg->hash_id) < atoi(SUCC_ID)) 
    {
        printf("Sending response to successor\n");
        send_udp_message(datagram_socket, 1, htons(atoi(NODEID)), htons(atoi(SUCC_ID)), SUCC_IP, htons(atoi(SUCC_PORT)), received_msg->ip_address, received_msg->node_port);
    } 
    else if (ntohs(received_msg->hash_id) < atoi(NODEID) && ntohs(received_msg->hash_id) > atoi(SUCC_ID)) 
    {
        send_udp_message(datagram_socket, 1, htons(atoi(PRED_ID)), htons(atoi(NODEID)), IP, htons(atoi(PORT)), received_msg->ip_address, received_msg->node_port);
    } 
    else 
    {
        forward_message(received_msg, datagram_socket);
    }
}

// Forward the message to the successor node
void forward_message(struct message *received_msg, int datagram_socket) 
{
    printf("Forwarding message to successor\n");

    // Initialize the successor's address structure
    struct sockaddr_in udpAddr;
    memset(&udpAddr, 0, sizeof(udpAddr));
    udpAddr.sin_family = AF_INET;

    // Convert and set the successor's port
    char *endptr;
    long port_value = strtol(SUCC_PORT, &endptr, 10);
    if (*endptr != '\0' || port_value <= 0 || port_value > 65535) {
        fprintf(stderr, "Invalid SUCC_PORT value: %s\n", SUCC_PORT);
        return;
    }
    udpAddr.sin_port = htons((uint16_t)port_value);

    // Convert and set the successor's IP address
    if (inet_pton(AF_INET, SUCC_IP, &udpAddr.sin_addr) != 1) {
        perror("Error converting successor IP address");
        return;
    }

    // Extract and convert the sender's IP address to a string
    char ipChar[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &received_msg->ip_address, ipChar, sizeof(ipChar)) == NULL) {
        perror("Error converting sender IP address");
        return;
    }

    // Send the message to the successor
    send_udp_message(
        datagram_socket,              // Datagram socket descriptor
        received_msg->message_type,   // Type of the message
        received_msg->hash_id,        // Hash ID associated with the message
        received_msg->node_id,        // ID of the node sending the message
        ipChar,                       // Sender's IP address in string form
        received_msg->node_port,      // Sender's port
        udpAddr.sin_addr,             // Successor's IP address
        udpAddr.sin_port              // Successor's port
    );
}


// Handle the reply message received via UDP
void handle_reply(struct message *received_msg) 
{
    printf("Received reply\n");
    int index = -1;
    for (int i = 0; i < MAX_REQUESTS; i++) 
    {
        if (requests[i].hash_id == received_msg->hash_id) 
        {
            index = i;
            break;
        }
    }
    if (index >= 0) 
    {
        printf("Request found, overwriting...\n");
        inet_ntop(AF_INET, &received_msg->ip_address, requests[index].node_ip, sizeof(requests[index].node_ip));
        requests[index].node_port = received_msg->node_port;
    } 
    else 
    {
        printf("Request not found\n");
        perror("Hash not found");
    }
}

// Reset polling state after handling a connection
void reset_polling(struct pollfd *sockets, int datagram_socket) 
{
    printf("Resetting polling\n");
    sockets[0].events = POLLIN;
    sockets[1].events = POLLIN;
    sockets[2].fd = -1;
    sockets[2].events = 0;
    if (sockets[1].fd != datagram_socket) 
    {
        sockets[1].fd = -1;
        sockets[1].events = 0;
    }
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
size_t process_packet(int conn, char *buffer, size_t n, int udp_socket);
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

char *buffer_discard(char *buffer, size_t discard, size_t keep);
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
    int rv, sock = -1;

    // Set up address info hints
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        // Use AF_INET for IPv4; change to AF_UNSPEC for IPv4/IPv6 support
    hints.ai_socktype = SOCK_DGRAM;   // Datagram socket

    // Resolve host and port
    if ((rv = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "Error: getaddrinfo failed - %s\n", gai_strerror(rv));
        return -1; // Return error instead of exiting
    }

    // Iterate through results to find a valid socket
    for (p = servinfo; p != NULL; p = p->ai_next) {
        // Attempt to create a socket
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == -1) {
            perror("Warning: Failed to create socket");
            continue;
        }

        // Configure socket options
        if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1) {
            perror("Error: setsockopt failed");
            close(sock);
            continue;
        }

        // Bind the socket to the address
        if (bind(sock, p->ai_addr, p->ai_addrlen) == -1) {
            perror("Warning: Failed to bind socket");
            close(sock);
            continue;
        }

        // Set non-blocking mode
        if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
            perror("Error: Failed to set non-blocking mode");
            close(sock);
            continue;
        }

        // Successfully created and configured socket
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "Error: Failed to create and bind socket\n");
        freeaddrinfo(servinfo);
        return -1;
    }

    freeaddrinfo(servinfo); // Clean up address info
    return sock;
}

void send_udp_message(int socket, uint8_t message_type, uint16_t hash_id, uint16_t node_id, const char *ip_address, uint16_t node_port, struct in_addr send_ip, uint16_t send_port) 
{
    struct message udp_message;

    // Initialize the UDP message fields
    udp_message = (struct message){
        .message_type = message_type,
        .hash_id = hash_id,
        .node_id = node_id,
        .ip_address = {0},
        .node_port = node_port
    };

    if (inet_pton(AF_INET, ip_address, &udp_message.ip_address) <= 0) {
        fprintf(stderr, "Error: Unable to parse IP address '%s'\n", ip_address);
        return;
    }

    // Configure the destination address for the UDP message
    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr)); // Zero out the structure

    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = send_port;
    udp_addr.sin_addr = send_ip;

    // Attempt to send the UDP message
    ssize_t bytes_sent = sendto(
        socket,
        &udp_message,
        sizeof(udp_message),
        0,
        (struct sockaddr*)&udp_addr,
        sizeof(udp_addr)
    );

    if (bytes_sent < 0) {
        perror("Error: Failed to send UDP message");
    }
}


void find_and_write(uint16_t hash_id, const char *ip, const char *port);
void find_and_write(uint16_t hash_id, const char *ip, const char *port) {
    for (int i = 0; i < MAX_REQUESTS; i++) {
        // Ensure that the array index is within bounds and hash_id is valid
        if (requests[i].hash_id == hash_id) {
            requests[i].node_ip = ip;
            requests[i].node_port = port;
            return;  // Found the matching request, so we exit early
        }
    }

    // If no match was found, optionally print an error or handle it
    fprintf(stderr, "Error: No matching request found for hash_id %u\n", hash_id);
}
