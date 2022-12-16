/** @file
 * @brief MQTT shell module
 * 
 * Provide MQTT shell commands to the console via the shell
 * 
*/

/*
 *  Copyright (c) 2022 Nordic Semiconductor
 *
 *  SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mqtt_shell, LOG_LEVEL_DBG);

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/random/rand32.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

static struct mqtt_ctx {
    int family;
    const struct shell *shell;
    bool connected;
    struct mqtt_utf8 username;
    struct mqtt_utf8 password;
    sec_tag_t sec_tag;
    union {
		struct sockaddr_in  broker;
		struct sockaddr_in6 broker6;
	};
} mqtt_ctx;

#define THREAD_STACK_SIZE	KB(2)
#define THREAD_PRIORITY		K_LOWEST_APPLICATION_THREAD_PRIO
#define INVALID_SEC_TAG     -1

#define PAYLOAD_BUFFER_SIZE 100

static struct k_thread mqtt_thread;
static K_THREAD_STACK_DEFINE(mqtt_thread_stack, THREAD_STACK_SIZE);

/* Buffers for MQTT client. */
static uint8_t rx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t tx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];

/* MQTT client context */
static struct mqtt_client client;

/**@brief Function to handle received publish event.
 */
static int handle_mqtt_publish_evt(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	int size_read = 0;
	int ret;
    char payload_buf[PAYLOAD_BUFFER_SIZE];

    char topic[evt->param.publish.message.topic.topic.size + 1];
    topic[evt->param.publish.message.topic.topic.size] = '\0';
    memcpy(topic, evt->param.publish.message.topic.topic.utf8, evt->param.publish.message.topic.topic.size);

    shell_print(mqtt_ctx.shell, "MQTT SUB topic: %s", topic);
    do {
        /* Read one less than size of the buffer to leave room for \0 character */
        ret = mqtt_read_publish_payload_blocking(c, payload_buf, sizeof(payload_buf) - 1);
        if (ret > 0) {
            payload_buf[ret] = '\0';
            shell_fprintf(mqtt_ctx.shell, SHELL_NORMAL, "%s", payload_buf);
            size_read += ret;
        }
    } while (ret >= 0 && size_read < evt->param.publish.message.payload.len);
    shell_print(mqtt_ctx.shell, "");

	/* Send QoS1 acknowledgment */
	if (evt->param.publish.message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		const struct mqtt_puback_param ack = {
			.message_id = evt->param.publish.message_id
		};

		mqtt_publish_qos1_ack(&client, &ack);
	}

	return 0;
}

static void mqtt_context_cleanup(struct mqtt_client *c)
{
    free((char *)c->client_id.utf8);
    c->client_id.size = 0;
    if (c->user_name != NULL) {
        free((char *)c->user_name->utf8);
        c->user_name = NULL;
    }
    if (c->password != NULL) {
        free((char *)c->password->utf8);
        c->password = NULL;
    }
}

void mqtt_evt_handler(struct mqtt_client *c,
                      const struct mqtt_evt *evt)
{
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result != 0) {
            LOG_ERR("MQTT connect failed: %d", evt->result);
            mqtt_ctx.connected = false;
            break;
        }
        mqtt_ctx.connected = true;
        LOG_INF("MQTT client connected");
        break;
    case MQTT_EVT_DISCONNECT:
        mqtt_ctx.connected = false;
        mqtt_context_cleanup(c);
		LOG_INF("MQTT client disconnected: %d", evt->result);
		break;

	case MQTT_EVT_PUBLISH: 
		handle_mqtt_publish_evt(c, evt);
        break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error: %d", evt->result);
			break;
		}

		LOG_INF("PUBACK packet id: %u", evt->param.puback.message_id);
		break;

	case MQTT_EVT_SUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT SUBACK error: %d", evt->result);
			break;
		}

		LOG_INF("SUBACK packet id: %u", evt->param.suback.message_id);
		break;

	case MQTT_EVT_PINGRESP:
		if (evt->result != 0) {
			LOG_ERR("MQTT PINGRESP error: %d", evt->result);
		}
		break;

	default:
		LOG_INF("Unhandled MQTT event type: %d", evt->type);
		break;
	}
}

/**@brief Resolves the configured hostname and
 * initializes the MQTT broker structure
 */
