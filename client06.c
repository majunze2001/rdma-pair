#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

#define BUFFER_SIZE (2 * 1024 * 1024 + 4 * 1024) // 2MB + 4KB

struct rdma_cm_id *conn = NULL;
struct ibv_pd *pd;
struct ibv_mr *mr;
struct ibv_cq *cq;
char *buffer;


void
send_request_and_receive_response()
{
	// Post send request
	struct ibv_send_wr send_wr, *bad_send_wr = NULL;
	struct ibv_sge send_sge;
	memset(&send_wr, 0, sizeof(send_wr));
	send_wr.wr_id = 1;
	send_wr.opcode = IBV_WR_SEND;
	send_wr.send_flags = IBV_SEND_SIGNALED;
	send_sge.addr = (uintptr_t)buffer;
	send_sge.length = strlen("request from client") + 1;
	send_sge.lkey = mr->lkey;
	send_wr.sg_list = &send_sge;
	send_wr.num_sge = 1;
	if (ibv_post_send(conn->qp, &send_wr, &bad_send_wr))
	{
		perror("ibv_post_send");
		exit(1);
	}

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

	printf("Received response from server: %s\n", buffer);
}

void
sigint_handler(int signum)
{
	printf("SIGINT received. Sending request to server...\n");
	send_request_and_receive_response();
}

int
main(int argc, char **argv)
{
	struct sockaddr_in addr;
	struct rdma_event_channel *ec = NULL;
	struct ibv_qp_init_attr qp_attr;

	signal(SIGINT, sigint_handler);

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

	printf("Client finished successfully.\n");
	return 0;
}
