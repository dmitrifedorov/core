/* Copyright (c) 2009-2016 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "strescape.h"
#include "hostpid.h"
#include "istream.h"
#include "istream-concat.h"
#include "ostream.h"
#include "istream-dot.h"
#include "safe-mkstemp.h"
#include "hex-dec.h"
#include "time-util.h"
#include "var-expand.h"
#include "restrict-access.h"
#include "settings-parser.h"
#include "anvil-client.h"
#include "master-service.h"
#include "master-service-ssl.h"
#include "iostream-ssl.h"
#include "rfc822-parser.h"
#include "message-date.h"
#include "auth-master.h"
#include "mail-storage-service.h"
#include "index/raw/raw-storage.h"
#include "lda-settings.h"
#include "lmtp-settings.h"
#include "mail-namespace.h"
#include "mail-deliver.h"
#include "main.h"
#include "client.h"
#include "commands.h"
#include "lmtp-proxy.h"

#define ERRSTR_TEMP_MAILBOX_FAIL "451 4.3.0 <%s> Temporary internal error"
#define ERRSTR_TEMP_USERDB_FAIL_PREFIX "451 4.3.0 <%s> "
#define ERRSTR_TEMP_USERDB_FAIL \
	ERRSTR_TEMP_USERDB_FAIL_PREFIX "Temporary user lookup failure"

#define LMTP_PROXY_DEFAULT_TIMEOUT_MSECS (1000*125)

int cmd_lhlo(struct client *client, const char *args)
{
	struct rfc822_parser_context parser;
	string_t *domain = t_str_new(128);
	const char *p;
	int ret = 0;

	if (*args == '\0') {
		client_send_line(client, "501 Missing hostname");
		return 0;
	}

	/* domain / address-literal */
	rfc822_parser_init(&parser, (const unsigned char *)args, strlen(args),
			   NULL);
	if (*args != '[')
		ret = rfc822_parse_dot_atom(&parser, domain);
	else {
		for (p = args+1; *p != ']'; p++) {
			if (*p == '\\' || *p == '[')
				break;
		}
		if (strcmp(p, "]") != 0)
			ret = -1;
	}
	if (ret < 0) {
		str_truncate(domain, 0);
		str_append(domain, "invalid");
	}

	client_state_reset(client, "LHLO");
	client_send_line(client, "250-%s", client->my_domain);
	if (master_service_ssl_is_enabled(master_service) &&
	    client->ssl_iostream == NULL)
		client_send_line(client, "250-STARTTLS");
	if (client_is_trusted(client))
		client_send_line(client, "250-XCLIENT ADDR PORT TTL TIMEOUT");
	client_send_line(client, "250-8BITMIME");
	client_send_line(client, "250-ENHANCEDSTATUSCODES");
	client_send_line(client, "250 PIPELINING");

	i_free(client->lhlo);
	client->lhlo = i_strdup(str_c(domain));
	client_state_set(client, "LHLO", "");
	return 0;
}

int cmd_starttls(struct client *client)
{
	struct ostream *plain_output = client->output;
	const char *error;

	if (client->ssl_iostream != NULL) {
		o_stream_nsend_str(client->output,
				   "443 5.5.1 TLS is already active.\r\n");
		return 0;
	}

	if (master_service_ssl_init(master_service,
				    &client->input, &client->output,
				    &client->ssl_iostream, &error) < 0) {
		i_error("TLS initialization failed: %s", error);
		o_stream_nsend_str(client->output,
			"454 4.7.0 Internal error, TLS not available.\r\n");
		return 0;
	}
	o_stream_nsend_str(plain_output,
			   "220 2.0.0 Begin TLS negotiation now.\r\n");
	if (ssl_iostream_handshake(client->ssl_iostream) < 0) {
		client_destroy(client, NULL, NULL);
		return -1;
	}
	return 0;
}

static int parse_address(const char *str, const char **address_r,
			 const char **rest_r)
{
	const char *start;

	if (*str++ != '<')
		return -1;
	start = str;
	if (*str == '"') {
		/* "quoted-string"@domain */
		for (str++; *str != '"'; str++) {
			if (*str == '\\')
				str++;
			if (*str == '\0')
				return -1;
		}
		str++;
	}
	for (; *str != '>'; str++) {
		if (*str == '\0' || *str == ' ')
			return -1;
	}
	*address_r = t_strdup_until(start, str);
	if (*str++ != '>')
		return -1;

	if (*str == ' ')
		str++;
	else if (*str != '\0')
		return -1;
	*rest_r = str;
	return 0;
}

static const char *
parse_xtext(struct client *client, const char *value)
{
	const char *p;
	string_t *str;
	unsigned int i;

	p = strchr(value, '+');
	if (p == NULL)
		return p_strdup(client->state_pool, value);

	/*
	   hexchar = ASCII "+" immediately followed by two upper case
		     hexadecimal digits
	*/
	str = t_str_new(128);
	for (i = 0; value[i] != '\0'; i++) {
		if (value[i] == '+' && value[i+1] != '\0' && value[i+2] != '\0') {
			str_append_c(str, hex2dec((const void *)(value+i+1), 2));
			i += 2;
		} else {
			str_append_c(str, value[i]);
		}
	}
	return p_strdup(client->state_pool, str_c(str));
}

static void lmtp_anvil_init(void)
{
	if (anvil == NULL) {
		const char *path = t_strdup_printf("%s/anvil", base_dir);
		anvil = anvil_client_init(path, NULL, 0);
	}
}

