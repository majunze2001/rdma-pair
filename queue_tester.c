#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

// #define UVM

// rdma setup
#ifdef UVM
#include <fcntl.h>
#include <sys/ioctl.h>
#define BUFFER_SIZE 2 * 1024 * 1024 // 2MB
#define SET_BUFFER 0x12345678
char *buffer;
#endif

#define DEVICE_NAME "/dev/fault_queue"
#define QUEUE_SIZE 32

struct fault_task
{
	void *fault_va;
	int processed;
};

struct fault_queue
{
	struct fault_task buffer[QUEUE_SIZE];
	volatile int head;
	volatile int tail;
};

int
main()
{
	int fd;
#ifdef UVM
	int ret;
	buffer = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
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

	// const char *request = "Request from server!";
	// strcpy(buffer, request);
	memset(buffer, 0, BUFFER_SIZE);
#endif
	fd = open(DEVICE_NAME, O_RDWR);
	if (fd < 0)
	{
		perror("open");
		return 1;
	}

	printf("open success\n");

	struct fault_queue *queue = mmap(NULL, sizeof(struct fault_queue),
	                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (queue == MAP_FAILED)
	{
		perror("mmap");
		return 1;
	}

	printf("mmap success\n");

	while (1)
	{
		__sync_synchronize(); // Memory barrier
		if (queue->head != queue->tail)
		{
			// printf("access queue success head[%d] tail[%d]\n", queue->head, queue->tail);
			struct fault_task *task = &queue->buffer[queue->tail];
			// printf("access task success\n");
			// Process the task...
			task->processed = 1;
			__sync_synchronize();
			// printf("update task success\n");
			// user space program does not update the queue
		}
		// usleep(500); // Reduce CPU consumption
		             // sleep(2); // debug only
	}

	printf("never reached\n");

	return 0;
}
