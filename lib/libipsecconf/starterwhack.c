/*
 * Libreswan whack functions to communicate with pluto (whack.c)
 *
 * Copyright (C) 2001-2002 Mathieu Lafon - Arkoon Network Security
 * Copyright (C) 2004-2006 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2010-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2011 Mattias Walström <lazzer@vmlinux.org>
 * Copyright (C) 2012-2017 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012 Philippe Vouters <Philippe.Vouters@laposte.net>
 * Copyright (C) 2013 Antony Antony <antony@phenome.org>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2016, Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <sys/types.h>
#include <sys/un.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "lsw_socket.h"

#include "ttodata.h"

#include "ipsecconf/starterwhack.h"
#include "ipsecconf/confread.h"
#include "ipsecconf/starterlog.h"

#include "lswalloc.h"
#include "lswlog.h"
#include "whack.h"
#include "id.h"
#include "ip_address.h"
#include "ip_info.h"

static int send_reply(int sock, char *buf, ssize_t len)
{
	/* send the secret to pluto */
	if (write(sock, buf, len) != len) {
		int e = errno;

		starter_log(LOG_LEVEL_ERR, "whack: write() failed (%d %s)",
			e, strerror(e));
		return RC_WHACK_PROBLEM;
	}
	return 0;
}

static int starter_whack_read_reply(int sock,
				char xauthusername[MAX_XAUTH_USERNAME_LEN],
				char xauthpass[XAUTH_MAX_PASS_LENGTH],
				int usernamelen,
				int xauthpasslen)
{
	char buf[4097]; /* arbitrary limit on log line length */
	char *be = buf;
	int ret = 0;

	for (;; ) {
		char *ls = buf;
		ssize_t rl = read(sock, be, (buf + sizeof(buf) - 1) - be);

		if (rl < 0) {
			int e = errno;

			fprintf(stderr, "whack: read() failed (%d %s)\n", e,
				strerror(e));
			return RC_WHACK_PROBLEM;
		}
		if (rl == 0) {
			if (be != buf)
				fprintf(stderr,
					"whack: last line from pluto too long or unterminated\n");


			break;
		}

		be += rl;
		*be = '\0';

		for (;; ) {
			char *le = strchr(ls, '\n');

			if (le == NULL) {
				/* move last, partial line to start of buffer */
				memmove(buf, ls, be - ls);
				be -= ls - buf;
				break;
			}
			le++;	/* include NL in line */

			/*
			 * figure out prefix number and how it should
			 * affect our exit status and printing
			 */
			char *lpe = NULL; /* line-prefix-end */
			unsigned long s = strtoul(ls, &lpe, 10);
			if (lpe == ls || *lpe != ' ') {
				/* includes embedded NL, see above */
				fprintf(stderr, "whack: log line missing NNN prefix: %*s",
					(int)(le - ls), ls);
#if 0
				ls = le;
				continue;
#else
				exit(RC_WHACK_PROBLEM);
#endif
			}

			ls = lpe + 1; /* skip NNN_ */

			if (write(STDOUT_FILENO, ls, le - ls) == -1) {
				int e = errno;
				starter_log(LOG_LEVEL_ERR,
					    "whack: write() starterwhack.c:124 failed (%d %s), and ignored.",
					    e, strerror(e));
			}
			/*
			 * figure out prefix number and how it should affect
			 * our exit status
			 */
			{

				switch (s) {
				case RC_COMMENT:
				case RC_RAW:
				case RC_LOG:
				case RC_INFORMATIONAL:
					/* ignore */
					break;

				case RC_SUCCESS:
					/* be happy */
					ret = 0;
					break;

				case RC_ENTERSECRET:
					if (xauthpasslen == 0) {
						xauthpasslen =
							whack_get_secret(
								xauthpass,
								XAUTH_MAX_PASS_LENGTH);
					}
					if (xauthpasslen >
						XAUTH_MAX_PASS_LENGTH) {
						/*
						 * for input >= 128,
						 * xauthpasslen would be 129
						 */
						xauthpasslen =
							XAUTH_MAX_PASS_LENGTH;
						starter_log(LOG_LEVEL_ERR,
							"xauth password cannot be >= %d chars",
							XAUTH_MAX_PASS_LENGTH);
					}
					ret = send_reply(sock, xauthpass,
							xauthpasslen);
					if (ret != 0)
						return ret;

					break;

				case RC_USERPROMPT:
					if (usernamelen == 0) {
						usernamelen = whack_get_value(
							xauthusername,
							MAX_XAUTH_USERNAME_LEN);
					}
					if (usernamelen >
						MAX_XAUTH_USERNAME_LEN) {
						/*
						 * for input >= 128,
						 * useramelen would be 129
						 */
						usernamelen =
							MAX_XAUTH_USERNAME_LEN;
						starter_log(LOG_LEVEL_ERR,
							"username cannot be >= %d chars",
							MAX_XAUTH_USERNAME_LEN);
					}
					ret = send_reply(sock, xauthusername,
							usernamelen);
					if (ret != 0)
						return ret;

					break;

				default:
					/* pass through */
					ret = s;
					break;
				}
			}
			ls = le;
		}
	}
	return ret;
}