int cmd_mail(struct client *client, const char *args)
{
	const char *addr, *const *argv;

	if (client->state.mail_from != NULL) {
		client_send_line(client, "503 5.5.1 MAIL already given");
		return 0;
	}

	if (strncasecmp(args, "FROM:", 5) != 0 ||
	    parse_address(args + 5, &addr, &args) < 0) {
		client_send_line(client, "501 5.5.4 Invalid parameters");
		return 0;
	}

	argv = t_strsplit(args, " ");
	for (; *argv != NULL; argv++) {
		if (strcasecmp(*argv, "BODY=7BIT") == 0)
			client->state.mail_body_7bit = TRUE;
		else if (strcasecmp(*argv, "BODY=8BITMIME") == 0)
			client->state.mail_body_8bitmime = TRUE;
		else {
			client_send_line(client,
				"501 5.5.4 Unsupported options");
			return 0;
		}
	}

	client->state.mail_from = p_strdup(client->state_pool, addr);
	p_array_init(&client->state.rcpt_to, client->state_pool, 64);
	client_send_line(client, "250 2.1.0 OK");
	client_state_set(client, "MAIL FROM", client->state.mail_from);

	if (client->lmtp_set->lmtp_user_concurrency_limit > 0) {
		/* connect to anvil before dropping privileges */
		lmtp_anvil_init();
	}

	client->state.mail_from_timeval = ioloop_timeval;
	return 0;
}

static bool
client_proxy_rcpt_parse_fields(struct lmtp_proxy_rcpt_settings *set,
			       const char *const *args, const char **address)
{
	const char *p, *key, *value;
	bool proxying = FALSE, port_set = FALSE;

	for (; *args != NULL; args++) {
		p = strchr(*args, '=');
		if (p == NULL) {
			key = *args;
			value = "";
		} else {
			key = t_strdup_until(*args, p);
			value = p + 1;
		}

		if (strcmp(key, "proxy") == 0)
			proxying = TRUE;
		else if (strcmp(key, "host") == 0)
			set->host = value;
		else if (strcmp(key, "port") == 0) {
			if (net_str2port(value, &set->port) < 0) {
				i_error("proxy: Invalid port number %s", value);
				return FALSE;
			}
			port_set = TRUE;
		} else if (strcmp(key, "proxy_timeout") == 0) {
			if (str_to_uint(value, &set->timeout_msecs) < 0) {
				i_error("proxy: Invalid proxy_timeout value %s", value);
				return FALSE;
			}
			set->timeout_msecs *= 1000;
		} else if (strcmp(key, "protocol") == 0) {
			if (strcmp(value, "lmtp") == 0) {
				set->protocol = LMTP_CLIENT_PROTOCOL_LMTP;
				if (!port_set)
					set->port = 24;
			} else if (strcmp(value, "smtp") == 0) {
				set->protocol = LMTP_CLIENT_PROTOCOL_SMTP;
				if (!port_set)
					set->port = 25;
			} else {
				i_error("proxy: Unknown protocol %s", value);
				return FALSE;
			}
		} else if (strcmp(key, "user") == 0 ||
			   strcmp(key, "destuser") == 0) {
			/* changing the username */
			*address = value;
		} else {
			/* just ignore it */
		}
	}
	if (proxying && set->host == NULL) {
		i_error("proxy: host not given");
		return FALSE;
	}
	return proxying;
}

static bool
client_proxy_is_ourself(const struct client *client,
			const struct lmtp_proxy_rcpt_settings *set)
{
	struct ip_addr ip;

	if (set->port != client->local_port)
		return FALSE;

	if (net_addr2ip(set->host, &ip) < 0)
		return FALSE;
	if (!net_ip_compare(&ip, &client->local_ip))
		return FALSE;
	return TRUE;
}

static const char *
address_add_detail(const char *username, char delim_c,
		   const char *detail)
{
	const char *domain;
	const char delim[] = {delim_c, '\0'};

	domain = strchr(username, '@');
	if (domain == NULL)
		return t_strconcat(username, delim, detail, NULL);
	else {
		username = t_strdup_until(username, domain);
		return t_strconcat(username, delim, detail, domain, NULL);
	}
}