static int broker_init(char *url, uint16_t port)
{
	int err;
	struct zsock_addrinfo *addr;
	struct zsock_addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};

    char service[9];
    snprintf(service, sizeof(service), "%hu:0", port);
	err = zsock_getaddrinfo(url, service, &hints, &addr);
	if (err) {
		LOG_ERR("getaddrinfo failed: %d", err);
		return -ECHILD;
	}
	if (addr->ai_addr->sa_family == AF_INET) {
		mqtt_ctx.broker = *(struct sockaddr_in *)addr->ai_addr;
	} else {
		mqtt_ctx.broker6 = *(struct sockaddr_in6 *)addr->ai_addr;
	}
    mqtt_ctx.family = addr->ai_addr->sa_family;
	/* Free the address. */
	zsock_freeaddrinfo(addr);

	return err;
}


static void mqtt_thread_fn(void *arg1, void *arg2, void *arg3)
{
	int err = 0;
	struct zsock_pollfd fds;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	fds.fd = client.transport.tcp.sock;
#if defined(CONFIG_MQTT_LIB_TLS)
	if (client.transport.type == MQTT_TRANSPORT_SECURE) {
		fds.fd = client.transport.tls.sock;
	}
#endif
	fds.events = ZSOCK_POLLIN;
	while (true) {
		err = zsock_poll(&fds, 1, mqtt_keepalive_time_left(&client));
		if (err < 0) {
			LOG_ERR("poll %d", errno);
			break;
		}
		/* timeout or revent, send KEEPALIVE */
		(void)mqtt_live(&client);

		if ((fds.revents & ZSOCK_POLLIN) == ZSOCK_POLLIN) {
			err = mqtt_input(&client);
			if (err != 0) {
				LOG_ERR("mqtt_input %d", err);
				mqtt_abort(&client);
				break;
			}
		}
		if ((fds.revents & ZSOCK_POLLERR) == ZSOCK_POLLERR) {
			LOG_ERR("POLLERR");
			mqtt_abort(&client);
			break;
		}
		if ((fds.revents & ZSOCK_POLLHUP) == ZSOCK_POLLHUP) {
			LOG_ERR("POLLHUP");
			mqtt_abort(&client);
			break;
		}
		if ((fds.revents & ZSOCK_POLLNVAL) == ZSOCK_POLLNVAL) {
			LOG_ERR("POLLNVAL");
			mqtt_abort(&client);
			break;
		}
	}
    mqtt_ctx.connected = false;
    mqtt_context_cleanup(&client);

	LOG_INF("MQTT thread terminated");
}

static int do_mqtt_connect(char *id, char *url, uint16_t port, char *username, char *password)
{
	int err;

	if (mqtt_ctx.connected) {
		return -EISCONN;
	}
    err = broker_init(url, port);
    if (err) {
        LOG_ERR("Failed to initialize broker connection");
        return err;
    }

    mqtt_client_init(&client);
    if (mqtt_ctx.family == AF_INET) {
        client.broker = &mqtt_ctx.broker;
    } else {
        client.broker = &mqtt_ctx.broker6;
    }
    client.evt_cb = mqtt_evt_handler;
    client.client_id.size = strlen(id);
    uint8_t *clientid = malloc(client.client_id.size);
    memcpy(clientid, id, client.client_id.size);
    client.client_id.utf8 = clientid;
    if (username == NULL) {
        client.user_name = NULL;
    } else {
        mqtt_ctx.username.size = strlen(username);
        mqtt_ctx.username.utf8 = malloc(mqtt_ctx.username.size);
        memcpy((char *)mqtt_ctx.username.utf8, username, mqtt_ctx.username.size);
        client.user_name = &mqtt_ctx.username;
    }
    if (password == NULL) {
        client.password = NULL;
    } else {
        mqtt_ctx.password.size = strlen(password);
        mqtt_ctx.password.utf8 = malloc(mqtt_ctx.password.size);
        memcpy((char *)mqtt_ctx.password.utf8, password, mqtt_ctx.password.size);
        client.password = &mqtt_ctx.password;
    }
    client.protocol_version = MQTT_VERSION_3_1_1;

    /* MQTT buffers configuration */
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);

    client.transport.type = MQTT_TRANSPORT_NON_SECURE;

    err = mqtt_connect(&client);
    if (err) {
        LOG_ERR("ERROR: MQTT Connect error: %d", err);
        mqtt_context_cleanup(&client);
        return err;
    }
    //mqtt_ctx.connected = true;
    k_thread_create(&mqtt_thread, mqtt_thread_stack,
			K_THREAD_STACK_SIZEOF(mqtt_thread_stack),
			mqtt_thread_fn, NULL, NULL, NULL,
			THREAD_PRIORITY, K_USER, K_NO_WAIT);
    return err;
}

