#include "lib_acl.h"
#include "lib_protocol.h"
#include "fiber/lib_fiber.h"
#include <signal.h>

static int __nfibers = 0;
static int __npkt = 10;
static int __delay = 10;
static int __timeout = 1000; 
static int __benchmark = 0;

static int __show_reply = 0;
static int __show_timeout = 0;
static int __show_unreach = 0;

static void display_res(ICMP_CHAT *chat)
{
	if (chat) {
		/* 显示 PING 的结果总结 */
		icmp_stat(chat);
		printf(">>>max pkts: %d\r\n", icmp_chat_seqno(chat));
	}
}

/* PING 线程入口 */
static void fiber_ping(ACL_FIBER *fiber acl_unused, void *arg)
{
	const char *dest = (const char *) arg;
	ACL_DNS_DB* dns_db;
	const char *ip;
	ICMP_CHAT *chat;

	/* 通过域名解析出IP地址 */
	dns_db = acl_gethostbyname(dest, NULL);
	if (dns_db == NULL) {
		acl_msg_warn("Can't find domain %s", dest);
		return;
	}

	/* 只取出域名第一个 IP 地址 PING */
	ip = acl_netdb_index_ip(dns_db, 0);
	if (ip == NULL || *ip == 0) {
		acl_msg_error("ip invalid");
		acl_netdb_free(dns_db);
		return;
	}

	/* 创建 ICMP 对象 */
	chat = icmp_chat_create(NULL, 1);

	/* 开始 PING */
	if (strcmp(dest, ip) == 0)
		icmp_ping_one(chat, NULL, ip, __npkt, __delay, __timeout);
	else
		icmp_ping_one(chat, dest, ip, __npkt, __delay, __timeout);

	acl_netdb_free(dns_db);  /* 释放域名解析对象 */
	display_res(chat);  /* 显示 PING 结果 */
	icmp_chat_free(chat);  /* 释放 ICMP 对象 */

	if (--__nfibers == 0)
		acl_fiber_schedule_stop();
}

static void reply_callback(ICMP_PKT_STATUS *status acl_unused, void *arg)
{
	if (__show_reply) {
		ICMP_HOST *host = (ICMP_HOST *) arg;
		printf("%s\r\n", host->dest_ip);
	}
}

static void timeout_callback(ICMP_PKT_STATUS *status, void *arg)
{
	if (__show_timeout) {
		ICMP_HOST *host = (ICMP_HOST *) arg;
		printf("timeout: from %s, %.2f ms\r\n", host->dest_ip, status->rtt);
	}
}

static void unreach_callback(ICMP_PKT_STATUS *status acl_unused, void *arg)
{
	if (__show_unreach) {
		ICMP_HOST *host = (ICMP_HOST *) arg;
		printf("unreach: from %st\r\n", host->dest_ip);
	}
}

static void finish_callback(ICMP_HOST *host, void *arg acl_unused)
{
	icmp_stat_host(host, 0);
}

static void fiber_pine_one(ACL_FIBER *fiber acl_unused, void *arg)
{
	char *ip = (char*) arg;
	ICMP_CHAT *chat = icmp_chat_create(NULL, 1);
	ICMP_HOST *host = icmp_host_new(chat, ip, ip, __npkt, 64,
		__delay, __timeout);

	icmp_host_set(host, host, reply_callback, timeout_callback,
		unreach_callback, finish_callback);

	icmp_chat(host);

	icmp_host_free(host);
	icmp_chat_free(chat);

	acl_myfree(ip);

	if (--__nfibers == 0)
		acl_fiber_schedule_stop();
}

static void usage(const char* progname)
{
	printf("usage: %s -h help\r\n"
		" -d delay[milliseconds]\r\n"
		" -t timout[milliseconds]\r\n"
		" -z stack_size\r\n"
		" -f ip_list_file\r\n"
		" -s show_result_list[timeout,reply,unreach]\r\n"
		" -b benchmark [if > 0 dest will be ignored]\r\n"
		" -n npkt dest1 dest2...\r\n", progname);

	printf("example: %s -n 10 www.sina.com.cn www.qq.com\r\n", progname);
}

static void show_list(const char *s)
{
	ACL_ITER iter;
	ACL_ARGV *tokens = acl_argv_split(s, ",;: \t\r\n");

#define EQ	!strcasecmp

	acl_foreach(iter, tokens) {
		const char *ptr = (const char *) iter.data;
		if (EQ(ptr, "timeout"))
			__show_timeout = 1;
		else if (EQ(ptr, "reply"))
			__show_reply = 1;
		else if (EQ(ptr, "unreach"))
			__show_unreach = 1;
	}

	acl_argv_free(tokens);
}

/* 当收到 SIGINT 信号(即在 PING 过程中用户按下 ctrl + c)时的信号处理函数 */
static void on_sigint(int signo acl_unused)
{
	exit(0);
}

int main(int argc, char* argv[])
{
	char  ch;
	int   i, stack_size = 16000;
	char  iplist[256];

	signal(SIGINT, on_sigint);  /* 用户按下 ctr + c 时中断 PING 程序 */
	acl_msg_stdout_enable(1);  /* 允许 acl_msg_xxx 记录的信息输出至屏幕 */

	iplist[0] = 0;

	while ((ch = getopt(argc, argv, "hn:d:z:b:t:f:s:")) > 0) {
		switch (ch) {
		case 'n':
			__npkt = atoi(optarg);
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		case 'z':
			stack_size = atoi(optarg);
			break;
		case 'd':
			__delay = atoi(optarg);
			break;
		case 't':
			__timeout = atoi(optarg);
			break;
		case 'b':
			__benchmark = atoi(optarg);
			break;
		case 'f':
			ACL_SAFE_STRNCPY(iplist, optarg, sizeof(iplist));
			break;
		case 's':
			show_list(optarg);
			break;
		default:
			break;
		}
	}

	if (__npkt <= 0)
		__npkt = 10;

	if (__benchmark > 0) {
		static char *localhost = "127.0.0.1";

		__nfibers = __benchmark;

		for (i = 0; i < __benchmark; i++)
			acl_fiber_create(fiber_ping, localhost, stack_size);
	} else if (iplist[0]) {
		ACL_ARGV *ips = acl_argv_alloc(10240);
		ACL_FILE *fp = acl_fopen(iplist, "r");
		ACL_ITER  iter;

		if (fp == NULL) {
			printf("open file %s error %s\r\n",
				iplist, acl_last_serror());
			acl_argv_free(ips);
			return 1;
		}
		while (!acl_feof(fp)) {
			char line[256];
			if (acl_fgets_nonl(line, sizeof(line), fp) == NULL)
				break;
			acl_argv_add(ips, line, NULL);
		}
		acl_fclose(fp);

		acl_foreach(iter, ips) {
			char *ip = acl_mystrdup((char*) iter.data);
			acl_fiber_create(fiber_pine_one, ip, stack_size);
		}

		__nfibers = acl_argv_size(ips);
		acl_argv_free(ips);
	} else {
		if (optind == argc) {
			usage(argv[0]);
			return 0;
		}

		/* 记录要启动的协程的总数 */
		__nfibers = argc - optind;

		for (i = optind; i < argc; i++)
			acl_fiber_create(fiber_ping, argv[i], stack_size);
	}

	acl_fiber_schedule();

	return 0;
}
