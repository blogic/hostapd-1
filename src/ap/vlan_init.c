/*
 * hostapd / VLAN initialization
 * Copyright 2003, Instant802 Networks, Inc.
 * Copyright 2005-2006, Devicescape Software, Inc.
 * Copyright (c) 2009, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"
#include <net/if.h>
#include <sys/ioctl.h>
#ifdef CONFIG_FULL_DYNAMIC_VLAN
#include <linux/sockios.h>
#include <linux/if_vlan.h>
#include <linux/if_bridge.h>
#endif /* CONFIG_FULL_DYNAMIC_VLAN */

#include "utils/common.h"
#include "hostapd.h"
#include "ap_config.h"
#include "ap_drv_ops.h"
#include "wpa_auth.h"
#include "vlan_init.h"
#include "vlan_util.h"

#include "wpa_auth_glue.h"
#ifdef CONFIG_RSN_PREAUTH_COPY
#include "preauth_auth.h"
#endif /* CONFIG_RSN_PREAUTH_COPY */


#ifdef CONFIG_FULL_DYNAMIC_VLAN

#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <linux/if_vlan.h>
#include "bridge.h"
#include "ifconfig.h"

#include "drivers/priv_netlink.h"
#include "utils/eloop.h"
#include <stdarg.h>
#include <sys/types.h>
#include <sys/wait.h>


struct full_dynamic_vlan {
	int s; /* socket on which to listen for new/removed interfaces. */
};

#define DVLAN_CLEAN_BR         0x1
#define DVLAN_CLEAN_VLAN       0x2
#define DVLAN_CLEAN_VLAN_PORT  0x4

struct dynamic_iface {
	char ifname[IFNAMSIZ + 1];
	int usage;
	int clean;
	struct dynamic_iface *next;
};


/* Increment ref counter for ifname and add clean flag.
 * If not in list, add it only if some flags are given.
 */
static void dyn_iface_get(struct hostapd_data *hapd, const char *ifname,
			  int clean)
{
	struct dynamic_iface *next, **dynamic_ifaces;
	struct hapd_interfaces *interfaces;

	interfaces = hapd->iface->interfaces;
	dynamic_ifaces = &interfaces->vlan_priv;

	for (next = *dynamic_ifaces; next; next = next->next) {
		if (os_strcmp(ifname, next->ifname) == 0)
			break;
	}

	if (next) {
		next->usage++;
		next->clean |= clean;
		return;
	}

	if (!clean)
		return;

	next = os_zalloc(sizeof(*next));
	if (!next)
		return;
	os_strlcpy(next->ifname, ifname, sizeof(next->ifname));
	next->usage = 1;
	next->clean = clean;
	next->next = *dynamic_ifaces;
	*dynamic_ifaces = next;
}


/* Decrement reference counter for given ifname.
 * Return clean flag iff reference counter was decreased to zero, else zero
 */
static int dyn_iface_put(struct hostapd_data *hapd, const char *ifname)
{
	struct dynamic_iface *next, *prev = NULL, **dynamic_ifaces;
	struct hapd_interfaces *interfaces;
	int clean;

	interfaces = hapd->iface->interfaces;
	dynamic_ifaces = &interfaces->vlan_priv;

	for (next = *dynamic_ifaces; next; next = next->next) {
		if (os_strcmp(ifname, next->ifname) == 0)
			break;
		prev = next;
	}

	if (!next)
		return 0;

	next->usage--;
	if (next->usage)
		return 0;

	if (prev)
		prev->next = next->next;
	else
		*dynamic_ifaces = next->next;
	clean = next->clean;
	os_free(next);

	return clean;
}