static int send_whack_msg(struct whack_message *msg, char *ctlsocket, struct logger *logger)
{
	struct sockaddr_un ctl_addr = { .sun_family = AF_UNIX };
	int sock;
	ssize_t len;
	struct whackpacker wp;
	err_t ugh;
	int ret;

	/* copy socket location */
	fill_and_terminate(ctl_addr.sun_path, ctlsocket, sizeof(ctl_addr.sun_path));

	/*  Pack strings */
	wp.msg = msg;
	wp.str_next = (unsigned char *)msg->string;
	wp.str_roof = (unsigned char *)&msg->string[sizeof(msg->string)];

	ugh = pack_whack_msg(&wp, logger);

	if (ugh != NULL) {
		starter_log(LOG_LEVEL_ERR,
			"send_wack_msg(): can't pack strings: %s", ugh);
		return -1;
	}

	len = wp.str_next - (unsigned char *)msg;

	/* Connect to pluto ctl */
	sock = cloexec_socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		starter_log(LOG_LEVEL_ERR, "socket() failed: %s",
			strerror(errno));
		return -1;
	}
	if (connect(sock, (struct sockaddr *)&ctl_addr,
			offsetof(struct sockaddr_un, sun_path) +
				strlen(ctl_addr.sun_path)) <
		0)
	{
		starter_log(LOG_LEVEL_ERR, "connect(pluto_ctl) failed: %s",
			strerror(errno));
		close(sock);
		return -1;
	}

	/* Send message */
	if (write(sock, msg, len) != len) {
		starter_log(LOG_LEVEL_ERR, "write(pluto_ctl) failed: %s",
			strerror(errno));
		close(sock);
		return -1;
	}

	/* read reply */
	{
		char xauthusername[MAX_XAUTH_USERNAME_LEN];
		char xauthpass[XAUTH_MAX_PASS_LENGTH];

		ret = starter_whack_read_reply(sock, xauthusername, xauthpass, 0,
					0);
		close(sock);
	}

	return ret;
}

static const struct whack_message empty_whack_message = {
	.magic = WHACK_MAGIC,
};

/* NOT RE-ENTRANT: uses a static buffer */
static char *connection_name(const struct starter_conn *conn)
{
	/* If connection name is '%auto', create a new name like conn_xxxxx */
	static char buf[32];

	if (streq(conn->name, "%auto")) {
		snprintf(buf, sizeof(buf), "conn_%ld", conn->id);
		return buf;
	} else {
		return conn->name;
	}
}