static int cmd_mqtt_connect(const struct shell *shell, size_t argc,
        char *argv[])
{
    uint8_t arg = 0;
    char *username = NULL;
    char *password = NULL;
    if (mqtt_ctx.connected) {
        shell_print(shell, "ERROR: Already connected to broker");
        return -EISCONN;
    }
    int err;
    if (!argv[3]) {
        shell_print(shell, "Usage: mqtt connect <id> <url> <port> [<username> <password>]");
        return -ENOEXEC;
    }
    char *id = argv[++arg];
    char *broker_url = argv[++arg];
    uint16_t broker_port = strtoul(argv[++arg], NULL, 10);
    if (argv[++arg]) {
        username = argv[arg];
    }
    if (argv[++arg]) {
        password = argv[arg];
    }
    err = do_mqtt_connect(id, broker_url, broker_port, username, password);
    mqtt_ctx.shell = shell;

    return 0;
}

static int cmd_mqtt_disconnect(const struct shell *shell, size_t argc,
        char *argv[])
{
    int err;
    if (!mqtt_ctx.connected) {
        return -ENOTCONN;
    }
    err = mqtt_disconnect(&client);
    return err;
}

static int cmd_mqtt_publish(const struct shell *shell, size_t argc, 
        char *argv[])
{
    int arg = 0;
    struct mqtt_publish_param param;

    if (!argv[++arg]) {
        shell_print(shell, "ERROR: Missing topic and message");
        return -ENOEXEC;
    }
    param.message.topic.topic.utf8 = argv[arg];
    param.message.topic.topic.size = strlen(argv[arg]);

    if (!argv[++arg]) {
        shell_print(shell, "ERROR: Missing message");
        return -ENOEXEC;
    }
    param.message.payload.data = argv[arg];
    param.message.payload.len = strlen(argv[arg]);
    param.message.topic.qos = (argv[++arg] ? strtoul(argv[arg], NULL, 10) : MQTT_QOS_1_AT_LEAST_ONCE);
    param.message_id = sys_rand32_get();
    param.dup_flag = 0;
    param.retain_flag = 0;

    return mqtt_publish(&client, &param);
}

static int cmd_mqtt_subscribe(const struct shell *shell, size_t argc,
        char *arcv[])
{
    int arg = 0;
    struct mqtt_topic subscribe_topic;
    static uint16_t sub_message_id;

    sub_message_id++;
    if (sub_message_id == UINT16_MAX) {
        sub_message_id = 1;
    }

    const struct mqtt_subscription_list subscription_list = {
        .list = &subscribe_topic,
        .list_count = 1,
        .message_id = sub_message_id
    };

    if (!arcv[++arg]) {
        shell_print(shell, "ERROR: need topic and qos");
        return -ENOEXEC;
    }
    subscribe_topic.topic.utf8 = arcv[arg];
    subscribe_topic.topic.size = strlen(arcv[arg]);

    if (!arcv[++arg]) {
        shell_print(shell, "ERROR: missing qos value");
        return -ENOEXEC;
    }
    uint8_t qos = strtoul(arcv[arg], NULL, 10);
    if (qos > MQTT_QOS_2_EXACTLY_ONCE) {
        shell_print(shell, "ERROR: qos must be equal or lesser than %d", 
                MQTT_QOS_2_EXACTLY_ONCE);
        return -ENOEXEC;
    }
    subscribe_topic.qos = qos;
    return mqtt_subscribe(&client, &subscription_list);
}

static int cmd_mqtt_status(const struct shell *shell, size_t argc,
        char *arcv[])
{
    ARG_UNUSED(argc);
    ARG_UNUSED(arcv);

    shell_print(shell, "MQTT is %s", (mqtt_ctx.connected ? "connected" : "disconnected"));
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(mqtt_commands,
    SHELL_CMD(connect, NULL, "id url port [username password]",
        cmd_mqtt_connect),
    SHELL_CMD(disconnect, NULL, "",
        cmd_mqtt_disconnect),
    SHELL_CMD(publish, NULL, "topic message [qos]",
        cmd_mqtt_publish),
    SHELL_CMD(subscribe, NULL, "topic",
        cmd_mqtt_subscribe),
    SHELL_CMD(status, NULL, "",
        cmd_mqtt_status),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(mqtt, &mqtt_commands, "MQTT commands", NULL);