static bool client_proxy_rcpt(struct client *client, const char *address,
			      const char *username, const char *detail, char delim,
			      const struct lmtp_recipient_params *params)
{
	struct auth_master_connection *auth_conn;
	struct lmtp_proxy_rcpt_settings set;
	struct auth_user_info info;
	struct mail_storage_service_input input;
	const char *args, *const *fields, *errstr, *orig_username = username;
	pool_t pool;
	int ret;

	memset(&input, 0, sizeof(input));
	input.module = input.service = "lmtp";
	mail_storage_service_init_settings(storage_service, &input);

	memset(&info, 0, sizeof(info));
	info.service = master_service_get_name(master_service);
	info.local_ip = client->local_ip;
	info.remote_ip = client->remote_ip;
	info.local_port = client->local_port;
	info.remote_port = client->remote_port;

	pool = pool_alloconly_create("auth lookup", 1024);
	auth_conn = mail_storage_service_get_auth_conn(storage_service);
	ret = auth_master_pass_lookup(auth_conn, username, &info,
				      pool, &fields);
	if (ret <= 0) {
		errstr = ret < 0 && fields[0] != NULL ? t_strdup(fields[0]) :
			t_strdup_printf(ERRSTR_TEMP_USERDB_FAIL, address);
		pool_unref(&pool);
		if (ret < 0) {
			client_send_line(client, "%s", errstr);
			return TRUE;
		} else {
			/* user not found from passdb. try userdb also. */
			return FALSE;
		}
	}

	memset(&set, 0, sizeof(set));
	set.port = client->local_port;
	set.protocol = LMTP_CLIENT_PROTOCOL_LMTP;
	set.timeout_msecs = LMTP_PROXY_DEFAULT_TIMEOUT_MSECS;
	set.params = *params;

	if (!client_proxy_rcpt_parse_fields(&set, fields, &username)) {
		/* not proxying this user */
		pool_unref(&pool);
		return FALSE;
	}
	if (strcmp(username, orig_username) != 0) {
		/* username changed. change the address as well */
		if (*detail == '\0')
			address = username;
		else
			address = address_add_detail(username, delim, detail);
	} else if (client_proxy_is_ourself(client, &set)) {
		i_error("Proxying to <%s> loops to itself", username);
		client_send_line(client, "554 5.4.6 <%s> "
				 "Proxying loops to itself", address);
		pool_unref(&pool);
		return TRUE;
	}

	if (client->proxy_ttl <= 1) {
		i_error("Proxying to <%s> appears to be looping (TTL=0)",
			username);
		client_send_line(client, "554 5.4.6 <%s> "
				 "Proxying appears to be looping (TTL=0)",
				 username);
		pool_unref(&pool);
		return TRUE;
	}
	if (array_count(&client->state.rcpt_to) != 0) {
		client_send_line(client, "451 4.3.0 <%s> "
			"Can't handle mixed proxy/non-proxy destinations",
			address);
		pool_unref(&pool);
		return TRUE;
	}
	if (client->proxy == NULL) {
		struct lmtp_proxy_settings proxy_set;

		memset(&proxy_set, 0, sizeof(proxy_set));
		proxy_set.my_hostname = client->my_domain;
		proxy_set.dns_client_socket_path = dns_client_socket_path;
		proxy_set.session_id = client->state.session_id;
		proxy_set.source_ip = client->remote_ip;
		proxy_set.source_port = client->remote_port;
		proxy_set.proxy_ttl = client->proxy_ttl-1;

		client->proxy = lmtp_proxy_init(&proxy_set, client->output);
		if (client->state.mail_body_8bitmime)
			args = " BODY=8BITMIME";
		else if (client->state.mail_body_7bit)
			args = " BODY=7BIT";
		else
			args = "";
		lmtp_proxy_mail_from(client->proxy, t_strdup_printf(
			"<%s>%s", client->state.mail_from, args));
	}
	if (lmtp_proxy_add_rcpt(client->proxy, address, &set) < 0)
		client_send_line(client, ERRSTR_TEMP_REMOTE_FAILURE);
	else
		client_send_line(client, "250 2.1.5 OK");
	pool_unref(&pool);
	return TRUE;
}

static const char *lmtp_unescape_address(const char *name)
{
	string_t *str;
	const char *p;

	if (*name != '"')
		return name;

	/* quoted-string local-part. drop the quotes unless there's a
	   '@' character inside or there's an error. */
	str = t_str_new(128);
	for (p = name+1; *p != '"'; p++) {
		if (*p == '\0')
			return name;
		if (*p == '\\') {
			if (p[1] == '\0') {
				/* error */
				return name;
			}
			p++;
		}
		if (*p == '@')
			return name;
		str_append_c(str, *p);
	}
	p++;
	if (*p != '@' && *p != '\0')
		return name;

	str_append(str, p);
	return str_c(str);
}

static void rcpt_address_parse(struct client *client, const char *address,
			       const char **username_r, char *delim_r,
			       const char **detail_r)
{
	const char *p, *domain;
	size_t idx;

	*username_r = address;
	*detail_r = "";

	if (*client->unexpanded_lda_set->recipient_delimiter == '\0')
		return;

	domain = strchr(address, '@');
	/* first character that matches the recipient_delimiter */
	idx = strcspn(address, client->unexpanded_lda_set->recipient_delimiter);
	p = address[idx] != '\0' ? address + idx : NULL;

	if (p != NULL && (domain == NULL || p < domain)) {
		*delim_r = *p;
		/* user+detail@domain */
		*username_r = t_strdup_until(*username_r, p);
		if (domain == NULL)
			*detail_r = p+1;
		else {
			*detail_r = t_strdup_until(p+1, domain);
			*username_r = t_strconcat(*username_r, domain, NULL);
		}
	}
}

static void lmtp_address_translate(struct client *client, const char **address)
{
	const char *transpos = client->lmtp_set->lmtp_address_translate;
	const char *p, *nextstr, *addrpos = *address;
	unsigned int len;
	string_t *username, *domain, *dest = NULL;

	if (*transpos == '\0')
		return;

	username = t_str_new(64);
	domain = t_str_new(64);

	/* check that string matches up to the first '%' */
	p = strchr(transpos, '%');
	if (p == NULL)
		len = strlen(transpos);
	else
		len = p-transpos;
	if (strncmp(transpos, addrpos, len) != 0)
		return;
	transpos += len;
	addrpos += len;

	while (*transpos != '\0') {
		switch (transpos[1]) {
		case 'n':
		case 'u':
			dest = username;
			break;
		case 'd':
			dest = domain;
			break;
		default:
			return;
		}
		transpos += 2;

		/* find where the next string starts */
		if (*transpos == '\0') {
			str_append(dest, addrpos);
			break;
		}
		p = strchr(transpos, '%');
		if (p == NULL)
			nextstr = transpos;
		else
			nextstr = t_strdup_until(transpos, p);
		p = strstr(addrpos, nextstr);
		if (p == NULL)
			return;
		str_append_n(dest, addrpos, p-addrpos);

		len = strlen(nextstr);
		transpos += len;
		addrpos = p + len;
	}
	str_append_c(username, '@');
	if (domain != NULL)
		str_append_str(username, domain);
	*address = str_c(username);
}

