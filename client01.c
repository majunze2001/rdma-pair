#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rdma/rdma_cma.h>

#define BUFFER_SIZE 1024

int main(int argc, char **argv) {
    struct sockaddr_in addr;
    struct rdma_cm_id *conn = NULL;
    struct rdma_event_channel *ec = NULL;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    char *buffer;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5000);
    inet_pton(AF_INET, "10.10.10.221", &addr.sin_addr);

    ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel");
        return 1;
    }

    if (rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id");
        return 1;
    }

    if (rdma_resolve_addr(conn, NULL, (struct sockaddr *)&addr, 2000)) {
        perror("rdma_resolve_addr");
        return 1;
    }

    // Resolve route
    struct rdma_cm_event *event;
    if (rdma_get_cm_event(ec, &event)) {
        perror("rdma_get_cm_event");
        return 1;
    }
    rdma_ack_cm_event(event);

    if (rdma_resolve_route(conn, 2000)) {
        perror("rdma_resolve_route");
        return 1;
    }

    if (rdma_get_cm_event(ec, &event)) {
        perror("rdma_get_cm_event");
        return 1;
    }
    rdma_ack_cm_event(event);

    // Allocate buffer and register memory
    pd = ibv_alloc_pd(conn->verbs);
    buffer = malloc(BUFFER_SIZE);
    strcpy(buffer, "Hello from client!");
    mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    // Connect RDMA
    struct rdma_conn_param cm_params = {0};
    if (rdma_connect(conn, &cm_params)) {
        perror("rdma_connect");
        return 1;
    }

    if (rdma_get_cm_event(ec, &event)) {
        perror("rdma_get_cm_event");
        return 1;
    }
    rdma_ack_cm_event(event);

    printf("Connected to server %s:5000\n", "10.10.10.221");

    // Perform RDMA Write
    // Here you would typically use a work request to perform an RDMA Write operation

    // Clean up
    ibv_dereg_mr(mr);
    free(buffer);
    rdma_destroy_id(conn);
    rdma_destroy_event_channel(ec);

    return 0;
}