static bool set_whack_end(struct whack_end *w,
			  const struct starter_end *l)
{
	const char *lr = l->leftright;
	w->leftright = lr;
	w->id = l->id;
	w->host_type = l->addrtype;

	switch (l->addrtype) {
	case KH_IPADDR:
	case KH_IFACE:
		w->host_addr = l->addr;
		break;

	case KH_DEFAULTROUTE:
	case KH_IPHOSTNAME:
		/* note: we always copy the name string below */
		w->host_addr = unset_address;
		break;

	case KH_OPPO:
	case KH_GROUP:
	case KH_OPPOGROUP:
		/* policy should have been set to OPPO */
		w->host_addr = unset_address;
		break;

	case KH_ANY:
		w->host_addr = unset_address;
		break;

	default:
		printf("Failed to load connection %s= is not set\n", lr);
		return false;
	}
	w->host_addr_name = l->strings[KSCF_IP];

	switch (l->nexttype) {
	case KH_IPADDR:
		w->host_nexthop = l->nexthop;
		break;

	case KH_DEFAULTROUTE: /* acceptable to set nexthop to %defaultroute */
	case KH_NOTSET:	/* acceptable to not set nexthop */
		/*
		 * but, get the family set up right
		 * XXX the nexthop type has to get into the whack message!
		 */
		w->host_nexthop = l->host_family->address.unspec;
		break;

	default:
		printf("%s: do something with nexthop case: %d\n", lr,
			l->nexttype);
		break;
	}

	w->sourceip = l->sourceip; /* could be NULL */

	if (cidr_is_specified(l->vti_ip))
		w->host_vtiip = l->vti_ip;

	if (cidr_is_specified(l->ifaceip))
		w->ifaceip = l->ifaceip;

	w->subnet = l->subnet;

	if (l->strings[KSCF_SUBNETS] != NULL) {
		w->subnets = clone_str(l->strings[KSCF_SUBNETS], "subnets");
	}

	w->host_ikeport = l->options[KNCF_IKEPORT];
	w->protoport = l->protoport;

	if (l->certx != NULL) {
		w->cert = l->certx;
	}
	if (l->ckaid != NULL) {
		w->ckaid = l->ckaid;
	}
	if (l->pubkey_type == PUBKEY_PREEXCHANGED) {
		/*
		 * Only send over raw (prexchanged) rsapubkeys (i.e.,
		 * not %cert et.a.)
		 *
		 * XXX: but what is with the two rsasigkeys?  Whack seems
		 * to be willing to send pluto two raw pubkeys under
		 * the same ID.  Just assume that the first key should
		 * be used for the CKAID.
		 */
		passert(l->pubkey != NULL);
		passert(l->pubkey_alg != 0);
		w->pubkey_alg = l->pubkey_alg;
		w->pubkey = l->pubkey;
	}
	w->ca = l->ca;
	if (l->options_set[KNCF_SENDCERT])
		w->sendcert = l->options[KNCF_SENDCERT];
	else
		w->sendcert = CERT_ALWAYSSEND;

	if (l->options_set[KNCF_AUTH])
		w->auth = l->options[KNCF_AUTH];

	if (l->options_set[KNCF_EAP])
		w->eap = l->options[KNCF_EAP];
	else
		w->eap = IKE_EAP_NONE;

	w->updown = l->updown;
	w->virt = NULL;
	w->virt = l->virt;
	w->key_from_DNS_on_demand = l->key_from_DNS_on_demand;

	if (l->options_set[KNCF_XAUTHSERVER])
		w->xauth_server = l->options[KNCF_XAUTHSERVER];
	if (l->options_set[KNCF_XAUTHCLIENT])
		w->xauth_client = l->options[KNCF_XAUTHCLIENT];
	if (l->strings_set[KSCF_USERNAME])
		w->xauth_username = l->strings[KSCF_USERNAME];
	if (l->strings_set[KSCF_GROUNDHOG])
		w->groundhog = l->strings[KSCF_GROUNDHOG];

	if (l->options_set[KNCF_MODECONFIGSERVER])
		w->modecfg_server = l->options[KNCF_MODECONFIGSERVER];
	if (l->options_set[KNCF_MODECONFIGCLIENT])
		w->modecfg_client = l->options[KNCF_MODECONFIGCLIENT];
	if (l->options_set[KNCF_CAT])
		w->cat = l->options[KNCF_CAT];
	w->addresspool = l->addresspool;
	return true;
}

static int starter_whack_add_pubkey(struct starter_config *cfg,
				    const struct starter_conn *conn,
				    const struct starter_end *end,
				    struct logger *logger)
{
	int ret = 0;
	const char *lr = end->leftright;