static void
client_send_line_overquota(struct client *client,
			   const struct mail_recipient *rcpt, const char *error)
{
	struct lda_settings *lda_set =
		mail_storage_service_user_get_set(rcpt->service_user)[1];

	client_send_line(client, "%s <%s> %s", lda_set->quota_full_tempfail ?
			 "452 4.2.2" : "552 5.2.2", rcpt->address, error);
}

static int
lmtp_rcpt_to_is_over_quota(struct client *client,
			   const struct mail_recipient *rcpt)
{
	struct mail_user *user;
	struct mail_namespace *ns;
	struct mailbox *box;
	struct mailbox_status status;
	const char *errstr;
	enum mail_error error;
	int ret;

	if (!client->lmtp_set->lmtp_rcpt_check_quota)
		return 0;

	ret = mail_storage_service_next(storage_service,
					rcpt->service_user, &user, &errstr);
	if (ret < 0) {
		i_error("Failed to initialize user %s: %s", rcpt->address, errstr);
		return -1;
	}

	ns = mail_namespace_find_inbox(user->namespaces);
	box = mailbox_alloc(ns->list, "INBOX", 0);
	ret = mailbox_get_status(box, STATUS_CHECK_OVER_QUOTA, &status);
	if (ret < 0) {
		errstr = mailbox_get_last_error(box, &error);
		if (error == MAIL_ERROR_NOQUOTA) {
			client_send_line_overquota(client, rcpt, errstr);
			ret = 1;
		}
	}
	mailbox_free(&box);
	mail_user_unref(&user);
	return ret;
}

static bool cmd_rcpt_finish(struct client *client, struct mail_recipient *rcpt)
{
	int ret;

	if ((ret = lmtp_rcpt_to_is_over_quota(client, rcpt)) != 0) {
		if (ret < 0) {
			client_send_line(client, ERRSTR_TEMP_MAILBOX_FAIL,
					 rcpt->address);
		}
		mail_storage_service_user_free(&rcpt->service_user);
		return FALSE;
	}
	array_append(&client->state.rcpt_to, &rcpt, 1);
	client_send_line(client, "250 2.1.5 OK");
	return TRUE;
}

static void rcpt_anvil_lookup_callback(const char *reply, void *context)
{
	struct mail_recipient *rcpt = context;
	struct client *client = rcpt->client;
	const struct mail_storage_service_input *input;
	unsigned int parallel_count = 0;

	rcpt->anvil_query = NULL;
	if (reply == NULL) {
		/* lookup failed */
	} else if (str_to_uint(reply, &parallel_count) < 0) {
		i_error("Invalid reply from anvil: %s", reply);
	}

	if (parallel_count >= client->lmtp_set->lmtp_user_concurrency_limit) {
		client_send_line(client, ERRSTR_TEMP_USERDB_FAIL_PREFIX
				 "Too many concurrent deliveries for user",
				 rcpt->address);
		mail_storage_service_user_free(&rcpt->service_user);
	} else if (cmd_rcpt_finish(client, rcpt)) {
		rcpt->anvil_connect_sent = TRUE;
		input = mail_storage_service_user_get_input(rcpt->service_user);
		master_service_anvil_send(master_service, t_strconcat(
			"CONNECT\t", my_pid, "\t", master_service_get_name(master_service),
			"/", input->username, "\n", NULL));
	}

	client_io_reset(client);
	client_input_handle(client);
}

int cmd_rcpt(struct client *client, const char *args)
{
	struct mail_recipient *rcpt;
	struct mail_storage_service_input input;
	const char *params, *address, *username, *detail;
	const char *const *argv;
	const char *error = NULL;
	char delim = '\0';
	int ret = 0;

	if (client->state.mail_from == NULL) {
		client_send_line(client, "503 5.5.1 MAIL needed first");
		return 0;
	}

	if (strncasecmp(args, "TO:", 3) != 0 ||
	    parse_address(args + 3, &address, &params) < 0) {
		client_send_line(client, "501 5.5.4 Invalid parameters");
		return 0;
	}

	rcpt = p_new(client->state_pool, struct mail_recipient, 1);
	rcpt->client = client;
	address = lmtp_unescape_address(address);

	argv = t_strsplit(params, " ");
	for (; *argv != NULL; argv++) {
		if (strncasecmp(*argv, "ORCPT=", 6) == 0) {
			rcpt->params.dsn_orcpt = parse_xtext(client, *argv + 6);
		} else {
			client_send_line(client, "501 5.5.4 Unsupported options");
			return 0;
		}
	}
	rcpt_address_parse(client, address, &username, &delim, &detail);

	client_state_set(client, "RCPT TO", address);

	if (client->lmtp_set->lmtp_proxy) {
		if (client_proxy_rcpt(client, address, username, detail, delim,
				      &rcpt->params))
			return 0;
	}

	/* Use a unique session_id for each mail delivery. This is especially
	   important for stats process to not see duplicate sessions. */
	if (array_count(&client->state.rcpt_to) == 0)
		rcpt->session_id = client->state.session_id;
	else {
		rcpt->session_id =
			p_strdup_printf(client->state_pool, "%s:%u",
					client->state.session_id,
					array_count(&client->state.rcpt_to)+1);
	}

	memset(&input, 0, sizeof(input));
	input.module = input.service = "lmtp";
	input.username = username;
	input.local_ip = client->local_ip;
	input.remote_ip = client->remote_ip;
	input.local_port = client->local_port;
	input.remote_port = client->remote_port;
	input.session_id = rcpt->session_id;

	ret = mail_storage_service_lookup(storage_service, &input,
					  &rcpt->service_user, &error);

	if (ret < 0) {
		i_error("Failed to lookup user %s: %s", username, error);
		client_send_line(client, ERRSTR_TEMP_MAILBOX_FAIL, address);
		return 0;
	}
	if (ret == 0) {
		client_send_line(client,
				 "550 5.1.1 <%s> User doesn't exist: %s",
				 address, username);
		return 0;
	}
	if (client->proxy != NULL) {
		/* NOTE: if this restriction is ever removed, we'll also need
		   to send different message bodies to local and proxy
		   (with and without Return-Path: header) */
		client_send_line(client, "451 4.3.0 <%s> "
			"Can't handle mixed proxy/non-proxy destinations",
			address);
		mail_storage_service_user_free(&rcpt->service_user);
		return 0;
	}

	lmtp_address_translate(client, &address);

	rcpt->address = p_strdup(client->state_pool, address);
	rcpt->detail = p_strdup(client->state_pool, detail);

	if (client->lmtp_set->lmtp_user_concurrency_limit == 0) {
		(void)cmd_rcpt_finish(client, rcpt);
		return 0;
	} else {
		const char *query = t_strconcat("LOOKUP\t",
			master_service_get_name(master_service),
			"/", str_tabescape(username), NULL);
		io_remove(&client->io);
		rcpt->anvil_query = anvil_client_query(anvil, query,
					rcpt_anvil_lookup_callback, rcpt);
		/* stop processing further commands while anvil query is
		   pending */
		return rcpt->anvil_query == NULL ? 0 : -1;
	}
}

