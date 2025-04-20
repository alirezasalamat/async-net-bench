/* ======================================
   client.c
   Connects to 127.0.0.1:12345, then for each
   size (1 KB, 1 MB) runs a 30-second loop:
     - connect
     - send 4-byte size
     - recv full payload
     - measure RTT per iteration
   At the end prints:
     iterations | avg latency (ms) | throughput (MiB/s)
   ====================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <pthread.h>

#define PORT 12345
#define SERVER_IP "127.0.0.1"

uint64_t difftimespec_ns(const struct timespec after, const struct timespec before)
{
    uint64_t elapsed_time_ns = ((uint64_t)after.tv_sec - (uint64_t)before.tv_sec) * (uint64_t)1000000000ULL
         + ((uint64_t)after.tv_nsec - (uint64_t)before.tv_nsec);
    if (elapsed_time_ns < 0) {
        fprintf(stderr, "Negative elapsed time: %ld\n", elapsed_time_ns);
        return 0;
    }
    return elapsed_time_ns;
}

uint64_t do_request(int tid, int sock, int size, uint64_t *total_bytes) {
    
    printf("[client-thread-%d] Allocating buffer of %d bytes\n", tid, size);
    char *buf;
    buf = malloc(size * sizeof buf);
    memset(buf, 0, size);

    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
    
    printf("[client-thread-%d] Sending request header with size %d bytes\n", tid, size);
    int network_number = htonl(size);
    ssize_t s = send(sock, &network_number, sizeof(network_number), 0);
    printf("[client-thread-%d] Successfully sent %ld bytes header\n", tid, s);
    if(s <= 0) {
        fprintf(stderr, "send error: %s\n", strerror(errno));
        free(buf);
        return 0;
    }


    printf("[client-thread-%d] Starting to receive %d bytes payload\n", tid, size);
    size_t bytes_received = 0;
    while (bytes_received < size) {
        int remaining = size - bytes_received;
        int r = recv(sock, buf + bytes_received, remaining, 0);
        if (r <= 0) {
            if (r == 0) {
                fprintf(stderr, "[client-thread-%d] Connection closed by server\n", tid);
            } else {
                fprintf(stderr, "[client-thread-%d] recv error: %s\n", tid, strerror(errno));
            }
            free(buf);
            return 0;
        }
        bytes_received += r;
        printf("[client-thread-%d] Received chunk of %d bytes (%zu/%d total)\n", 
               tid, r, bytes_received, size);
    }
    printf("[client-thread-%d] Successfully received all %zu bytes\n", tid, bytes_received);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    uint64_t ns = difftimespec_ns(t1, t0);
    *total_bytes += bytes_received;
    free(buf);
    return ns;
}

struct thread_args {
    int thread_id;
    int size1;
    int size2;
    int duration_sec;
    uint64_t total_bytes;
    uint64_t total_ns;
    uint64_t iterations;
};

void* thread_func(void* arg) {
    struct thread_args* args = (struct thread_args*)arg;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); pthread_exit(NULL); }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); pthread_exit(NULL);
    }
    printf("[client-thread-%d] connected to %s:%d\n", args->thread_id, SERVER_IP, PORT);

    int64_t duration_sec_ns = args->duration_sec * 1000000000ULL;
    while (1) {
        clock_gettime(CLOCK_MONOTONIC_RAW, &now);
        if (difftimespec_ns(now, start) >= duration_sec_ns) {
            printf("[client-thread-%d] duration exceeded, exiting...\n", args->thread_id);
            break;
        }

        uint64_t ns = do_request(args->thread_id, sock, args->size1, &args->total_bytes);
        args->total_ns += ns;
        ns = do_request(args->thread_id, sock, args->size2, &args->total_bytes);
        args->total_ns += ns;
        args->iterations++;
    }
    close(sock);
    return NULL;
}

void run_test(int size1, int size2, int duration_sec, int num_threads) {
    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    struct thread_args* args = malloc(num_threads * sizeof(struct thread_args));
    
    printf("[client] starting %d threads for %d seconds...\n", num_threads, duration_sec);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].size1 = size1;
        args[i].size2 = size2;
        args[i].duration_sec = duration_sec;
        args[i].total_bytes = 0;
        args[i].total_ns = 0;
        args[i].iterations = 0;
        
        if (pthread_create(&threads[i], NULL, thread_func, &args[i]) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    uint64_t total_bytes = 0;
    uint64_t total_ns = 0;
    uint64_t total_iterations = 0;

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    double duration_sec_executed = (end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;

    for (int i = 0; i < num_threads; i++) {
        double avg_latency_ms = (double)args[i].total_ns / args[i].iterations / 1e6;
        double throughput_mib = (double)args[i].total_bytes / (1024.0*1024.0) / duration_sec_executed;
        printf("\n");
        printf("━━━━━━━━━━━━━━━━━━━━━━ Client Thread %d Statistics ━━━━━━━━━━━━━━━━━━━━━━\n", i);
        printf("[client-thread-%d] Completed iterations: %ld\n", i, args[i].iterations);
        printf("[client-thread-%d] Average latency: %.3f ms\n", i, avg_latency_ms);
        printf("[client-thread-%d] Throughput: %.3f MiB/s\n", i, throughput_mib);
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");

        total_bytes += args[i].total_bytes;
        total_ns += args[i].total_ns;
        total_iterations += args[i].iterations;
    }

    double avg_latency_ms = (double)total_ns / total_iterations / 1e6;
    double throughput_mib = (double)total_bytes / (1024.0*1024.0) / duration_sec;

    printf("\n");
    printf("📊 Final Benchmark Results 📊\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("[client] Configuration:\n");
    printf("[client] - Threads: %d\n", num_threads);
    printf("[client] - Message sizes: %d bytes, %d bytes\n", size1, size2);
    printf("[client] - Duration: %d seconds\n", duration_sec);
    printf("\n");
    printf("[client] Performance Metrics:\n");
    printf("[client] - Total duration sec executed: %.3f secs\n", duration_sec_executed);
    printf("[client] - Total data transferred: %.2f MiB\n", (double)total_bytes / (1024.0*1024.0));
    printf("[client] - Total iterations: %ld\n", total_iterations);
    printf("[client] - Average latency: %.3f ms\n", avg_latency_ms);
    printf("[client] - Aggregate throughput: %.3f MiB/s\n", throughput_mib);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    free(threads);
    free(args);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <duration_sec> <size_1> <size_2> <num_threads>\n", argv[0]);
        return 1;
    }
    
    int duration_sec = atoi(argv[1]);
    int size_1 = atoi(argv[2]);
    int size_2 = atoi(argv[3]);
    int num_threads = atoi(argv[4]);

    if (duration_sec <= 0 || size_1 <= 0 || size_2 <= 0 || num_threads <= 0) {
        fprintf(stderr, "Invalid arguments\n");
        return 1;
    }

    printf("Running test with %d threads for %d seconds with sizes %d KB and %d KB\n", 
           num_threads, duration_sec, size_1, size_2);

    run_test(size_1 * 1024, size_2 * 1024, duration_sec, num_threads);
    return 0;
}