	struct whack_message msg = empty_whack_message;
	msg.whack_key = true;
	msg.pubkey_alg = end->pubkey_alg;

	if (end->id && end->pubkey != NULL) {
		msg.keyid = end->id;

		switch (end->pubkey_type) {
		case PUBKEY_DNSONDEMAND:
			starter_log(LOG_LEVEL_DEBUG,
				"conn %s/%s has key from DNS",
				connection_name(conn), lr);
			break;

		case PUBKEY_CERTIFICATE:
			starter_log(LOG_LEVEL_DEBUG,
				"conn %s/%s has key from certificate",
				connection_name(conn), lr);
			break;

		case PUBKEY_NOTSET:
			break;

		case PUBKEY_PREEXCHANGED:
		{
			int base;
			switch (end->pubkey_alg) {
			case IPSECKEY_ALGORITHM_RSA:
			case IPSECKEY_ALGORITHM_ECDSA:
				base = 0; /* figure it out */
				break;
			case IPSECKEY_ALGORITHM_X_PUBKEY:
				base = 64; /* dam it */
				break;
			default:
				bad_case(end->pubkey_alg);
			}
			chunk_t keyspace = NULL_HUNK; /* must free */
			err_t err = ttochunk(shunk1(end->pubkey), base, &keyspace);
			if (err != NULL) {
				enum_buf pkb;
				starter_log(LOG_LEVEL_ERR,
					    "conn %s: %s%s malformed [%s]",
					    connection_name(conn), lr,
					    str_enum(&ipseckey_algorithm_config_names, end->pubkey_alg, &pkb),
					    err);
				return 1;
			}

			enum_buf pkb;
			starter_log(LOG_LEVEL_DEBUG,
				    "\tsending %s %s%s=%s",
				    connection_name(conn), lr,
				    str_enum(&ipseckey_algorithm_config_names, end->pubkey_alg, &pkb),
				    end->pubkey);
			msg.keyval = keyspace;
			ret = send_whack_msg(&msg, cfg->ctlsocket, logger);
			free_chunk_content(&keyspace);
		}
		}
	}

	if (ret < 0)
		return ret;

	return 0;
}

static void conn_log_val(const struct starter_conn *conn,
			 const char *name, const char *value)
{
	if (value != NULL) {
		starter_log(LOG_LEVEL_DEBUG, "conn: \"%s\" %s=%s",
			    conn->name, name, value);
	}
}

int starter_whack_add_conn(struct starter_config *cfg,
			   const struct starter_conn *conn,
			   struct logger *logger)
{
	struct whack_message msg = empty_whack_message;
	msg.whack_addconn = true;
	msg.name = connection_name(conn);

	msg.host_afi = conn->left.host_family;
	msg.child_afi = conn->clientaddrfamily;

	if (conn->right.addrtype == KH_IPHOSTNAME)
		msg.dnshostname = conn->right.strings[KSCF_IP];

	msg.nic_offload = conn->options[KNCF_NIC_OFFLOAD];
	if (conn->options_set[KNCF_IKELIFETIME_MS]) {
		msg.ikelifetime = deltatime_ms(conn->options[KNCF_IKELIFETIME_MS]);
	}
	if (conn->options_set[KNCF_IPSEC_LIFETIME_MS]) {
		msg.ipsec_lifetime = deltatime_ms(conn->options[KNCF_IPSEC_LIFETIME_MS]);
	}
	msg.sa_rekey_margin = deltatime_ms(conn->options[KNCF_REKEYMARGIN_MS]);
	msg.sa_ipsec_max_bytes = conn->options[KNCF_IPSEC_MAXBYTES];
	msg.sa_ipsec_max_packets = conn->options[KNCF_IPSEC_MAXPACKETS];
	msg.sa_rekeyfuzz_percent = conn->options[KNCF_REKEYFUZZ];
	if (conn->options_set[KNCF_KEYINGTRIES]) {
		msg.keyingtries.set = true;
		msg.keyingtries.value = conn->options[KNCF_KEYINGTRIES];
	}
	msg.replay_window = conn->options[KNCF_REPLAY_WINDOW]; /*has default*/
	msg.ipsec_interface = conn->strings[KSCF_IPSEC_INTERFACE];