int cmd_quit(struct client *client, const char *args ATTR_UNUSED)
{
	client_send_line(client, "221 2.0.0 OK");
	/* don't log the (state name) for successful QUITs */
	i_info("Disconnect from %s: Successful quit", client_remote_id(client));
	client->disconnected = TRUE;
	client_destroy(client, NULL, NULL);
	return -1;
}

int cmd_vrfy(struct client *client, const char *args ATTR_UNUSED)
{
	client_send_line(client, "252 2.3.3 Try RCPT instead");
	return 0;
}

int cmd_rset(struct client *client, const char *args ATTR_UNUSED)
{
	client_state_reset(client, "RSET");
	client_send_line(client, "250 2.0.0 OK");
	return 0;
}

int cmd_noop(struct client *client, const char *args ATTR_UNUSED)
{
	client_send_line(client, "250 2.0.0 OK");
	return 0;
}

static bool orcpt_get_valid_rfc822(const char *orcpt, const char **addr_r)
{
	if (orcpt == NULL || strncasecmp(orcpt, "rfc822;", 7) != 0)
		return FALSE;
	/* FIXME: we should verify the address further */
	*addr_r = orcpt + 7;
	return TRUE;
}

static int
client_deliver(struct client *client, const struct mail_recipient *rcpt,
	       struct mail *src_mail, struct mail_deliver_session *session)
{
	struct mail_deliver_context dctx;
	struct mail_storage *storage;
	const struct mail_storage_service_input *input;
	const struct mail_storage_settings *mail_set;
	struct lda_settings *lda_set;
	struct mail_namespace *ns;
	struct setting_parser_context *set_parser;
	struct timeval delivery_time_started;
	void **sets;
	const char *line, *error, *username;
	string_t *str;
	enum mail_error mail_error;
	int ret;

	input = mail_storage_service_user_get_input(rcpt->service_user);
	username = t_strdup(input->username);

	mail_set = mail_storage_service_user_get_mail_set(rcpt->service_user);
	set_parser = mail_storage_service_user_get_settings_parser(rcpt->service_user);
	if (client->proxy_timeout_secs > 0 &&
	    (mail_set->mail_max_lock_timeout == 0 ||
	     mail_set->mail_max_lock_timeout > client->proxy_timeout_secs)) {
		/* set lock timeout waits to be less than when proxy has
		   advertised that it's going to timeout the connection.
		   this avoids duplicate deliveries in case the delivery
		   succeeds after the proxy has already disconnected from us. */
		line = t_strdup_printf("mail_max_lock_timeout=%u",
				       client->proxy_timeout_secs <= 1 ? 1 :
				       client->proxy_timeout_secs-1);
		if (settings_parse_line(set_parser, line) < 0)
			i_unreached();
	}

	/* get the timestamp before user is created, since it starts the I/O */
	io_loop_time_refresh();
	delivery_time_started = ioloop_timeval;

	client_state_set(client, "DATA", username);
	i_set_failure_prefix("lmtp(%s, %s): ", my_pid, username);
	if (mail_storage_service_next(storage_service, rcpt->service_user,
				      &client->state.dest_user, &error) < 0) {
		i_error("Failed to initialize user: %s", error);
		client_send_line(client, ERRSTR_TEMP_MAILBOX_FAIL,
				 rcpt->address);
		return -1;
	}

	sets = mail_storage_service_user_get_set(rcpt->service_user);
	lda_set = sets[1];
	if (settings_var_expand(&lda_setting_parser_info, lda_set, client->pool,
			mail_user_var_expand_table(client->state.dest_user), &error) <= 0) {
		i_error("Failed to expand settings: %s", error);
		client_send_line(client, ERRSTR_TEMP_MAILBOX_FAIL,
				 rcpt->address);
		return -1;
	}

	str = t_str_new(256);
	if (var_expand_with_funcs(str, client->state.dest_user->set->mail_log_prefix,
				  mail_user_var_expand_table(client->state.dest_user),
				  mail_user_var_expand_func_table,
				  client->state.dest_user, &error) <= 0) {
		i_error("Failed to expand mail_log_prefix=%s: %s",
			client->state.dest_user->set->mail_log_prefix, error);
		client_send_line(client, ERRSTR_TEMP_MAILBOX_FAIL,
				 rcpt->address);
		return -1;
	}
	i_set_failure_prefix("%s", str_c(str));

