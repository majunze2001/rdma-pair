#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h> // for clock_gettime
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h> // Add this line

// BUFFER_SIZE ALL ACROSS
#define BUFFER_SIZE (2 * 1024 * 1024 + 4 * 1024) // 2MB + 4KB

#define SET_BUFFER 0x12345678 // also sets PID as current
#define FAULT_HANDLED 0x12345679

struct rdma_cm_id *conn = NULL;
struct ibv_pd *pd;
struct ibv_mr *mr;
struct ibv_cq *cq;
char *buffer;
int fd;
int ret;
// Global mutex for protecting critical sections
pthread_mutex_t send_receive_mutex = PTHREAD_MUTEX_INITIALIZER;
// Atomic flag to check if send_request_and_receive_response is in progress
atomic_bool send_receive_in_progress = false;


#define PROFILE

#ifdef PROFILE
FILE *log_file = NULL; // Global file descriptor
#endif

void
send_request_and_receive_response()
{
#ifdef PROFILE
	struct timespec start_time, end_time, time1, time2, time3, time4, time5;
	clock_gettime(CLOCK_MONOTONIC, &start_time);
#endif
	pthread_mutex_lock(&send_receive_mutex);
	// Set the atomic flag
	atomic_store(&send_receive_in_progress, true);
#ifdef PROFILE
	clock_gettime(CLOCK_MONOTONIC, &time1); // lock
#endif
	// Post send request
	struct ibv_send_wr send_wr, *bad_send_wr = NULL;
	struct ibv_sge send_sge;
	memset(&send_wr, 0, sizeof(send_wr));
	send_wr.wr_id = 1;
	send_wr.opcode = IBV_WR_SEND;
	send_wr.send_flags = IBV_SEND_SIGNALED;
	send_sge.addr = (uintptr_t)buffer;
	send_sge.length = strlen(buffer);
	// send_sge.length = BUFFER_SIZE;
	send_sge.lkey = mr->lkey;
	send_wr.sg_list = &send_sge;
	send_wr.num_sge = 1;
	if (ibv_post_send(conn->qp, &send_wr, &bad_send_wr))
	{
		perror("ibv_post_send");
		exit(1);
	}
#ifdef PROFILE
	clock_gettime(CLOCK_MONOTONIC, &time2); // Post send request
#endif
	// Wait for send completion
	struct ibv_wc wc;
	while (ibv_poll_cq(cq, 1, &wc) < 1)
	{
	}
	if (wc.status != IBV_WC_SUCCESS)
	{
		fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
		        ibv_wc_status_str(wc.status), wc.status, (int)wc.wr_id);
		exit(1);
	}
#ifdef PROFILE
	clock_gettime(CLOCK_MONOTONIC, &time3); // Wait for send completion
#endif
	// Post receive request
	struct ibv_recv_wr recv_wr, *bad_recv_wr = NULL;
	struct ibv_sge recv_sge;
	memset(&recv_wr, 0, sizeof(recv_wr));
	recv_wr.wr_id = 2;
	recv_sge.addr = (uintptr_t)buffer;
	recv_sge.length = BUFFER_SIZE;
	recv_sge.lkey = mr->lkey;
	recv_wr.sg_list = &recv_sge;
	recv_wr.num_sge = 1;
	if (ibv_post_recv(conn->qp, &recv_wr, &bad_recv_wr))
	{
		perror("ibv_post_recv");
		exit(1);
	}
#ifdef PROFILE
	clock_gettime(CLOCK_MONOTONIC, &time4); // Post receive request
#endif
	// Wait for receive completion
	while (ibv_poll_cq(cq, 1, &wc) < 1)
	{
	}
	if (wc.status != IBV_WC_SUCCESS)
	{
		fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
		        ibv_wc_status_str(wc.status), wc.status, (int)wc.wr_id);
		exit(1);
	}

#ifdef PROFILE
	clock_gettime(CLOCK_MONOTONIC, &time5); // Wait for receive completion
#endif

	// Clear the atomic flag
	atomic_store(&send_receive_in_progress, false);
	pthread_mutex_unlock(&send_receive_mutex);

#ifdef PROFILE
	clock_gettime(CLOCK_MONOTONIC, &end_time); // unlock
#endif

#ifdef PROFILE
	// log
	long total_time = (end_time.tv_sec - start_time.tv_sec) * 1e9 +
	                  (end_time.tv_nsec - start_time.tv_nsec);

	long lock_time = (time1.tv_sec - start_time.tv_sec) * 1e9 +
	                 (time1.tv_nsec - start_time.tv_nsec);

	long ps_time = (time2.tv_sec - time1.tv_sec) * 1e9 +
	               (time2.tv_nsec - time1.tv_nsec);

	long ws_time = (time3.tv_sec - time2.tv_sec) * 1e9 +
	               (time3.tv_nsec - time2.tv_nsec);

	long pr_time = (time4.tv_sec - time3.tv_sec) * 1e9 +
	               (time4.tv_nsec - time3.tv_nsec);

	long wr_time = (time5.tv_sec - time4.tv_sec) * 1e9 +
	               (time5.tv_nsec - time4.tv_nsec);

	long unlock_time = (end_time.tv_sec - time5.tv_sec) * 1e9 +
	                   (end_time.tv_nsec - time5.tv_nsec);

	fprintf(log_file, "total_time %ld\n", total_time);
	fprintf(log_file, "lock_time %ld\n", lock_time);
	fprintf(log_file, "ps_time %ld\n", ps_time);
	fprintf(log_file, "ws_time %ld\n", ws_time);
	fprintf(log_file, "pr_time %ld\n", pr_time);
	fprintf(log_file, "wr_time %ld\n", wr_time);
	fprintf(log_file, "unlock_time %ld\n", unlock_time);
	fflush(log_file); // Ensure it's written immediately

	printf("Received response from server: %s\n", buffer);