	msg.retransmit_interval = deltatime_ms(conn->options[KNCF_RETRANSMIT_INTERVAL_MS]);
	msg.retransmit_timeout = deltatime_ms(conn->options[KNCF_RETRANSMIT_TIMEOUT_MS]);

	msg.ike_version = conn->ike_version;
	msg.ikev2 = conn->options[KNCF_IKEv2];
	msg.pfs = conn->options[KNCF_PFS];
	msg.compress = conn->options[KNCF_COMPRESS];
	enum keyword_satype satype = conn->options[KNCF_TYPE];
	msg.encap_mode = (satype == KS_TUNNEL ? ENCAP_MODE_TUNNEL :
			  satype == KS_TRANSPORT ? ENCAP_MODE_TRANSPORT :
			  ENCAP_MODE_UNSET);
	msg.phase2 = conn->options[KNCF_PHASE2];
	msg.authby = conn->authby;
	msg.sighash_policy = conn->sighash_policy;
	msg.never_negotiate_shunt = conn->never_negotiate_shunt;
	msg.negotiation_shunt = conn->negotiation_shunt;
	msg.failure_shunt = conn->failure_shunt;
	msg.autostart = conn->autostart;

	msg.connalias = conn->connalias;

	msg.metric = conn->options[KNCF_METRIC];

	msg.ikev2_allow_narrowing = conn->options[KNCF_IKEv2_ALLOW_NARROWING];
	msg.rekey = conn->options[KNCF_REKEY];
	msg.reauth = conn->options[KNCF_REAUTH];

	if (conn->options_set[KNCF_MTU])
		msg.mtu = conn->options[KNCF_MTU];
	if (conn->options_set[KNCF_PRIORITY])
		msg.priority = conn->options[KNCF_PRIORITY];
	if (conn->options_set[KNCF_TFC])
		msg.tfc = conn->options[KNCF_TFC];
	if (conn->options_set[KNCF_NO_ESP_TFC])
		msg.send_no_esp_tfc = conn->options[KNCF_NO_ESP_TFC];
	if (conn->options_set[KNCF_NFLOG_CONN])
		msg.nflog_group = conn->options[KNCF_NFLOG_CONN];

	if (conn->options_set[KNCF_REQID]) {
		if (conn->options[KNCF_REQID] <= 0 ||
		    conn->options[KNCF_REQID] > IPSEC_MANUAL_REQID_MAX) {
			starter_log(LOG_LEVEL_ERR,
				"Ignoring reqid value - range must be 1-%u",
				IPSEC_MANUAL_REQID_MAX);
		} else {
			msg.sa_reqid = conn->options[KNCF_REQID];
		}
	}

	if (conn->options_set[KNCF_TCP_REMOTEPORT]) {
		msg.tcp_remoteport = conn->options[KNCF_TCP_REMOTEPORT];
	}

	if (conn->options_set[KNCF_ENABLE_TCP]) {
		msg.enable_tcp = conn->options[KNCF_ENABLE_TCP];
	}

	/* default to HOLD */
	msg.dpd_action = (conn->options_set[KNCF_DPDACTION] ? conn->options[KNCF_DPDACTION] :
			  DPD_ACTION_UNSET);
	msg.dpd_delay = conn->dpd_delay;
	msg.dpd_timeout = conn->dpd_timeout;

	if (conn->options_set[KNCF_SEND_CA])
		msg.send_ca = conn->options[KNCF_SEND_CA];
	else
		msg.send_ca = CA_SEND_NONE;


	msg.encapsulation = conn->options[KNCF_ENCAPSULATION];

	if (conn->options_set[KNCF_NAT_KEEPALIVE])
		msg.nat_keepalive = conn->options[KNCF_NAT_KEEPALIVE];
	else
		msg.nat_keepalive = true;

	/* can be 0 aka unset */
	msg.nat_ikev1_method = conn->options[KNCF_NAT_IKEv1_METHOD];