	memset(&dctx, 0, sizeof(dctx));
	dctx.session = session;
	dctx.pool = session->pool;
	dctx.set = lda_set;
	dctx.timeout_secs = LDA_SUBMISSION_TIMEOUT_SECS;
	dctx.session_id = rcpt->session_id;
	dctx.src_mail = src_mail;
	dctx.src_envelope_sender = client->state.mail_from;
	dctx.dest_user = client->state.dest_user;
	dctx.session_time_msecs =
		timeval_diff_msecs(&client->state.data_end_timeval,
				   &client->state.mail_from_timeval);
	dctx.delivery_time_started = delivery_time_started;

	if (orcpt_get_valid_rfc822(rcpt->params.dsn_orcpt, &dctx.dest_addr)) {
		/* used ORCPT */
	} else if (*dctx.set->lda_original_recipient_header != '\0') {
		dctx.dest_addr = mail_deliver_get_address(src_mail,
				dctx.set->lda_original_recipient_header);
	}
	if (dctx.dest_addr == NULL)
		dctx.dest_addr = rcpt->address;
	dctx.final_dest_addr = rcpt->address;
	if (*rcpt->detail == '\0' ||
	    !client->lmtp_set->lmtp_save_to_detail_mailbox)
		dctx.dest_mailbox_name = "INBOX";
	else {
		ns = mail_namespace_find_inbox(dctx.dest_user->namespaces);
		dctx.dest_mailbox_name =
			t_strconcat(ns->prefix, rcpt->detail, NULL);
	}

	dctx.save_dest_mail = array_count(&client->state.rcpt_to) > 1 &&
		client->state.first_saved_mail == NULL;

	if (mail_deliver(&dctx, &storage) == 0) {
		if (dctx.dest_mail != NULL) {
			i_assert(client->state.first_saved_mail == NULL);
			client->state.first_saved_mail = dctx.dest_mail;
		}
		client_send_line(client, "250 2.0.0 <%s> %s Saved",
				 rcpt->address, rcpt->session_id);
		ret = 0;
	} else if (dctx.tempfail_error != NULL) {
		client_send_line(client, "451 4.2.0 <%s> %s",
				 rcpt->address, dctx.tempfail_error);
		ret = -1;
	} else if (storage != NULL) {
		error = mail_storage_get_last_error(storage, &mail_error);
		if (mail_error == MAIL_ERROR_NOQUOTA) {
			client_send_line_overquota(client, rcpt, error);
		} else {
			client_send_line(client, "451 4.2.0 <%s> %s",
					 rcpt->address, error);
		}
		ret = -1;
	} else {
		/* This shouldn't happen */
		i_error("BUG: Saving failed to unknown storage");
		client_send_line(client, ERRSTR_TEMP_MAILBOX_FAIL,
				 rcpt->address);
		ret = -1;
	}
	return ret;
}

static bool client_deliver_next(struct client *client, struct mail *src_mail,
				struct mail_deliver_session *session)
{
	struct mail_recipient *const *rcpts;
	unsigned int count;
	int ret;

	rcpts = array_get(&client->state.rcpt_to, &count);
	while (client->state.rcpt_idx < count) {
		ret = client_deliver(client, rcpts[client->state.rcpt_idx],
				     src_mail, session);
		client_state_set(client, "DATA", "");
		i_set_failure_prefix("lmtp(%s): ", my_pid);

		client->state.rcpt_idx++;
		if (ret == 0)
			return TRUE;
		/* failed. try the next one. */
		if (client->state.dest_user != NULL)
			mail_user_unref(&client->state.dest_user);
	}
	return FALSE;
}

static void client_rcpt_fail_all(struct client *client)
{
	struct mail_recipient *const *rcptp;

	array_foreach(&client->state.rcpt_to, rcptp) {
		client_send_line(client, ERRSTR_TEMP_MAILBOX_FAIL,
				 (*rcptp)->address);
	}
}

static struct istream *client_get_input(struct client *client)
{
	struct client_state *state = &client->state;
	struct istream *cinput, *inputs[3];

	inputs[0] = i_stream_create_from_data(state->added_headers,
					      strlen(state->added_headers));

	if (state->mail_data_output != NULL) {
		o_stream_unref(&state->mail_data_output);
		inputs[1] = i_stream_create_fd(state->mail_data_fd,
					       MAIL_READ_FULL_BLOCK_SIZE);
		i_stream_set_init_buffer_size(inputs[1],
					      MAIL_READ_FULL_BLOCK_SIZE);
	} else {
		inputs[1] = i_stream_create_from_data(state->mail_data->data,
						      state->mail_data->used);
	}
	inputs[2] = NULL;

	cinput = i_stream_create_concat(inputs);
	i_stream_set_name(cinput, "<lmtp DATA>");
	i_stream_unref(&inputs[0]);
	i_stream_unref(&inputs[1]);
	return cinput;
}

static int client_open_raw_mail(struct client *client, struct istream *input)
{
	static const char *wanted_headers[] = {
		"From", "To", "Message-ID", "Subject", "Return-Path",
		NULL
	};
	struct mailbox *box;
	struct mailbox_transaction_context *trans;
	struct mailbox_header_lookup_ctx *headers_ctx;
	enum mail_error error;

	if (raw_mailbox_alloc_stream(client->raw_mail_user, input,
				     (time_t)-1, client->state.mail_from,
				     &box) < 0) {
		i_error("Can't open delivery mail as raw: %s",
			mailbox_get_last_error(box, &error));
		mailbox_free(&box);
		client_rcpt_fail_all(client);
		return -1;
	}

	trans = mailbox_transaction_begin(box, 0);

	headers_ctx = mailbox_header_lookup_init(box, wanted_headers);
	client->state.raw_mail = mail_alloc(trans, 0, headers_ctx);
	mailbox_header_lookup_unref(&headers_ctx);
	mail_set_seq(client->state.raw_mail, 1);
	return 0;
}

