#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGE_SIZE (2 * 1024 * 1024) // 2MB

// Structure to hold remote memory region information
struct mr_info
{
	uintptr_t remote_addr;
	uint32_t rkey;
	size_t mem_size; // used for client request allocation
};

// Global variables
struct sockaddr_in addr;
struct rdma_cm_id *listener = NULL, *conn = NULL;
struct rdma_event_channel *ec = NULL;
struct ibv_pd *pd;
struct ibv_mr *mr;
struct ibv_cq *cq;
struct ibv_qp_init_attr qp_attr;
char *buffer;
uint64_t client_addr;
uint32_t client_rkey;
size_t buffer_size;

// Function to post a receive work request
void
post_receive()
{
	printf("Posting a receive WR...\n");
	struct ibv_recv_wr recv_wr, *bad_recv_wr = NULL;
	struct ibv_sge recv_sge;
	memset(&recv_wr, 0, sizeof(recv_wr));
	recv_wr.wr_id = 1;
	recv_sge.addr = (uintptr_t)buffer;
	recv_sge.length = buffer_size;
	recv_sge.lkey = mr->lkey;
	recv_wr.sg_list = &recv_sge;
	recv_wr.num_sge = 1;
	if (ibv_post_recv(conn->qp, &recv_wr, &bad_recv_wr))
	{
		perror("ibv_post_recv");
		exit(1);
	}
}

void
init_buffer()
{
	int i;
	char str[100];

	for (i = 0; i < buffer_size / PAGE_SIZE; i++)
	{
		snprintf(str, sizeof(str), "Page [%d]", i);
		strcpy(buffer + i * PAGE_SIZE, str);
		printf("set %s\n", str);
	}
}

// Main loop to handle client requests and send responses
void
main_loop(uint64_t client_addr, uint32_t client_rkey)
{
	struct ibv_wc wc;

	while (1)
	{
		// printf("Waiting for client request...\n");
		post_receive();

		while (ibv_poll_cq(cq, 1, &wc) < 1)
		{
		}
		if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM)
		{
			fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
			        ibv_wc_status_str(wc.status), wc.status, (int)wc.wr_id);
			exit(1);
		}

		// printf("Received request: %s\n", buffer);

	}
}

int
main(int argc, char **argv)
{
	// Initialize server address
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(5000);
	inet_pton(AF_INET, "10.10.10.221", &addr.sin_addr);

	// Create event channel
	printf("Creating event channel...\n");
	ec = rdma_create_event_channel();
	if (!ec)
	{
		perror("rdma_create_event_channel");
		return 1;
	}

	// Create RDMA ID for listening
	printf("Creating RDMA ID...\n");
	if (rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP))
	{
		perror("rdma_create_id");
		return 1;
	}

	// Bind address to RDMA ID
	printf("Binding address...\n");
	if (rdma_bind_addr(listener, (struct sockaddr *)&addr))
	{
		perror("rdma_bind_addr");
		return 1;
	}

	// Start listening for incoming connections
	printf("Listening...\n");
	if (rdma_listen(listener, 10))
	{
		perror("rdma_listen");
		return 1;
	}

	printf("Server is listening at %s:5000\n", "10.10.10.221");

	// Accept incoming connection and process client requests
	printf("Waiting for connection...\n");
	struct rdma_cm_event *event;
	if (rdma_get_cm_event(ec, &event))
	{
		perror("rdma_get_cm_event");
		return 1;
	}

	if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST)
	{
		fprintf(stderr, "Unexpected event: %s\n", rdma_event_str(event->event));
		return 1;
	}
	else
	{
		struct mr_info *client_mr = (struct mr_info *)event->param.conn.private_data;
		if (client_mr == NULL)
		{
			fprintf(stderr, "Private data is NULL\n");
			return 1;
		}
		memcpy(&client_addr, &client_mr->remote_addr, sizeof(client_addr));
		memcpy(&client_rkey, &client_mr->rkey, sizeof(client_rkey));
		memcpy(&buffer_size, &client_mr->mem_size, sizeof(buffer_size));

		printf("client_addr: %lx\n", client_addr);
		printf("client_rkey: %u\n", client_rkey);
		printf("buffer_size: %lx\n", buffer_size);
	}
	conn = event->id;
	rdma_ack_cm_event(event);

	// Allocate Protection Domain
	printf("Allocating PD...\n");
	pd = ibv_alloc_pd(conn->verbs);
	if (!pd)
	{
		perror("ibv_alloc_pd");
		return 1;
	}

	// Allocate buffer using huge pages and register memory
	printf("Allocating buffer and registering memory...\n");
	buffer = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (buffer == MAP_FAILED)
	{
		perror("mmap");
		return 1;
	}
	memset(buffer, 0, buffer_size);
	mr = ibv_reg_mr(pd, buffer, buffer_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	printf("key: %u\n", mr->rkey);
	printf("addr: %lx\n", (uintptr_t)buffer);
	init_buffer();

	// Create completion queue
	printf("Creating completion queue...\n");
	cq = ibv_create_cq(conn->verbs, 10, NULL, NULL, 0);

	// Create queue pair
	printf("Creating queue pair...\n");
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

	// Accept RDMA connection
	printf("Accepting RDMA connection...\n");
	struct rdma_conn_param cm_params = {0};
	struct mr_info mr_info = {(uintptr_t)buffer, mr->rkey};
	cm_params.private_data = &mr_info;
	cm_params.private_data_len = sizeof(mr_info);
	cm_params.responder_resources = 1;
	cm_params.initiator_depth = 1;
	if (rdma_accept(conn, &cm_params))
	{
		perror("rdma_accept");
		return 1;
	}

	// Get CM event
	printf("Getting CM event...\n");
	if (rdma_get_cm_event(ec, &event))
	{
		perror("rdma_get_cm_event");
		return 1;
	}

	if (event->event != RDMA_CM_EVENT_ESTABLISHED)
	{
		fprintf(stderr, "Unexpected event: %s\n", rdma_event_str(event->event));
		return 1;
	}
	rdma_ack_cm_event(event);

	// Enter main loop to handle client requests
	main_loop(client_addr, client_rkey);

	// Clean up connection-specific resources
	ibv_destroy_qp(conn->qp);
	ibv_dereg_mr(mr);
	munmap(buffer, buffer_size);
	rdma_destroy_id(conn);

	// Clean up listener resources
	rdma_destroy_id(listener);
	rdma_destroy_event_channel(ec);

	printf("Server finished successfully.\n");
	return 0;
}