	/* Activate sending out own vendorid */
	if (conn->options_set[KNCF_SEND_VENDORID])
		msg.send_vendorid = conn->options[KNCF_SEND_VENDORID];

	/* Activate Cisco quircky behaviour not replacing old IPsec SA's */
	if (conn->options_set[KNCF_INITIAL_CONTACT])
		msg.initial_contact = conn->options[KNCF_INITIAL_CONTACT];

	/* Activate their quircky behaviour - rumored to be needed for ModeCfg and RSA */
	if (conn->options_set[KNCF_CISCO_UNITY])
		msg.cisco_unity = conn->options[KNCF_CISCO_UNITY];

	if (conn->options_set[KNCF_VID_STRONGSWAN])
		msg.fake_strongswan = conn->options[KNCF_VID_STRONGSWAN];

	/* Active our Cisco interop code if set */
	msg.remote_peer_type = conn->options[KNCF_REMOTE_PEER_TYPE];

#ifdef HAVE_NM
	/* Network Manager support */
	msg.nm_configured = conn->options[KNCF_NM_CONFIGURED];
#endif

	if (conn->strings_set[KSCF_SEC_LABEL]) {
		msg.sec_label = conn->sec_label;
		starter_log(LOG_LEVEL_DEBUG, "conn: \"%s\" sec_label=%s",
			    conn->name, msg.sec_label);
	}

	msg.conn_debug = conn->options[KNCF_DEBUG];

	msg.modecfg_dns = conn->modecfg_dns;
	conn_log_val(conn, "modecfgdns", msg.modecfg_dns);
	msg.modecfg_domains = conn->modecfg_domains;
	conn_log_val(conn, "modecfgdomains", msg.modecfg_domains);
	msg.modecfg_banner = conn->modecfg_banner;
	conn_log_val(conn, "modecfgbanner", msg.modecfg_banner);

	msg.conn_mark_both = conn->conn_mark_both;
	conn_log_val(conn, "mark", msg.conn_mark_both);
	msg.conn_mark_in = conn->conn_mark_in;
	conn_log_val(conn, "mark-in", msg.conn_mark_in);
	msg.conn_mark_out = conn->conn_mark_out;
	conn_log_val(conn, "mark-out", msg.conn_mark_out);

	msg.vti_interface = conn->strings[KSCF_VTI_INTERFACE];
	conn_log_val(conn, "vti-interface", msg.vti_interface);
	msg.vti_routing = conn->options[KNCF_VTI_ROUTING];
	msg.vti_shared = conn->options[KNCF_VTI_SHARED];

	msg.ppk_ids = conn->ppk_ids;
	conn_log_val(conn, "ppk-ids", msg.ppk_ids);

	msg.redirect_to = conn->strings[KSCF_REDIRECT_TO];
	conn_log_val(conn, "redirect-to", msg.redirect_to);
	msg.accept_redirect_to = conn->strings[KSCF_ACCEPT_REDIRECT_TO];
	conn_log_val(conn, "accept-redirect-to", msg.accept_redirect_to);
	msg.send_redirect = conn->options[KNCF_SEND_REDIRECT];

	msg.mobike = conn->options[KNCF_MOBIKE]; /*yn_options*/
	msg.intermediate = conn->options[KNCF_INTERMEDIATE]; /*yn_options*/
	msg.sha2_truncbug = conn->options[KNCF_SHA2_TRUNCBUG]; /*yn_options*/
	msg.overlapip = conn->options[KNCF_OVERLAPIP]; /*yn_options*/
	msg.ms_dh_downgrade = conn->options[KNCF_MS_DH_DOWNGRADE]; /*yn_options*/
	msg.pfs_rekey_workaround = conn->options[KNCF_PFS_REKEY_WORKAROUND];
	msg.dns_match_id = conn->options[KNCF_DNS_MATCH_ID]; /* yn_options */
	msg.pam_authorize = conn->options[KNCF_PAM_AUTHORIZE]; /* yn_options */
	msg.ignore_peer_dns = conn->options[KNCF_IGNORE_PEER_DNS]; /* yn_options */
	msg.ikepad = conn->options[KNCF_IKEPAD]; /* yn_options */
	msg.require_id_on_certificate = conn->options[KNCF_REQUIRE_ID_ON_CERTIFICATE]; /* yn_options */
	msg.modecfgpull = conn->options[KNCF_MODECFGPULL]; /* yn_options */
	msg.aggressive = conn->options[KNCF_AGGRESSIVE]; /* yn_options */