static void
client_input_data_write_local(struct client *client, struct istream *input)
{
	struct mail_deliver_session *session;
	struct mail *src_mail;
	uid_t old_uid, first_uid = (uid_t)-1;

	if (client_open_raw_mail(client, input) < 0)
		return;

	session = mail_deliver_session_init();
	old_uid = geteuid();
	src_mail = client->state.raw_mail;
	while (client_deliver_next(client, src_mail, session)) {
		if (client->state.first_saved_mail == NULL ||
		    client->state.first_saved_mail == src_mail)
			mail_user_unref(&client->state.dest_user);
		else {
			/* use the first saved message to save it elsewhere too.
			   this might allow hard linking the files. */
			client->state.dest_user = NULL;
			src_mail = client->state.first_saved_mail;
			first_uid = geteuid();
			i_assert(first_uid != 0);
		}
	}
	mail_deliver_session_deinit(&session);

	if (client->state.first_saved_mail != NULL) {
		struct mail *mail = client->state.first_saved_mail;
		struct mailbox_transaction_context *trans = mail->transaction;
		struct mailbox *box = trans->box;
		struct mail_user *user = box->storage->user;

		/* just in case these functions are going to write anything,
		   change uid back to user's own one */
		if (first_uid != old_uid) {
			if (seteuid(0) < 0)
				i_fatal("seteuid(0) failed: %m");
			if (seteuid(first_uid) < 0)
				i_fatal("seteuid() failed: %m");
		}

		mail_free(&mail);
		mailbox_transaction_rollback(&trans);
		mailbox_free(&box);
		mail_user_unref(&user);
	}

	if (old_uid == 0) {
		/* switch back to running as root, since that's what we're
		   practically doing anyway. it's also important in case we
		   lose e.g. config connection and need to reconnect to it. */
		if (seteuid(0) < 0)
			i_fatal("seteuid(0) failed: %m");
		/* enable core dumping again. we need to chdir also to
		   root-owned directory to get core dumps. */
		restrict_access_allow_coredumps(TRUE);
		if (chdir(base_dir) < 0)
			i_error("chdir(%s) failed: %m", base_dir);
	}
}

static void client_input_data_finish(struct client *client)
{
	client_io_reset(client);
	client_state_reset(client, "DATA finished");
	if (i_stream_have_bytes_left(client->input))
		client_input_handle(client);
}

static void client_proxy_finish(void *context)
{
	struct client *client = context;

	lmtp_proxy_deinit(&client->proxy);
	client_input_data_finish(client);
}

static const char *client_get_added_headers(struct client *client)
{
	string_t *str = t_str_new(200);
	void **sets;
	const struct lmtp_settings *lmtp_set;
	const char *host, *rcpt_to = NULL;

	if (array_count(&client->state.rcpt_to) == 1) {
		struct mail_recipient *const *rcptp =
			array_idx(&client->state.rcpt_to, 0);

		sets = mail_storage_service_user_get_set((*rcptp)->service_user);
		lmtp_set = sets[2];

		switch (lmtp_set->parsed_lmtp_hdr_delivery_address) {
		case LMTP_HDR_DELIVERY_ADDRESS_NONE:
			break;
		case LMTP_HDR_DELIVERY_ADDRESS_FINAL:
			rcpt_to = (*rcptp)->address;
			break;
		case LMTP_HDR_DELIVERY_ADDRESS_ORIGINAL:
			if (!orcpt_get_valid_rfc822((*rcptp)->params.dsn_orcpt,
						    &rcpt_to))
				rcpt_to = (*rcptp)->address;
			break;
		}
	}

	/* don't set Return-Path when proxying so it won't get added twice */
	if (array_count(&client->state.rcpt_to) > 0) {
		str_printfa(str, "Return-Path: <%s>\r\n",
			    client->state.mail_from);
		if (rcpt_to != NULL)
			str_printfa(str, "Delivered-To: %s\r\n", rcpt_to);
	}

	str_printfa(str, "Received: from %s", client->lhlo);
	host = net_ip2addr(&client->remote_ip);
	if (host[0] != '\0')
		str_printfa(str, " ([%s])", host);
	str_append(str, "\r\n");
	if (client->ssl_iostream != NULL) {
		str_printfa(str, "\t(using %s)\r\n",
			    ssl_iostream_get_security_string(client->ssl_iostream));
	}
	str_printfa(str, "\tby %s with LMTP id %s",
		    client->my_domain, client->state.session_id);

	str_append(str, "\r\n\t");
	if (rcpt_to != NULL)
		str_printfa(str, "for <%s>", rcpt_to);
	str_printfa(str, "; %s\r\n", message_date_create(ioloop_time));
	return str_c(str);
}

static void client_input_data_write(struct client *client)
{
	struct istream *input;

	/* stop handling client input until saving/proxying is finished */
	if (client->to_idle != NULL)
		timeout_remove(&client->to_idle);
	io_remove(&client->io);
	i_stream_destroy(&client->dot_input);

	client->state.data_end_timeval = ioloop_timeval;

	input = client_get_input(client);
	if (array_count(&client->state.rcpt_to) != 0)
		client_input_data_write_local(client, input);
	if (client->proxy != NULL) {
		client_state_set(client, "DATA", "proxying");
		lmtp_proxy_start(client->proxy, input,
				 client_proxy_finish, client);
	} else {
		client_input_data_finish(client);
	}
	i_stream_unref(&input);
}

