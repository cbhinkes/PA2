/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here

# Student #1: Vedant Patel
# Student #2: Cole Hinkes

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client.
 */
typedef struct {
    int epoll_fd;         /* File descriptor for the epoll instance */
    int socket_fd;        /* UDP socket file descriptor */
    long tx_cnt;          /* Transmitted packet count */
    long rx_cnt;          /* Received packet count */
} client_thread_data_t;

/*
 * This function runs in a separate client thread to handle communication with the server using UDP.
 */
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP"; /* 16-byte message */
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);

    // Prepare server address structure for sendto/recvfrom.
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    // Register the UDP socket with epoll for incoming data.
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("epoll_ctl failed");
        pthread_exit(NULL);
    }

    // Initialize counters.
    data->tx_cnt = 0;
    data->rx_cnt = 0;

    // Send and receive messages until num_requests is reached.
    for (int i = 0; i < num_requests; i++) {
        gettimeofday(&start, NULL);

        // Send UDP packet to server.
        ssize_t sent_bytes = sendto(data->socket_fd, send_buf, MESSAGE_SIZE, 0,
                                    (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (sent_bytes != MESSAGE_SIZE) {
            perror("sendto failed");
            break;
        }
        data->tx_cnt++;

        // Wait for the server's echo response using epoll.
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 1);
        if (nfds < 0) {
            perror("epoll_wait failed");
            break;
        }
        for (int j = 0; j < nfds; j++) {
            if (events[j].events & EPOLLIN) {
                ssize_t recvd_bytes = recvfrom(data->socket_fd, recv_buf, MESSAGE_SIZE, 0,
                                               (struct sockaddr *)&server_addr, &addr_len);
                if (recvd_bytes < 0) {
                    perror("recvfrom failed");
                    break;
                }
                if (recvd_bytes == MESSAGE_SIZE) {
                    data->rx_cnt++;
                }
            }
        }

    }
    return NULL;
}

/*
 * This function orchestrates multiple client threads: creating UDP sockets,
 * connecting to the server, and aggregating performance metrics.
 */
void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];

    // Create client UDP sockets.
    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (thread_data[i].socket_fd < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd < 0) {
            perror("epoll_create1 failed");
            exit(EXIT_FAILURE);
        }
        
        thread_data[i].tx_cnt = 0;
        thread_data[i].rx_cnt = 0;
    }

    // Create client threads.
    for (int i = 0; i < num_client_threads; i++) {
        if (pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]) != 0) {
            perror("pthread_create failed");
            exit(EXIT_FAILURE);
        }
    }

    long total_tx_cnt = 0;
    long total_rx_cnt = 0;

    // Wait for threads to finish and aggregate metrics.
    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_tx_cnt += thread_data[i].tx_cnt;
        total_rx_cnt += thread_data[i].rx_cnt;

        close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }

    long lost_pkt_cnt = total_tx_cnt - total_rx_cnt;
    printf("Total Sent: %ld, Total Received: %ld, Lost Packets: %ld\n", total_tx_cnt, total_rx_cnt, lost_pkt_cnt);
}


void run_server() {
    int server_fd, epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    struct epoll_event event, events[MAX_EVENTS];
    socklen_t client_len = sizeof(client_addr);

    // Create a UDP socket.
    server_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_fd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Bind the socket to INADDR_ANY and the specified port.
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Create an epoll instance.
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Register the UDP socket with epoll for incoming data.
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        perror("epoll_ctl failed");
        close(server_fd);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }

    // Server run-to-completion event loop.
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            perror("epoll_wait failed");
            break;
        }
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd && (events[i].events & EPOLLIN)) {
                char buffer[MESSAGE_SIZE];
                // Use recvfrom to get the packet and record the client's address.
                ssize_t bytes_received = recvfrom(server_fd, buffer, MESSAGE_SIZE, 0,
                                                  (struct sockaddr *)&client_addr, &client_len);
                if (bytes_received < 0) {
                    perror("recvfrom failed");
                    continue;
                }
                // Echo the packet back to the client.
                ssize_t sent_bytes = sendto(server_fd, buffer, bytes_received, 0,
                                            (struct sockaddr *)&client_addr, client_len);
                if (sent_bytes < 0) {
                    perror("sendto failed");
                }
            }
        }
    }

    // Cleanup
    close(server_fd);
    close(epoll_fd);
}


int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}