int run_script(char* output, int size, const char* script, ...)
{
	va_list ap;
	char *args[100];
	int argno = 0;
	pid_t pid;
	siginfo_t status;
	int filedes[2];

	va_start(ap, script);

	while (1) {
		args[argno] = va_arg(ap, char *);
		if (!args[argno])
			break;
		argno++;
		if (argno >= 100)
			break;
	}

	if (output) {
		if (pipe(filedes) < 0) {
			perror("pipe");
			return -1;
		}
	}

	pid = fork();
	if (pid < 0) {
		if (output) {
			close(filedes[1]);
			close(filedes[0]);
		}
		perror("fork");
		return -1;
	} else if (pid == 0) {
		if (output) {
			while ((dup2(filedes[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {};
			close(filedes[1]);
			close(filedes[0]);
		}
		execv(script, args);
		perror("execv");
		exit(1);
	}
	if (output)
		close(filedes[1]);
	if (waitid(P_PID, pid, &status, WEXITED) < 0)
		return -1;
	if (status.si_code != CLD_EXITED)
		return -1;
	if (status.si_status != 0)
		return 1;
	if (!output)
		return 0;

	for (;;) {
		if (size <= 0)
			break;
		if (read(filedes[0], output, 1) <= 0)
			break;
		if (*output == '\n') {
			*output = '\0';
			break;
		}
		output++;
		size--;
	}

	close(filedes[0]);
	return 0;
}


char* itoa(int i)
{
	static char buf[20];
	os_snprintf(buf, sizeof(buf), "%d", i);
	return buf;
}
#endif /* CONFIG_FULL_DYNAMIC_VLAN */


static int vlan_if_add(struct hostapd_data *hapd, struct hostapd_vlan *vlan,
		       int existsok)
{
	int ret, i;

	for (i = 0; i < NUM_WEP_KEYS; i++) {
		if (!hapd->conf->ssid.wep.key[i])
			continue;
		wpa_printf(MSG_ERROR,
			   "VLAN: Refusing to set up VLAN iface %s with WEP",
			   vlan->ifname);
		return -1;
	}

	if (!if_nametoindex(vlan->ifname))
		ret = hostapd_vlan_if_add(hapd, vlan->ifname);
	else if (!existsok)
		return -1;
	else
		ret = 0;

	if (ret)
		return ret;

	ifconfig_up(vlan->ifname); /* else wpa group will fail fatal */

#ifdef CONFIG_RSN_PREAUTH_COPY
	if (!vlan->rsn_preauth)
		vlan->rsn_preauth = rsn_preauth_snoop_init(hapd, vlan->ifname);
#endif /* CONFIG_RSN_PREAUTH_COPY */

	if (hapd->wpa_auth)
		ret = wpa_auth_ensure_group(hapd->wpa_auth, vlan->vlan_id);

	if (ret == 0)
		return ret;

	wpa_printf(MSG_ERROR, "WPA initialization for VLAN %d failed (%d)",
		   vlan->vlan_id, ret);
	if (wpa_auth_release_group(hapd->wpa_auth, vlan->vlan_id))
		wpa_printf(MSG_ERROR, "WPA deinit of %s failed", vlan->ifname);

#ifdef CONFIG_RSN_PREAUTH_COPY
	rsn_preauth_snoop_deinit(hapd, vlan->ifname, vlan->rsn_preauth);
#endif /* CONFIG_RSN_PREAUTH_COPY */

	/* group state machine setup failed */
	if (hostapd_vlan_if_remove(hapd, vlan->ifname))
		wpa_printf(MSG_ERROR, "Removal of %s failed", vlan->ifname);

	return ret;
}


static int vlan_if_remove(struct hostapd_data *hapd, struct hostapd_vlan *vlan)
{
	int ret;

	ret = wpa_auth_release_group(hapd->wpa_auth, vlan->vlan_id);
	if (ret)
		wpa_printf(MSG_ERROR,
			   "WPA deinitialization for VLAN %d failed (%d)",
			   vlan->vlan_id, ret);

#ifdef CONFIG_RSN_PREAUTH_COPY
	rsn_preauth_snoop_deinit(hapd, vlan->ifname, vlan->rsn_preauth);
	vlan->rsn_preauth = NULL;
#endif /* CONFIG_RSN_PREAUTH_COPY */

	return hostapd_vlan_if_remove(hapd, vlan->ifname);
}


#ifdef CONFIG_FULL_DYNAMIC_VLAN
/*
 * These are only available in recent linux headers (without the leading
 * underscore).
 */
#define _GET_VLAN_REALDEV_NAME_CMD	8
#define _GET_VLAN_VID_CMD		9

#ifndef CONFIG_VLAN_NETLINK

int vlan_rem(const char *if_name)
{
	int fd;
	struct vlan_ioctl_args if_request;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_rem(%s)", if_name);
	if ((os_strlen(if_name) + 1) > sizeof(if_request.device1)) {
		wpa_printf(MSG_ERROR, "VLAN: Interface name too long: '%s'",
			   if_name);
		return -1;
	}

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	os_memset(&if_request, 0, sizeof(if_request));

	os_strlcpy(if_request.device1, if_name, sizeof(if_request.device1));
	if_request.cmd = DEL_VLAN_CMD;

	if (ioctl(fd, SIOCSIFVLAN, &if_request) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: DEL_VLAN_CMD failed for %s: "
			   "%s", __func__, if_name, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}


/*
	Add a vlan interface with VLAN ID 'vid' and tagged interface
	'if_name'.

	returns -1 on error
	returns 1 if the interface already exists
	returns 0 otherwise
*/
int vlan_add(const char *if_name, int vid, const char *vlan_if_name)
{
	int fd;
	struct vlan_ioctl_args if_request;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_add(if_name=%s, vid=%d)",
		   if_name, vid);
	ifconfig_up(if_name);

	if ((os_strlen(if_name) + 1) > sizeof(if_request.device1)) {
		wpa_printf(MSG_ERROR, "VLAN: Interface name too long: '%s'",
			   if_name);
		return -1;
	}

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	os_memset(&if_request, 0, sizeof(if_request));

	/* Determine if a suitable vlan device already exists. */

	os_snprintf(if_request.device1, sizeof(if_request.device1), "vlan%d",
		    vid);

	if_request.cmd = _GET_VLAN_VID_CMD;

	if (ioctl(fd, SIOCSIFVLAN, &if_request) == 0) {

		if (if_request.u.VID == vid) {
			if_request.cmd = _GET_VLAN_REALDEV_NAME_CMD;

			if (ioctl(fd, SIOCSIFVLAN, &if_request) == 0 &&
			    os_strncmp(if_request.u.device2, if_name,
				       sizeof(if_request.u.device2)) == 0) {
				close(fd);
				wpa_printf(MSG_DEBUG, "VLAN: vlan_add: "
					   "if_name %s exists already",
					   if_request.device1);
				return 1;
			}
		}
	}

	/* A suitable vlan device does not already exist, add one. */

	os_memset(&if_request, 0, sizeof(if_request));
	os_strlcpy(if_request.device1, if_name, sizeof(if_request.device1));
	if_request.u.VID = vid;
	if_request.cmd = ADD_VLAN_CMD;

	if (ioctl(fd, SIOCSIFVLAN, &if_request) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: ADD_VLAN_CMD failed for %s: "
			   "%s",
			   __func__, if_request.device1, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}


static int vlan_set_name_type(unsigned int name_type)
{
	int fd;
	struct vlan_ioctl_args if_request;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_set_name_type(name_type=%u)",
		   name_type);
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	os_memset(&if_request, 0, sizeof(if_request));

	if_request.u.name_type = name_type;
	if_request.cmd = SET_VLAN_NAME_TYPE_CMD;
	if (ioctl(fd, SIOCSIFVLAN, &if_request) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: SET_VLAN_NAME_TYPE_CMD "
			   "name_type=%u failed: %s",
			   __func__, name_type, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

#endif /* CONFIG_VLAN_NETLINK */

static void vlan_newlink_tagged(int vlan_naming, char* tagged_interface,
				char* br_name, int vid,
				struct hostapd_data *hapd)
{
	char vlan_ifname[IFNAMSIZ];
	int clean;
	char *script = hapd->conf->ssid.vlan_script;

	if (vlan_naming ==  DYNAMIC_VLAN_NAMING_WITH_DEVICE)
		os_snprintf(vlan_ifname, sizeof(vlan_ifname), "%s.%d",
			     tagged_interface,  vid);
	else
		os_snprintf(vlan_ifname, sizeof(vlan_ifname), "vlan%d",
			     vid);

	clean = 0;
	ifconfig_up(tagged_interface);

	if (script) {
		if (!run_script(NULL, 0, script, "br_addif", br_name, tagged_interface, "tagged", itoa(vid)))
			clean |= DVLAN_CLEAN_VLAN_PORT;
	} else {
		if (!vlan_add(tagged_interface, vid, vlan_ifname))
			clean |= DVLAN_CLEAN_VLAN;

		if (!br_addif(br_name, vlan_ifname))
			clean |= DVLAN_CLEAN_VLAN_PORT;
	}

	dyn_iface_get(hapd, vlan_ifname, clean);

	ifconfig_up(vlan_ifname);
}

static void vlan_bridge_name(char *br_name, struct hostapd_data *hapd, int vid)
{
	char *tagged_interface = hapd->conf->ssid.vlan_tagged_interface;
	char *script = hapd->conf->ssid.vlan_script;

	if (script && !run_script(br_name, IFNAMSIZ, script, "br_name", hapd->conf->vlan_bridge, tagged_interface, itoa(vid)))
		return;

	if (hapd->conf->vlan_bridge[0]) {
		os_snprintf(br_name, IFNAMSIZ, "%s%d",
			    hapd->conf->vlan_bridge,  vid);
	} else if (tagged_interface) {
		os_snprintf(br_name, IFNAMSIZ, "br%s.%d",
			    tagged_interface, vid);
	} else {
		os_snprintf(br_name, IFNAMSIZ,
		            "brvlan%d", vid);
	}
}


static void vlan_get_bridge(char *br_name, struct hostapd_data *hapd, int vid)
{
	char *tagged_interface = hapd->conf->ssid.vlan_tagged_interface;
	int vlan_naming = hapd->conf->ssid.vlan_naming;
	char *script = hapd->conf->ssid.vlan_script;
	int ret;

	if (!script)
		ret = br_addbr(br_name);
	else
		ret = run_script(NULL, 0, script, "br_addbr", br_name, itoa(vid));

	dyn_iface_get(hapd, br_name, ret ? 0 : DVLAN_CLEAN_BR);

	ifconfig_up(br_name);

	if (tagged_interface)
		vlan_newlink_tagged(vlan_naming, tagged_interface, br_name,
				    vid, hapd);
}


static void vlan_newlink(char *ifname, struct hostapd_data *hapd)
{
	char br_name[IFNAMSIZ];
	struct hostapd_vlan *vlan;
	int untagged, *tagged, i, notempty;
	char *script = hapd->conf->ssid.vlan_script;
	int ret;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_newlink(%s)", ifname);

	for (vlan = hapd->conf->vlan; vlan; vlan = vlan->next) {
		if (vlan->configured)
			continue;
		if (os_strcmp(ifname, vlan->ifname))
			continue;

		vlan->configured = 1;

		notempty = vlan->vlan_desc.notempty;
		untagged = vlan->vlan_desc.untagged;
		tagged = vlan->vlan_desc.tagged;

		if (!notempty) {
			/* non-VLAN sta */
			if (hapd->conf->bridge[0]) {
				if (script)
					ret = run_script(NULL, 0, script, "br_addif", hapd->conf->bridge, ifname);
				else
					ret = br_addif(hapd->conf->bridge, ifname);
				if (!ret)
					vlan->clean |= DVLAN_CLEAN_WLAN_PORT;
			}
		} else if (untagged > 0 && untagged <= MAX_VLAN_ID) {
			vlan_bridge_name(br_name, hapd, untagged);

			vlan_get_bridge(br_name, hapd, untagged);

			if (script)
				ret = run_script(NULL, 0, script, "br_addif", br_name, ifname, "untagged", itoa(untagged));
			else
				ret = br_addif(br_name, ifname);
			if (!ret)
				vlan->clean |= DVLAN_CLEAN_WLAN_PORT;
		}

		for (i = 0; i < MAX_NUM_TAGGED_VLAN && tagged[i]; i++) {
			if (tagged[i] == untagged)
				continue;
			if (tagged[i] <= 0 || tagged[i] > MAX_VLAN_ID)
				continue;
			if (i > 0 && tagged[i] == tagged[i-1])
				continue;
			vlan_bridge_name(br_name, hapd, tagged[i]);
			vlan_get_bridge(br_name, hapd, tagged[i]);
			vlan_newlink_tagged(DYNAMIC_VLAN_NAMING_WITH_DEVICE,
					    ifname, br_name, tagged[i], hapd);
		}

		ifconfig_up(ifname);

#ifdef CONFIG_RSN_PREAUTH_COPY
		if (!vlan->rsn_preauth)
			vlan->rsn_preauth = rsn_preauth_snoop_init(hapd, vlan->ifname);
#endif /* CONFIG_RSN_PREAUTH_COPY */

		break;
	}
}

static void vlan_dellink_tagged(int vlan_naming, char* tagged_interface,
				char* br_name, int vid,
				struct hostapd_data *hapd)
{
	char vlan_ifname[IFNAMSIZ];
	int clean;
	char *script = hapd->conf->ssid.vlan_script;

	if (vlan_naming ==  DYNAMIC_VLAN_NAMING_WITH_DEVICE)
		os_snprintf(vlan_ifname, sizeof(vlan_ifname), "%s.%d",
			     tagged_interface,  vid);
	else
		os_snprintf(vlan_ifname, sizeof(vlan_ifname), "vlan%d",
			     vid);

	clean = dyn_iface_put(hapd, vlan_ifname);

	if (script) {
		if (clean & DVLAN_CLEAN_VLAN_PORT)
			run_script(NULL, 0, script, "br_delif", br_name, tagged_interface, "tagged", itoa(vid));
	} else {
		if (clean & DVLAN_CLEAN_VLAN_PORT)
			br_delif(br_name, vlan_ifname);

		if (clean & DVLAN_CLEAN_VLAN) {
			ifconfig_down(vlan_ifname);
			vlan_rem(vlan_ifname);
		}
	}
}

static void vlan_put_bridge(char *br_name, struct hostapd_data *hapd, int vid)
{
	int clean;
	char *tagged_interface = hapd->conf->ssid.vlan_tagged_interface;
	int vlan_naming = hapd->conf->ssid.vlan_naming;
	char *script = hapd->conf->ssid.vlan_script;

	if (tagged_interface)
		vlan_dellink_tagged(vlan_naming, tagged_interface, br_name,
				    vid, hapd);

	clean = dyn_iface_put(hapd, br_name);

	if (!(clean & DVLAN_CLEAN_BR))
		return;
	if (!script && br_getnumports(br_name) != 0)
		return;

	ifconfig_down(br_name);

	if (script)
		run_script(NULL, 0, script, "br_delbr", br_name, itoa(vid));
	else
		br_delbr(br_name);
}

static void vlan_dellink(char *ifname, struct hostapd_data *hapd)
{
	struct hostapd_vlan *first, *prev, *vlan = hapd->conf->vlan;
	char br_name[IFNAMSIZ];
	int untagged, i, *tagged, notempty;
	char *script = hapd->conf->ssid.vlan_script;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_dellink(%s)", ifname);

	first = prev = vlan;

	while (vlan) {
		if (os_strcmp(ifname, vlan->ifname)) {
			prev = vlan;
			vlan = vlan->next;
			continue;
		}

		if (!vlan->configured)
			goto skip_counting;

#ifdef CONFIG_RSN_PREAUTH_COPY
		rsn_preauth_snoop_deinit(hapd, vlan->ifname,
					 vlan->rsn_preauth);
#endif /* CONFIG_RSN_PREAUTH_COPY */

		notempty = vlan->vlan_desc.notempty;
		untagged = vlan->vlan_desc.untagged;
		tagged = vlan->vlan_desc.tagged;

		for (i = 0; i < MAX_NUM_TAGGED_VLAN && tagged[i]; i++) {
			if (tagged[i] == untagged)
				continue;
			if (tagged[i] <= 0 || tagged[i] > MAX_VLAN_ID)
				continue;
			if (i > 0 && tagged[i] == tagged[i-1])
				continue;
			vlan_bridge_name(br_name, hapd, tagged[i]);
			vlan_dellink_tagged(DYNAMIC_VLAN_NAMING_WITH_DEVICE,
					    ifname, br_name, tagged[i], hapd);
			vlan_put_bridge(br_name, hapd, tagged[i]);
		}

		if (!notempty) {
			/* non-VLAN sta */
			if (hapd->conf->bridge[0] && vlan->clean & DVLAN_CLEAN_WLAN_PORT) {
				if (script)
					run_script(NULL, 0, script, "br_delif", hapd->conf->bridge, ifname);
				else
					br_delif(hapd->conf->bridge, ifname);
			}
		} else if (untagged > 0 && untagged <= MAX_VLAN_ID) {
			vlan_bridge_name(br_name, hapd, untagged);

			if (vlan->clean & DVLAN_CLEAN_WLAN_PORT) {
				if (script)
					run_script(NULL, 0, script, "br_delif", br_name, vlan->ifname, "untagged", itoa(untagged));
				else
					br_delif(br_name, vlan->ifname);
			}

			vlan_put_bridge(br_name, hapd, untagged);
		}

skip_counting:
		/* ensure this vlan interface is actually removed even if
		 * NEWLINK message is only received later */
		if (if_nametoindex(vlan->ifname) && vlan_if_remove(hapd, vlan))
			wpa_printf(MSG_ERROR, "VLAN: Could not remove VLAN "
				   "iface: %s: %s",
				   vlan->ifname, strerror(errno));

		if (vlan == first) {
			hapd->conf->vlan = vlan->next;
		} else {
			prev->next = vlan->next;
		}
		os_free(vlan);

		break;
	}
}


static void
vlan_read_ifnames(struct nlmsghdr *h, size_t len, int del,
		  struct hostapd_data *hapd)
{
	struct ifinfomsg *ifi;
	int attrlen, nlmsg_len, rta_len;
	struct rtattr *attr;
	char ifname[IFNAMSIZ + 1];

	if (len < sizeof(*ifi))
		return;

	ifi = NLMSG_DATA(h);

	nlmsg_len = NLMSG_ALIGN(sizeof(struct ifinfomsg));

	attrlen = h->nlmsg_len - nlmsg_len;
	if (attrlen < 0)
		return;

	attr = (struct rtattr *) (((char *) ifi) + nlmsg_len);

	os_memset(ifname, 0, sizeof(ifname));
	rta_len = RTA_ALIGN(sizeof(struct rtattr));
	while (RTA_OK(attr, attrlen)) {
		if (attr->rta_type == IFLA_IFNAME) {
			int n = attr->rta_len - rta_len;
			if (n < 0)
				break;

			if ((size_t) n >= sizeof(ifname))
				n = sizeof(ifname) - 1;
			os_memcpy(ifname, ((char *) attr) + rta_len, n);

		}

		attr = RTA_NEXT(attr, attrlen);
	}

	if (!ifname[0])
		return;
	if (del && if_nametoindex(ifname)) {
		 /* interface still exists, race condition ->
		  * iface has just been recreated */
		return;
	}

	wpa_printf(MSG_DEBUG,
		   "VLAN: RTM_%sLINK: ifi_index=%d ifname=%s ifi_family=%d ifi_flags=0x%x (%s%s%s%s)",
		   del ? "DEL" : "NEW",
		   ifi->ifi_index, ifname, ifi->ifi_family, ifi->ifi_flags,
		   (ifi->ifi_flags & IFF_UP) ? "[UP]" : "",
		   (ifi->ifi_flags & IFF_RUNNING) ? "[RUNNING]" : "",
		   (ifi->ifi_flags & IFF_LOWER_UP) ? "[LOWER_UP]" : "",
		   (ifi->ifi_flags & IFF_DORMANT) ? "[DORMANT]" : "");

	if (del)
		vlan_dellink(ifname, hapd);
	else
		vlan_newlink(ifname, hapd);
}


static void vlan_event_receive(int sock, void *eloop_ctx, void *sock_ctx)
{
	char buf[8192];
	int left;
	struct sockaddr_nl from;
	socklen_t fromlen;
	struct nlmsghdr *h;
	struct hostapd_data *hapd = eloop_ctx;

	fromlen = sizeof(from);
	left = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT,
			(struct sockaddr *) &from, &fromlen);
	if (left < 0) {
		if (errno != EINTR && errno != EAGAIN)
			wpa_printf(MSG_ERROR, "VLAN: %s: recvfrom failed: %s",
				   __func__, strerror(errno));
		return;
	}

	h = (struct nlmsghdr *) buf;
	while (NLMSG_OK(h, left)) {
		int len, plen;

		len = h->nlmsg_len;
		plen = len - sizeof(*h);
		if (len > left || plen < 0) {
			wpa_printf(MSG_DEBUG, "VLAN: Malformed netlink "
				   "message: len=%d left=%d plen=%d",
				   len, left, plen);
			break;
		}

		switch (h->nlmsg_type) {
		case RTM_NEWLINK:
			vlan_read_ifnames(h, plen, 0, hapd);
			break;
		case RTM_DELLINK:
			vlan_read_ifnames(h, plen, 1, hapd);
			break;
		}

		h = NLMSG_NEXT(h, left);
	}

	if (left > 0) {
		wpa_printf(MSG_DEBUG, "VLAN: %s: %d extra bytes in the end of "
			   "netlink message", __func__, left);
	}
}


static struct full_dynamic_vlan *
full_dynamic_vlan_init(struct hostapd_data *hapd)
{
	struct sockaddr_nl local;
	struct full_dynamic_vlan *priv;

	priv = os_zalloc(sizeof(*priv));
	if (priv == NULL)
		return NULL;

#ifndef CONFIG_VLAN_NETLINK
	vlan_set_name_type(hapd->conf->ssid.vlan_naming ==
			   DYNAMIC_VLAN_NAMING_WITH_DEVICE ?
			   VLAN_NAME_TYPE_RAW_PLUS_VID_NO_PAD :
			   VLAN_NAME_TYPE_PLUS_VID_NO_PAD);
#endif /* CONFIG_VLAN_NETLINK */

	priv->s = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (priv->s < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(PF_NETLINK,SOCK_RAW,"
			   "NETLINK_ROUTE) failed: %s",
			   __func__, strerror(errno));
		os_free(priv);
		return NULL;
	}

	os_memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;
	local.nl_groups = RTMGRP_LINK;
	if (bind(priv->s, (struct sockaddr *) &local, sizeof(local)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: bind(netlink) failed: %s",
			   __func__, strerror(errno));
		close(priv->s);
		os_free(priv);
		return NULL;
	}

	if (eloop_register_read_sock(priv->s, vlan_event_receive, hapd, NULL))
	{
		close(priv->s);
		os_free(priv);
		return NULL;
	}

	return priv;
}


static void full_dynamic_vlan_deinit(struct full_dynamic_vlan *priv)
{
	if (priv == NULL)
		return;
	eloop_unregister_read_sock(priv->s);
	close(priv->s);
	os_free(priv);
}
#endif /* CONFIG_FULL_DYNAMIC_VLAN */


static int vlan_dynamic_add(struct hostapd_data *hapd,
			    struct hostapd_vlan *vlan)
{
	while (vlan) {
		if (vlan->vlan_id != VLAN_ID_WILDCARD) {
			if (vlan_if_add(hapd, vlan, 1)) {
				wpa_printf(MSG_ERROR,
					   "VLAN: Could not add VLAN %s: %s",
					   vlan->ifname, strerror(errno));
				return -1;
			}
#ifdef CONFIG_FULL_DYNAMIC_VLAN
			vlan_newlink(vlan->ifname, hapd);
#endif /* CONFIG_FULL_DYNAMIC_VLAN */
		}

		vlan = vlan->next;
	}

	return 0;
}


static void vlan_dynamic_remove(struct hostapd_data *hapd,
				struct hostapd_vlan *vlan)
{
	struct hostapd_vlan *next;

	while (vlan) {
		next = vlan->next;

#ifdef CONFIG_FULL_DYNAMIC_VLAN
		/* vlan_dellink takes care of cleanup and interface removal */
		if (vlan->vlan_id != VLAN_ID_WILDCARD)
			vlan_dellink(vlan->ifname, hapd);
#else
		if (vlan->vlan_id != VLAN_ID_WILDCARD &&
		    vlan_if_remove(hapd, vlan)) {
			wpa_printf(MSG_ERROR, "VLAN: Could not remove VLAN "
				   "iface: %s: %s",
				   vlan->ifname, strerror(errno));
		}
#endif /* CONFIG_FULL_DYNAMIC_VLAN */

		vlan = next;
	}
}


int vlan_init(struct hostapd_data *hapd)
{
#ifdef CONFIG_FULL_DYNAMIC_VLAN
	hapd->full_dynamic_vlan = full_dynamic_vlan_init(hapd);
#endif /* CONFIG_FULL_DYNAMIC_VLAN */

	if ((hapd->conf->ssid.dynamic_vlan != DYNAMIC_VLAN_DISABLED ||
	     hapd->conf->ssid.per_sta_vif) && !hapd->conf->vlan) {
		/* dynamic vlans enabled but no (or empty) vlan_file given */
		struct hostapd_vlan *vlan;
		vlan = os_zalloc(sizeof(*vlan));
		if (vlan == NULL) {
			wpa_printf(MSG_ERROR, "Out of memory while assigning "
				   "VLAN interfaces");
			return -1;
		}

		vlan->vlan_id = VLAN_ID_WILDCARD;
		os_snprintf(vlan->ifname, sizeof(vlan->ifname), "%s.#",
			    hapd->conf->iface);
		vlan->next = hapd->conf->vlan;
		hapd->conf->vlan = vlan;
	}

	if (vlan_dynamic_add(hapd, hapd->conf->vlan))
		return -1;

        return 0;
}


void vlan_deinit(struct hostapd_data *hapd)
{
	vlan_dynamic_remove(hapd, hapd->conf->vlan);

#ifdef CONFIG_FULL_DYNAMIC_VLAN
	full_dynamic_vlan_deinit(hapd->full_dynamic_vlan);
	hapd->full_dynamic_vlan = NULL;
#endif /* CONFIG_FULL_DYNAMIC_VLAN */
}


struct hostapd_vlan * vlan_add_dynamic(struct hostapd_data *hapd,
				       struct hostapd_vlan *vlan,
				       int vlan_id,
				       struct vlan_description vlan_desc)
{
	struct hostapd_vlan *n = NULL;
	char *ifname, *pos;

	if (vlan == NULL || vlan->vlan_id != VLAN_ID_WILDCARD)
		return NULL;

	wpa_printf(MSG_DEBUG, "VLAN: %s(vlan_id=%d ifname=%s)",
		   __func__, vlan_id, vlan->ifname);
	ifname = os_strdup(vlan->ifname);
	if (ifname == NULL)
		return NULL;
	pos = os_strchr(ifname, '#');
	if (pos == NULL)
		goto free_ifname;
	*pos++ = '\0';

	n = os_zalloc(sizeof(*n));
	if (n == NULL)
		goto free_ifname;

	n->vlan_id = vlan_id;
	n->vlan_desc = vlan_desc;
	n->dynamic_vlan = 1;

	os_snprintf(n->ifname, sizeof(n->ifname), "%s%d%s", ifname, vlan_id,
		    pos);

	n->next = hapd->conf->vlan;
	hapd->conf->vlan = n;

	/* hapd->conf->vlan needs this new VLAN here for WPA setup */
	if (vlan_if_add(hapd, n, 0)) {
		hapd->conf->vlan = n->next;
		os_free(n);
		n = NULL;
		goto free_ifname;
	}

free_ifname:
	os_free(ifname);
	return n;
}


int vlan_remove_dynamic(struct hostapd_data *hapd, int vlan_id)
{
	struct hostapd_vlan *vlan;

	if (vlan_id <= 0)
		return 1;

	wpa_printf(MSG_DEBUG, "VLAN: %s(ifname=%s vlan_id=%d)",
		   __func__, hapd->conf->iface, vlan_id);

	vlan = hapd->conf->vlan;
	while (vlan) {
		if (vlan->vlan_id == vlan_id && vlan->dynamic_vlan > 0) {
			vlan->dynamic_vlan--;
			break;
		}
		vlan = vlan->next;
	}

	if (vlan == NULL)
		return 1;

	if (vlan->dynamic_vlan == 0) {
		vlan_if_remove(hapd, vlan);
#ifdef CONFIG_FULL_DYNAMIC_VLAN
		vlan_dellink(vlan->ifname, hapd);
#endif /* CONFIG_FULL_DYNAMIC_VLAN */
	}

	return 0;
}