static int client_input_add_file(struct client *client,
				 const unsigned char *data, size_t size)
{
	struct client_state *state = &client->state;
	string_t *path;
	int fd;

	if (state->mail_data_output != NULL) {
		/* continue writing to file */
		if (o_stream_send(state->mail_data_output,
				  data, size) != (ssize_t)size)
			return -1;
		return 0;
	}

	/* move everything to a temporary file. */
	path = t_str_new(256);
	mail_user_set_get_temp_prefix(path, client->raw_mail_user->set);
	fd = safe_mkstemp_hostpid(path, 0600, (uid_t)-1, (gid_t)-1);
	if (fd == -1) {
		i_error("Temp file creation to %s failed: %m", str_c(path));
		return -1;
	}

	/* we just want the fd, unlink it */
	if (i_unlink(str_c(path)) < 0) {
		/* shouldn't happen.. */
		i_close_fd(&fd);
		return -1;
	}

	state->mail_data_fd = fd;
	state->mail_data_output = o_stream_create_fd_file(fd, 0, FALSE);
	o_stream_set_name(state->mail_data_output, str_c(path));
	o_stream_cork(state->mail_data_output);

	o_stream_nsend(state->mail_data_output,
		       state->mail_data->data, state->mail_data->used);
	o_stream_nsend(client->state.mail_data_output, data, size);
	if (o_stream_nfinish(client->state.mail_data_output) < 0) {
		i_error("write(%s) failed: %s", str_c(path),
			o_stream_get_error(client->state.mail_data_output));
		return -1;
	}
	return 0;
}

static int
client_input_add(struct client *client, const unsigned char *data, size_t size)
{
	if (client->state.mail_data->used + size <=
	    CLIENT_MAIL_DATA_MAX_INMEMORY_SIZE &&
	    client->state.mail_data_output == NULL) {
		buffer_append(client->state.mail_data, data, size);
		return 0;
	} else {
		return client_input_add_file(client, data, size);
	}
}

static void client_input_data_handle(struct client *client)
{
	const unsigned char *data;
	size_t size;
	ssize_t ret;

	while ((ret = i_stream_read(client->dot_input)) > 0 || ret == -2) {
		data = i_stream_get_data(client->dot_input, &size);
		if (client_input_add(client, data, size) < 0) {
			client_destroy(client, "451 4.3.0",
				       "Temporary internal failure");
			return;
		}
		i_stream_skip(client->dot_input, size);
	}
	if (ret == 0)
		return;

	if (!client->dot_input->eof) {
		/* client probably disconnected */
		client_destroy(client, NULL, NULL);
		return;
	}

	client_input_data_write(client);
}

static void client_input_data(struct client *client)
{
	if (client_input_read(client) < 0)
		return;

	client_input_data_handle(client);
}

int cmd_data(struct client *client, const char *args ATTR_UNUSED)
{
	if (client->state.mail_from == NULL) {
		client_send_line(client, "503 5.5.1 MAIL needed first");
		return 0;
	}
	if (array_count(&client->state.rcpt_to) == 0 && client->proxy == NULL) {
		client_send_line(client, "554 5.5.1 No valid recipients");
		return 0;
	}

	client->state.added_headers =
		p_strdup(client->state_pool, client_get_added_headers(client));

	i_assert(client->state.mail_data == NULL);
	client->state.mail_data = buffer_create_dynamic(default_pool, 1024*64);

	i_assert(client->dot_input == NULL);
	client->dot_input = i_stream_create_dot(client->input, TRUE);
	client_send_line(client, "354 OK");
	/* send the DATA reply immediately before we start handling any data */
	o_stream_uncork(client->output);

	io_remove(&client->io);
	client_state_set(client, "DATA", "");
	client->io = io_add(client->fd_in, IO_READ, client_input_data, client);
	client_input_data_handle(client);
	return -1;
}

int cmd_xclient(struct client *client, const char *args)
{
	const char *const *tmp;
	struct ip_addr remote_ip;
	in_port_t remote_port = 0;
	unsigned int ttl = UINT_MAX, timeout_secs = 0;
	bool args_ok = TRUE;

	if (!client_is_trusted(client)) {
		client_send_line(client, "550 You are not from trusted IP");
		return 0;
	}
	remote_ip.family = 0;
	for (tmp = t_strsplit(args, " "); *tmp != NULL; tmp++) {
		if (strncasecmp(*tmp, "ADDR=", 5) == 0) {
			if (net_addr2ip(*tmp + 5, &remote_ip) < 0)
				args_ok = FALSE;
		} else if (strncasecmp(*tmp, "PORT=", 5) == 0) {
			if (net_str2port(*tmp + 5, &remote_port) < 0)
				args_ok = FALSE;
		} else if (strncasecmp(*tmp, "TTL=", 4) == 0) {
			if (str_to_uint(*tmp + 4, &ttl) < 0)
				args_ok = FALSE;
		} else if (strncasecmp(*tmp, "TIMEOUT=", 8) == 0) {
			if (str_to_uint(*tmp + 8, &timeout_secs) < 0)
				args_ok = FALSE;
		}
	}
	if (!args_ok) {
		client_send_line(client, "501 Invalid parameters");
		return 0;
	}

	/* args ok, set them and reset the state */
	client_state_reset(client, "XCLIENT");
	if (remote_ip.family != 0)
		client->remote_ip = remote_ip;
	if (remote_port != 0)
		client->remote_port = remote_port;
	if (ttl != UINT_MAX)
		client->proxy_ttl = ttl;
	client->proxy_timeout_secs = timeout_secs;
	client_send_line(client, "220 %s %s", client->my_domain,
			 client->lmtp_set->login_greeting);
	return 0;
}