#endif
}

void
sigint_handler(int signum)
{
	printf("SIGINT received. Sending request to server...\n");
	send_request_and_receive_response();
}

void
sigio_handler(int sig)
{
	if (!((uint64_t *)buffer)[0])
	{
		printf("Invalid address -- sigio\n"); // read_buffer
	}

	// Check if send_request_and_receive_response is in progress
	if (!atomic_load(&send_receive_in_progress))
	{
		send_request_and_receive_response();
	}

	ret = ioctl(fd, FAULT_HANDLED);
	if (ret == -1)
	{
		printf("FAULT_HANDLED failed\n");
	}
	printf("sigio_handler done\n");
}

int
main(int argc, char **argv)
{
	struct sockaddr_in addr;
	struct rdma_event_channel *ec = NULL;
	struct ibv_qp_init_attr qp_attr;

	signal(SIGINT, sigint_handler);
	signal(SIGIO, sigio_handler);

#ifdef PROFILE
	log_file = fopen("timing_log.txt", "a");
	if (!log_file)
	{
		perror("Failed to open log file");
		return 1;
	}
#endif

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(5000);
	inet_pton(AF_INET, "10.10.10.221", &addr.sin_addr);

	printf("Creating event channel...\n");
	ec = rdma_create_event_channel();
	if (!ec)
	{
		perror("rdma_create_event_channel");
		return 1;
	}

	printf("Creating RDMA ID...\n");
	if (rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP))
	{
		perror("rdma_create_id");
		return 1;
	}

	printf("Resolving address...\n");
	if (rdma_resolve_addr(conn, NULL, (struct sockaddr *)&addr, 2000))
	{
		perror("rdma_resolve_addr");
		return 1;
	}

	printf("Getting CM event...\n");
	struct rdma_cm_event *event;
	if (rdma_get_cm_event(ec, &event))
	{
		perror("rdma_get_cm_event");
		return 1;
	}
	rdma_ack_cm_event(event);

	printf("Resolving route...\n");
	if (rdma_resolve_route(conn, 2000))
	{
		perror("rdma_resolve_route");
		return 1;
	}

	printf("Getting CM event...\n");
	if (rdma_get_cm_event(ec, &event))
	{
		perror("rdma_get_cm_event");
		return 1;
	}
	rdma_ack_cm_event(event);

	// Allocate Protection Domain
	printf("Allocating PD...\n");
	pd = ibv_alloc_pd(conn->verbs);
	if (!pd)
	{
		perror("ibv_alloc_pd");
		return 1;
	}

	// Allocate buffer using huge pages
	printf("Allocating buffer...\n");
	buffer = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (buffer == MAP_FAILED)
	{
		perror("mmap");
		return 1;
	}

	memset(buffer, 0, BUFFER_SIZE);
	strcpy(buffer, "0x5421DEF000");
	mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	if (!mr)
	{
		perror("ibv_reg_mr");
		return 1;
	}

	printf("Creating CQ...\n");
	cq = ibv_create_cq(conn->verbs, 10, NULL, NULL, 0);

	printf("Creating QP...\n");
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.send_cq = cq;
	qp_attr.recv_cq = cq;
	qp_attr.cap.max_send_wr = 10;
	qp_attr.cap.max_recv_wr = 10;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;
	if (rdma_create_qp(conn, pd, &qp_attr))
	{
		perror("rdma_create_qp");
		return 1;
	}

	printf("Connecting...\n");
	struct rdma_conn_param cm_params = {0};
	if (rdma_connect(conn, &cm_params))
	{
		perror("rdma_connect");
		return 1;
	}

	printf("Getting CM event...\n");
	if (rdma_get_cm_event(ec, &event))
	{
		perror("rdma_get_cm_event");
		return 1;
	}
	rdma_ack_cm_event(event);

	printf("%s\n", buffer);
	fd = open("/dev/nvidia-uvm", O_RDWR);
	if (fd == -1)
	{
		printf("uvm open failed\n");
		return -1;
	}

	ret = ioctl(fd, SET_BUFFER, buffer);
	if (ret < 0)
	{
		printf("SET_BUFFER failed\n");
		return -1;
	}
	while (1)
	{
		pause(); // Wait for a signal to be caught
	}

	// Clean up
	printf("Cleaning up...\n");
	ibv_destroy_qp(conn->qp);
	ibv_destroy_cq(cq);
	ibv_dereg_mr(mr);
	munmap(buffer, BUFFER_SIZE);
	rdma_destroy_id(conn);
	rdma_destroy_event_channel(ec);
	pthread_mutex_destroy(&send_receive_mutex);

	printf("Client finished successfully.\n");
#ifdef PROFILE
	fclose(log_file);
#endif
	return 0;
}