	msg.iptfs = conn->options[KNCF_IPTFS]; /* yn_options */
	msg.iptfs_dont_frag = conn->options[KNCF_IPTFS_DONT_FRAG]; /* yn_options */
	if (conn->options_set[KNCF_IPTFS_PKT_SIZE])
		msg.iptfs_pkt_size = conn->options[KNCF_IPTFS_PKT_SIZE];
	if (conn->options_set[KNCF_IPTFS_MAX_QUEUE])
		msg.iptfs_max_qsize = conn->options[KNCF_IPTFS_MAX_QUEUE];
	if (conn->options_set[KNCF_IPTFS_IN_DELAY])
		msg.iptfs_in_delay = conn->options[KNCF_IPTFS_IN_DELAY];
	if (conn->options_set[KNCF_IPTFS_REORD_WIN])
		msg.iptfs_in_delay = conn->options[KNCF_IPTFS_REORD_WIN];
	if (conn->options_set[KNCF_IPTFS_DROP_TIME])
		msg.iptfs_in_delay = conn->options[KNCF_IPTFS_DROP_TIME];

	msg.decap_dscp = conn->options[KNCF_DECAP_DSCP]; /* yn_options */
	msg.encap_dscp = conn->options[KNCF_ENCAP_DSCP]; /* yn_options */
	msg.nopmtudisc = conn->options[KNCF_NOPMTUDISC]; /* yn_options */
	msg.accept_redirect = conn->options[KNCF_ACCEPT_REDIRECT]; /* yn_options */
	msg.fragmentation = conn->options[KNCF_FRAGMENTATION]; /* yna_options */
	msg.esn = conn->options[KNCF_ESN]; /* yne_options */
	msg.ppk = conn->options[KNCF_PPK]; /* nppi_options */

	if (conn->options_set[KNCF_XAUTHBY])
		msg.xauthby = conn->options[KNCF_XAUTHBY];
	if (conn->options_set[KNCF_XAUTHFAIL])
		msg.xauthfail = conn->options[KNCF_XAUTHFAIL];

	if (!set_whack_end(&msg.left, &conn->left))
		return -1;
	if (!set_whack_end(&msg.right, &conn->right))
		return -1;

	msg.esp = conn->esp;
	conn_log_val(conn, "esp", msg.esp);
	msg.ike = conn->ike_crypto;
	conn_log_val(conn, "ike", msg.ike);

	int r = send_whack_msg(&msg, cfg->ctlsocket, logger);
	if (r != 0)
		return r;

	if (conn->left.pubkey != NULL) {
		r = starter_whack_add_pubkey(cfg, conn, &conn->left, logger);
		if (r != 0)
			return r;
	}
	if (conn->right.pubkey != NULL) {
		r = starter_whack_add_pubkey(cfg, conn, &conn->right, logger);
		if (r != 0)
			return r;
	}

	return 0;
}

int starter_whack_route_conn(struct starter_config *cfg,
			     struct starter_conn *conn,
			     struct logger *logger)
{
	struct whack_message msg = empty_whack_message;
	msg.whack_route = true;
	msg.name = connection_name(conn);
	return send_whack_msg(&msg, cfg->ctlsocket, logger);
}

int starter_whack_initiate_conn(struct starter_config *cfg,
				struct starter_conn *conn,
				struct logger *logger)
{
	struct whack_message msg = empty_whack_message;
	msg.whack_initiate = true;
	msg.whack_async = true;
	msg.name = connection_name(conn);
	return send_whack_msg(&msg, cfg->ctlsocket, logger);
}

int starter_whack_listen(struct starter_config *cfg, struct logger *logger)
{
	struct whack_message msg = empty_whack_message;
	msg.whack_listen = true;
	return send_whack_msg(&msg, cfg->ctlsocket, logger);
}
