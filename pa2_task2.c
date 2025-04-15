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
#define TIMEOUT_MS 1

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
    int client_id;
    int epoll_fd;
    int socket_fd;
    long tx_cnt;
    long rx_cnt;
    long long total_rtt;
    int seq_num;
} client_thread_data_t;

void make_frame(char *buf, int client_id, int seq_num) {
    memcpy(buf, &client_id, 4);
    memcpy(buf + 4, &seq_num, 4);
    memcpy(buf + 8, "ABCDEFGH", 8);
}

int extract_seq_num(char *buf) {
    int seq;
    memcpy(&seq, buf + 4, 4);
    return seq;
}

int extract_client_id(char *buf) {
    int cid;
    memcpy(&cid, buf, 4);
    return cid;
}

void *client_thread_func(void *arg) {

    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE];
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;
    struct sockaddr_in server_addr;

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;

    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("epoll_ctl failed");
        pthread_exit(NULL);
    }

    for (int i = 0; i < num_requests;) {
        make_frame(send_buf, data->client_id, data->seq_num);
        gettimeofday(&start, NULL);
        sendto(data->socket_fd, send_buf, MESSAGE_SIZE, 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
        data->tx_cnt++;
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, TIMEOUT_MS);
        if (nfds <= 0) continue;
        for (int j = 0; j < nfds; j++) {
            socklen_t addr_len = sizeof(server_addr);
            ssize_t bytes = recvfrom(data->socket_fd, recv_buf, MESSAGE_SIZE, 0, (struct sockaddr *)&server_addr, &addr_len);
            if (bytes > 0) {
                int ack_seq = extract_seq_num(recv_buf);
                int ack_id = extract_client_id(recv_buf);
                if (ack_id == data->client_id && ack_seq == data->seq_num) {
                    gettimeofday(&end, NULL);
                    long long rtt = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
                    data->total_rtt += rtt;
                    data->rx_cnt++;
                    data->seq_num++;
                    i++;
                }
            }
        }
    }
    return NULL;
}

void run_client() {

    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];

    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
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
        thread_data[i].total_rtt = 0;
        thread_data[i].client_id = i;
        thread_data[i].seq_num = 0;
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    long total_tx = 0, total_rx = 0;
    long long total_rtt = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_rtt += thread_data[i].total_rtt;
        close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }

    printf("Total Sent: %ld\n", total_tx);
    printf("Total Received: %ld\n", total_rx);
    printf("Lost Packets: %ld\n", total_tx - total_rx);
    printf("Average RTT: %lld us\n", total_rtt / total_rx);
}

void run_server() {

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr, client_addr;
    struct epoll_event event, events[MAX_EVENTS];

    if (sockfd < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    event.events = EPOLLIN;
    event.data.fd = sockfd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sockfd, &event);

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            perror("epoll_wait failed");
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == sockfd) {
                char buf[MESSAGE_SIZE];
                socklen_t len = sizeof(client_addr);
                ssize_t bytes = recvfrom(sockfd, buf, MESSAGE_SIZE, 0, (struct sockaddr *)&client_addr, &len);
                if (bytes > 0) {
                    sendto(sockfd, buf, bytes, 0, (struct sockaddr *)&client_addr, len);
                }
            }
        }
    }

    close(sockfd);
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