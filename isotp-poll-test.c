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

	/* for recvmsg() instead if read() */
	char ctrlmsg[CMSG_SPACE(sizeof(__u32))]; /* only check for SO_RXQ_OVFL */
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *cmsg;

	static __u32 dropcnt;
	static __u32 last_dropcnt;

	char buf[100];
	int ret;

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

	/* enable drop monitoring */
	const int dropmonitor_on = 1;
	CHECK(setsockopt(sock, SOL_SOCKET, SO_RXQ_OVFL, &dropmonitor_on, sizeof(dropmonitor_on)));

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

	/* these settings are static and can be held out of the hot path */
	msg.msg_name = &addr;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = &ctrlmsg;
	iov.iov_base = buf;

	do {
		if (!blocking)
			CHECK(poll(&pollfd, 1, -1)); /* Wait with infinite timeout */

		if (pollfd.revents & POLLIN || (blocking && in)) {

			iov.iov_len = sizeof(buf);
			msg.msg_namelen = sizeof(addr);
			msg.msg_controllen = sizeof(ctrlmsg);
			msg.msg_flags = 0;

			buf_size = CHECK(recvmsg(sock, &msg, 0));

			for (cmsg = CMSG_FIRSTHDR(&msg);
			     cmsg && (cmsg->cmsg_level == SOL_SOCKET);
			     cmsg = CMSG_NXTHDR(&msg,cmsg)) {
				if (cmsg->cmsg_type == SO_RXQ_OVFL) {
					memcpy(&dropcnt, CMSG_DATA(cmsg), sizeof(__u32));
				}
			}

			/* check for (unlikely) dropped frames on this specific socket */
			if (dropcnt != last_dropcnt) {
				__u32 frames = dropcnt - last_dropcnt;

				printf("DROPCOUNT: dropped %u CAN frame%s socket (total drops %u)\n",
				       frames, (frames > 1)?"s":"", dropcnt);

				last_dropcnt = dropcnt;
			}

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
