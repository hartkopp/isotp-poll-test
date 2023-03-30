#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <stdbool.h>
#include <err.h>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <linux/can.h>
#include <linux/can/isotp.h>

#define CHECK(expr) ({ int ret = (expr); if (ret == -1) err(EXIT_FAILURE, "%s", #expr); ret; })

int main(int argc, char *argv[])
{
	int sock;
	struct sockaddr_can addr;
	char opt;
	bool in = false, out = false;
	bool validate_seq = false;
	bool blocking = false;
	bool wait_tx_done = false;
	bool quiet = false;
	int buf_size = 0;
	unsigned cnt = 1, max_msgs = 0;

	/* These default can be overridden with -s and -d */
	addr.can_addr.tp.tx_id = 0x123;
	addr.can_addr.tp.rx_id = 0x321;

	while ((opt = getopt(argc, argv, "abc:d:ioqs:w")) != -1) {
		switch (opt) {
		case 'a':
			validate_seq = true;
			break;
		case 'b':
			blocking = true;
			break;
		case 'c':
			max_msgs = atol(optarg);
			break;
		case 'i':
			in = true;
			break;
		case 'o':
			out = true;
			break;
		case 'q':
			quiet = true;
			break;
		case 'w':
			wait_tx_done = true;
			break;
		case 's':
			addr.can_addr.tp.tx_id = strtoul(optarg, NULL, 16);
			if (strlen(optarg) > 7)
				addr.can_addr.tp.tx_id |= CAN_EFF_FLAG;
			break;
		case 'd':
			addr.can_addr.tp.rx_id = strtoul(optarg, NULL, 16);
			if (strlen(optarg) > 7)
				addr.can_addr.tp.rx_id |= CAN_EFF_FLAG;
			break;
		default: /* '?' */
			err(EXIT_FAILURE, "Usage: %s [-i] [-o]", argv[0]);
		}
	}

	sock = CHECK(socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP));

	if (wait_tx_done) {
		struct can_isotp_options opts = { .flags = CAN_ISOTP_WAIT_TX_DONE};
		CHECK(setsockopt(sock, SOL_CAN_ISOTP, CAN_ISOTP_OPTS, &opts, sizeof(opts)));
	}

	const char *ifname = "vcan0";
	addr.can_family = AF_CAN;
	addr.can_ifindex = if_nametoindex(ifname);
	if (!addr.can_ifindex)
		err(EXIT_FAILURE, "%s", ifname);

	CHECK(bind(sock, (struct sockaddr *)&addr, sizeof(addr)));

	if (!blocking) {
		int flags = CHECK(fcntl(sock, F_GETFL, 0));
		CHECK(fcntl(sock, F_SETFL, flags | O_NONBLOCK));
	}

	struct pollfd pollfd = {
		.fd = sock,
		.events = ((in ? POLLIN : 0) | ((out & !in) ? POLLOUT : 0))
	};

	do {
		char buf[100];
		int ret;

		if (!blocking)
			CHECK(poll(&pollfd, 1, -1)); /* Wait with infinite timeout */

		if (pollfd.revents & POLLIN || (blocking && in)) {
			buf_size = CHECK(read(sock, buf, sizeof(buf) - 1));
			if (!quiet)
				printf("#%u: Read %d bytes\n", cnt, buf_size);
			if (validate_seq) {
				unsigned cnt_rcvd = 0;
				buf[buf_size] = 0;
				sscanf(buf, "Hello%u", &cnt_rcvd);
				if (cnt != cnt_rcvd)
					errx(EXIT_FAILURE, "Lost messages. Expected: #%u, received #%u", cnt, cnt_rcvd);
			}
			if (out)
				pollfd.events |= POLLOUT; /* Start writing only after reception of data */
		}
		if (pollfd.revents & POLLOUT || (blocking && out)) {
			if (!in) {
				char str[200];
				sprintf(str, "Hello%u", cnt);
				ret = CHECK(write(sock, str, strlen(str)));
			} else {
				ret = CHECK(write(sock, buf, buf_size));
			}
			if (!quiet)
				printf("#%u: Wrote %d bytes\n", cnt, ret);
		}
	} while (cnt++ < max_msgs || max_msgs == 0);

	return 0;
}
