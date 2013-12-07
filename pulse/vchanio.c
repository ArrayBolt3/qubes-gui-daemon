/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <libvchan.h>
#include <xs.h>
#include <xenctrl.h>

#ifdef XENCTRL_HAS_XC_INTERFACE
static xc_interface *xc_handle = NULL;
#else
static int xc_handle = -1;
#endif
/* 
the following is really a workaround for the fact that when the server
side dies because of "xm destroy", we get no wakeup. So we have to check
periodically if the peer is connected. Hopefully this can be removed in
some final version.
*/
void slow_check_for_libvchan_is_eof(struct libvchan *ctrl)
{
	struct evtchn_status evst;
	evst.port = ctrl->evport;
	evst.dom = DOMID_SELF;
	if (xc_evtchn_status(xc_handle, &evst)) {
		perror("xc_evtchn_status");
		exit(1);
	}
	if (evst.status != EVTCHNSTAT_interdomain) {
		fprintf(stderr, "event channel disconnected\n");
		exit(0);
	}
}


static int wait_for_vchan_once(struct libvchan *ctrl)
{
	fd_set rfds;
	int vfd, ret;
	struct timeval tv = { 0, 100000 };
	vfd = libvchan_fd_for_select(ctrl);
	FD_ZERO(&rfds);
	FD_SET(vfd, &rfds);
	ret = select(vfd + 1, &rfds, NULL, NULL, &tv);
	if (ret < 0) {
		perror("select");
		exit(1);
	}
	if (libvchan_is_eof(ctrl)) {
		fprintf(stderr, "libvchan_is_eof\n");
		exit(0);
	}
	if (ret == 0)
		slow_check_for_libvchan_is_eof(ctrl);
	if (FD_ISSET(vfd, &rfds))
		// we don't care about the result, but we need to do the read to
		// clear libvchan_fd pending state 
		libvchan_wait(ctrl);
	return ret;
}

void wait_for_vchan(struct libvchan *ctrl)
{
	while (wait_for_vchan_once(ctrl) == 0);
}

struct libvchan *peer_client_init(int dom, int port, char **name)
{
	struct libvchan *ctrl;
	struct xs_handle *xs;
	char buf[64];
	unsigned int len = 0;
	char *tmp;

	xs = xs_daemon_open();
	if (!xs) {
		perror("xs_daemon_open");
		exit(1);
	}
	if (name) {
		snprintf(buf, sizeof(buf), "/local/domain/%d/name", dom);
		*name = xs_read(xs, 0, buf, &len);
		if (!*name) {
			perror("xs_read domainname");
			exit(1);
		}
	}

	snprintf(buf, sizeof(buf), "/local/domain/%d", dom);
	do {
		ctrl = libvchan_client_init(dom, port);
		if (ctrl == NULL) {
			/* check if domain still alive */
			tmp = xs_read(xs, 0, buf, &len);
			if (!tmp) {
				fprintf(stderr, "domain dead\n");
				exit(1);
			}
			free(tmp);
			sleep(1);
		}
	} while (ctrl == NULL);
	xs_daemon_close(xs);
#ifdef XENCTRL_HAS_XC_INTERFACE
	xc_handle = xc_interface_open(NULL, 0, 0);
	if (xc_handle == NULL) {
#else
	xc_handle = xc_interface_open();
	if (xc_handle < 0) {
#endif
		perror("xc_interface_open");
		exit(1);
	}
	return ctrl;
}
