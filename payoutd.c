/** \file payoutd.c
 *  \brief Main source file for the payoutd daemon.
 *
 *  In a nutshell:
 *  - we are single threaded
 *  - libevent is used to trigger 2 periodic events ("poll event" and "check quit") which poll the hardware and check if we should quit
 *  - main() function supports arguments -h (redis hostname), -p (redis port), -d (serial device name) and -?
 *  - libevent calls cbOnPollEvent() for the "poll" event
 *  - libevent calls cbOnCheckQuitEvent() for the "check quit" event
 *  - redis is used in conjunction with libevent
 *  - if a message is detected in 'validator-request' or 'hopper-request' the cbOnRequestMessage() is called
 *  - the cbOnRequestMessage() checks if the command is known and if its known dispatches the call to a handle<Cmd> function
 *  - a command handler interprets the provided JSON message, issues commands to the money hardware and publishes a JSON response
 *  - the naming convention used most of the time is like: the JSON command is 'configure-bezel' so the handler function is called handleConfigureBezel()
 *  - handleConfigureBezel() itself calls mc_ssp_configure_bezel() which sends the SSP command to the hardware
 *  - each device has its own poll event handling function (responsible for publishing the events to the devices event topic)
 *  - those poll handler functions are hopperEventHandler() and validatorEventHandler()
 *  - on startup/exiting of the daemon started/exiting messages are published to the 'payout-event' topic
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>

// lowlevel library provided by the cash hardware vendor
// innovative technologies (http://innovative-technology.com).
// ssp manual available at http://innovative-technology.com/images/pdocuments/manuals/SSP_Manual.pdf
#include "libitlssp/ssp_commands.h"

// json library
#include <jansson.h>

// c client for redis
#include <hiredis/hiredis.h>
#include <hiredis/async.h>
// adding libevent adapter for hiredis async
#include <hiredis/adapters/libevent.h>

#include <syslog.h>

// libuuid is used to generate msgIds for the responses
#include <uuid/uuid.h>

// https://sites.google.com/site/rickcreamer/Home/cc/c-implementation-of-stringbuffer-functionality
#include "StringBuffer.h"
#include "StringBuffer.c"

/** \brief redis context used for publishing messages */
redisAsyncContext *redisPublishCtx = NULL;

/** \brief redis context used for subscribing to topics */
redisAsyncContext *redisSubscribeCtx = NULL;

struct m_metacash;

/**
 * \brief Structure which describes an actual physical ITL device
 */
struct m_device {
	/** \brief Hardware Id (type of the device) */
	int id;
	/** \brief Human readable name of the device */
	char *name;
	/** \brief Indicates if the device is available */
	int sspDeviceAvailable;
	/** \brief Preshared secret key */
	unsigned long long key;
	/** \brief State of the channel inhibits */
	unsigned char channelInhibits;
	/** \brief SSP_COMMAND structure to use for communicating with this device */
	SSP_COMMAND sspC;
	/** \brief SSP6_REQUEST_DATA structure to use initializing this device */
	SSP6_SETUP_REQUEST_DATA sspSetupReq;
	/** \brief Callback function which is used to inspect and publish events reported by this device */
	void (*eventHandlerFn) (struct m_device *device, struct m_metacash *metacash, SSP_POLL_DATA6 *poll);
};

/**
 * \brief Structure which contains the generic setup data and
 * the device structures for our two ITL devices.
 */
struct m_metacash {
	/** \brief If !=0 then we should quit, checked via libevent callback */
	int quit;
	/** \brief If !=0 then we have actual hardware available */
	int sspAvailable;
	/** \brief The name of the device we should use to connect to the ITL hardware */
	char *serialDevice;
	/** \brief Should the hardware accept coins at all (default off for now) */
	int acceptCoins;
	/** \brief Should the syslog messages also be written to stderr (default no, enable with -e) */
	int logSyslogStderr;

	/** \brief The port of the redis server to which we connect */
	int redisPort;
	/** \brief The hostname of the redis server to which we connect */
	char *redisHost;

	/** \brief base struct for libevent */
	struct event_base *eventBase;
	/** \brief event struct for the periodic polling of the devices */
	struct event *evPoll;
	/** \brief event struct for the periodic check for quitting */
	struct event *evCheckQuit;

	/** \brief struct for the smart-hopper device */
	struct m_device hopper;
	/** \brief struct for the smart-payout device */
	struct m_device validator;
};

/**
 * \brief Structure which describes an actual command which we
 * received in one of our request topics.
 */
struct m_command {
	/** \brief The complete received message parsed as JSON */
	json_t *jsonMessage;
	/** \brief The command from the message */
	char *command;
	/** \brief The correlId to use in the response (this is the msgId from the message which contained the command) */
	char *correlId;
	/** \brief The msgId for the response */
	char *msgId;
	/** \brief The topic to which the response should be published */
	char *responseTopic;
	/** \brief The device to which the command should be issued */
	struct m_device *device;
};

// mcSsp* : ssp helper functions
int mcSspOpenSerialDevice(struct m_metacash *metacash);
void mcSspCloseSerialDevice(struct m_metacash *metacash);
void mcSspSetupCommand(SSP_COMMAND *sspC, int deviceId);
void mcSspInitializeDevice(SSP_COMMAND *sspC, unsigned long long key, struct m_device *device);
void mcSspPollDevice(struct m_device *device, struct m_metacash *metacash);

// mc_ssp_* : ssp magic values and functions (each of these relate directly to a command specified in the ssp protocol)

/** \brief Magic Constant for the "GET FIRMWARE VERSION" command ID as specified in SSP */
#define SSP_CMD_GET_FIRMWARE_VERSION 0x20
/** \brief Magic Constant for the "GET DATASET VERSION" command ID as specified in SSP */
#define SSP_CMD_GET_DATASET_VERSION 0x21
/** \brief Magic Constant for the "GET ALL LEVELS" command ID as specified in SSP */
#define SSP_CMD_GET_ALL_LEVELS 0x22
/** \brief Magic Constant for the "SET DENOMINATION LEVEL" command ID as specified in SSP */
#define SSP_CMD_SET_DENOMINATION_LEVEL 0x34
/** \brief Magic Constant for the "SET CASHBOX PAYOUT LIMIT" command ID as specified in SSP */
#define SSP_CMD_SET_CASHBOX_PAYOUT_LIMIT 0x4E
/** \brief Magic Constant for the "LAST REJECT NOTE" command ID as specified in SSP */
#define SSP_CMD_LAST_REJECT_NOTE 0x17
/** \brief Magic Constant for the "CONFIGURE BEZEL" command ID as specified in SSP */
#define SSP_CMD_CONFIGURE_BEZEL 0x54
/** \brief Magic Constant for the "SMART EMPTY" command ID as specified in SSP */
#define SSP_CMD_SMART_EMPTY 0x52
/** \brief Magic Constant for the "CASHBOX PAYOUT OPERATION DATA" command ID as specified in SSP */
#define SSP_CMD_CASHBOX_PAYOUT_OPERATION_DATA 0x53
/** \brief Magic Constant for the "SET REFILL MODE " command ID as specified in SSP */
#define SSP_CMD_SET_REFILL_MODE 0x30
/** \brief Magic Constant for the "DISPLAY OFF" command ID as specified in SSP */
#define SSP_CMD_DISPLAY_OFF 0x4
/** \brief Magic Constant for the "DISPLAY ON" command ID as specified in SSP */
#define SSP_CMD_DISPLAY_ON 0x3

SSP_RESPONSE_ENUM mc_ssp_empty(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_smart_empty(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_cashbox_payout_operation_data(SSP_COMMAND *sspC, char **json);
SSP_RESPONSE_ENUM mc_ssp_configure_bezel(SSP_COMMAND *sspC, unsigned char r, unsigned char g,
		unsigned char b, unsigned char volatileOption, unsigned char bezelTypeOption);
SSP_RESPONSE_ENUM mc_ssp_display_on(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_display_off(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_last_reject_note(SSP_COMMAND *sspC, unsigned char *reason);
SSP_RESPONSE_ENUM mc_ssp_set_refill_mode(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_get_all_levels(SSP_COMMAND *sspC, char **json);
SSP_RESPONSE_ENUM mc_ssp_set_denomination_level(SSP_COMMAND *sspC, int amount, int level, const char *cc);
SSP_RESPONSE_ENUM mc_ssp_set_cashbox_payout_limit(SSP_COMMAND *sspC, unsigned int amount, int level, const char *cc);
SSP_RESPONSE_ENUM mc_ssp_float(SSP_COMMAND *sspC, const int value, const char *cc, const char option);
SSP_RESPONSE_ENUM mc_ssp_channel_security_data(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_get_firmware_version(SSP_COMMAND *sspC, char *firmwareVersion);
SSP_RESPONSE_ENUM mc_ssp_get_dataset_version(SSP_COMMAND *sspC, char *datasetVersion);

/** \brief Magic Constant for the "route to cashbox" option as specified in SSP */
const char SSP_OPTION_ROUTE_CASHBOX = 0x01;
/** \brief Magic Constant for the "route to storage" option as specified in SSP */
const char SSP_OPTION_ROUTE_STORAGE = 0x00;

/** \brief Magic Constant for the "volatile" option in configure bezel as specified in SSP */
const unsigned char SSP_OPTION_VOLATILE = 0x00;
/** \brief Magic Constant for the "non volatile" option in configure bezel as specified in SSP */
const unsigned char SSP_OPTION_NON_VOLATILE = 0x01;
/** \brief Magic Constant for the "solid" option in configure bezel as specified in SSP */
const unsigned char SSP_OPTION_SOLID = 0x00;
/** \brief Magic Constant for the "flashing" option in configure bezel as specified in SSP */
const unsigned char SSP_OPTION_FLASHING = 0x01;
/** \brief Magic Constant for the "disabled" option in configure bezel as specified in SSP */
const unsigned char SSP_OPTION_DISABLED = 0x02;

static const unsigned long long DEFAULT_KEY = 0x123456701234567LL;

// metacash
int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash);
void setup(struct m_metacash *metacash);
void hopperEventHandler(struct m_device *device, struct m_metacash *metacash, SSP_POLL_DATA6 *poll);
void validatorEventHandler(struct m_device *device, struct m_metacash *metacash, SSP_POLL_DATA6 *poll);

static const char *CURRENCY = "EUR";

/**
 * \brief Set by the signalHandler function and checked in cbCheckQuit.
 */
int receivedSignal = 0;

/**
 * \brief Signal handler
 */
void signalHandler(int signal) {
	receivedSignal = signal;
}

/**
 * \brief Waits for 300ms each time called.
 * \details Details only to get graph.
 * \callergraph
 */
void hardwareWaitTime() {
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 300000000;
	nanosleep(&ts, NULL);
}

/**
 * \brief Connect to redis and return a new redisAsyncContext.
 */
redisAsyncContext* connectRedis(struct m_metacash *metacash) {
	redisAsyncContext *conn = redisAsyncConnect(metacash->redisHost,
			metacash->redisPort);

	if (conn == NULL || conn->err) {
		if (conn) {
			syslog(LOG_ERR,  "fatal: Connection error: %s\n", conn->errstr);
		} else {
			syslog(LOG_ERR,
					"fatal: Connection error: can't allocate redis context\n");
		}
	} else {
		// reference the metcash struct in data for use in connect/disconnect callback
		conn->data = metacash;
	}

	return conn;
}

/**
 * \brief Callback function for libEvent timer triggered "Poll" event.
 * \details Details only to get graph.
 * \callgraph
 */
void cbOnPollEvent(int fd, short event, void *privdata) {
	struct m_metacash *metacash = privdata;
	if (metacash->sspAvailable == 0) {
		// return immediately if we can't communicate with the hardware
		return;
	}

	// don't poll the hopper for events if it is unavailable
	if(metacash->hopper.sspDeviceAvailable) {
		mcSspPollDevice(&metacash->hopper, metacash);
	}

	// don't poll the validator for events if it is unavailable
	if(metacash->validator.sspDeviceAvailable) {
		mcSspPollDevice(&metacash->validator, metacash);
	}
}

/**
 * \brief Callback function for libEvent timer triggered "CheckQuit" event.
 */
void cbOnCheckQuitEvent(int fd, short event, void *privdata) {
	if (receivedSignal != 0) {
		syslog(LOG_NOTICE, "received signal or quit cmd. going to exit event loop.");

		struct m_metacash *metacash = privdata;
		event_base_loopexit(metacash->eventBase, NULL);
		receivedSignal = 0;
	}
}

/**
 * \brief Callback function triggered by an incoming message in the "metacash" topic.
 */
void cbOnMetacashMessage(redisAsyncContext *c, void *r, void *privdata) {
	// empty for now
}

/**
 * \brief Test if cmd.command equals command
 */
int isCommand(struct m_command *cmd, const char *command) {
	return ! strcmp(cmd->command, command);
}

/**
 * \brief Helper function to publish a message to the "payout-event" topic.
 */
int publishPayoutEvent(char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(redisPublishCtx, NULL, NULL, "PUBLISH %s %s", "payout-event", reply);

	free(reply);

	return 0;
}

/**
 * \brief Helper function to publish a message to the "hopper-event" topic.
 */
int publishHopperEvent(char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(redisPublishCtx, NULL, NULL, "PUBLISH %s %s", "hopper-event", reply);

	free(reply);

	return 0;
}

/**
 * \brief Helper function to publish a message to the "validator-event" topic.
 */
int publishValidatorEvent(char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(redisPublishCtx, NULL, NULL, "PUBLISH %s %s", "validator-event", reply);

	free(reply);

	return 0;
}

/**
 * \brief Helper function to publish a message to the given topic.
 * \details Details only to get graph.
 * \callergraph
 */
int replyWith(char *topic, char *format, ...) {
	va_list varags;
	va_start(varags, format);

	char *reply = NULL;
	vasprintf(&reply, format, varags);

	va_end(varags);

	redisAsyncCommand(redisPublishCtx, NULL, NULL, "PUBLISH %s %s", topic, reply);

	free(reply);

	return 0;
}

/**
 * \brief Helper function to publish a reply to a message which was missing a
 * mandatory property (or the property was of the wrong type).
 * \details Details only to get graph.
 * \callergraph
 */
int replyWithPropertyError(struct m_command *cmd, char *name) {
	char *msgId = "unknown";
	if(cmd->msgId) {
		msgId = cmd->msgId;
	}

	char *correlId = "unknown";
	if(cmd->correlId) {
		correlId = cmd->correlId;
	}

	return replyWith(cmd->responseTopic,
			"{\"msgId\":\"%s\",\"correlId\":\"%s\",\"error\":\"Property '%s' missing or of wrong type\"}",
			msgId,
			correlId,
			name);
}

/**
 * \brief Helper function to publish a reply to a message which contains a human readable
 * version of the SSP response.
 * \details Details only to get graph.
 * \callergraph
 */
int replyWithSspResponse(struct m_command *cmd, SSP_RESPONSE_ENUM response) {
	if(response == SSP_RESPONSE_OK) {
		return replyWith(cmd->responseTopic, "{\"msgId\":\"%s\",\"correlId\":\"%s\",\"result\":\"ok\"}",
				cmd->msgId,
				cmd->correlId);
	} else {
		char *errorMsg;

		switch(response) {
			case SSP_RESPONSE_UNKNOWN_COMMAND:
				errorMsg = "unknown command";
				break;
			case SSP_RESPONSE_INCORRECT_PARAMETERS:
				errorMsg = "incorrect parameters";
				break;
			case SSP_RESPONSE_INVALID_PARAMETER:
				errorMsg = "invalid parameter";
				break;
			case SSP_RESPONSE_COMMAND_NOT_PROCESSED:
				errorMsg = "command not processed";
				break;
			case SSP_RESPONSE_SOFTWARE_ERROR:
				errorMsg = "software error";
				break;
			case SSP_RESPONSE_CHECKSUM_ERROR:
				errorMsg = "checksum error";
				break;
			case SSP_RESPONSE_FAILURE:
				errorMsg = "failure";
				break;
			case SSP_RESPONSE_HEADER_FAILURE:
				errorMsg = "header failure";
				break;
			case SSP_RESPONSE_KEY_NOT_SET:
				errorMsg = "key not set";
				break;
			case SSP_RESPONSE_TIMEOUT:
				errorMsg = "timeout";
				break;
			default:
				errorMsg = "unknown";
		}

		return replyWith(cmd->responseTopic, "{\"msgId\":\"%s\",\"correlId\":\"%s\",\"sspError\":\"%s\"}",
				cmd->msgId,
				cmd->correlId,
				errorMsg);
	}
}

/**
 * \brief Handles the JSON "quit" command.
 */
void handleQuit(struct m_command *cmd) {
	receivedSignal = 1;
	replyWithSspResponse(cmd, SSP_RESPONSE_OK); // :D
}

/**
 * \brief Handles the JSON "empty" command.
 */
void handleEmpty(struct m_command *cmd) {
	replyWithSspResponse(cmd, mc_ssp_empty(&cmd->device->sspC));
}

/**
 * \brief Handles the JSON "smart-empty" command.
 */
void handleSmartEmpty(struct m_command *cmd) {
	replyWithSspResponse(cmd, mc_ssp_smart_empty(&cmd->device->sspC));
}

/**
 * \brief Handles the JSON "do-payout" and "test-payout" commands.
 */
void handlePayout(struct m_command *cmd) {
	int payoutOption = 0;

	if (isCommand(cmd, "do-payout")) {
		payoutOption = SSP6_OPTION_BYTE_DO;
	} else {
		payoutOption = SSP6_OPTION_BYTE_TEST;
	}

	json_t *jAmount = json_object_get(cmd->jsonMessage, "amount");
	if(! json_is_integer(jAmount)) {
		replyWithPropertyError(cmd, "amount");
		return;
	}

	int amount = json_integer_value(jAmount);

	SSP_RESPONSE_ENUM resp = ssp6_payout(&cmd->device->sspC, amount, CURRENCY,
			payoutOption);

	if (resp == SSP_RESPONSE_COMMAND_NOT_PROCESSED) {
		char *error = NULL;
		switch (cmd->device->sspC.ResponseData[1]) {
		case 0x01:
			error = "not enough value in smart payout";
			break;
		case 0x02:
			error = "can't pay exact amount";
			break;
		case 0x03:
			error = "smart payout busy";
			break;
		case 0x04:
			error = "smart payout disabled";
			break;
		default:
			error = "unknown";
			break;
		}

		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"error\":\"%s\"}", cmd->correlId, error);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * \brief Handles the JSON "do-float" and "test-float" commands.
 */
void handleFloat(struct m_command *cmd) {
	// basically a copy of do/test-payout ...
	int payoutOption = 0;

	if (isCommand(cmd, "do-float")) {
		payoutOption = SSP6_OPTION_BYTE_DO;
	} else {
		payoutOption = SSP6_OPTION_BYTE_TEST;
	}

	json_t *jAmount = json_object_get(cmd->jsonMessage, "amount");
	if(! json_is_integer(jAmount)) {
		replyWithPropertyError(cmd, "amount");
		return;
	}

	int amount = json_integer_value(jAmount);

	SSP_RESPONSE_ENUM resp = mc_ssp_float(&cmd->device->sspC, amount, CURRENCY,
			payoutOption);

	if (resp == SSP_RESPONSE_COMMAND_NOT_PROCESSED) {
		char *error = NULL;
		switch (cmd->device->sspC.ResponseData[1]) {
		case 0x01:
			error = "not enough value in smart payout";
			break;
		case 0x02:
			error = "can't pay exact amount";
			break;
		case 0x03:
			error = "smart payout busy";
			break;
		case 0x04:
			error = "smart payout disabled";
			break;
		default:
			error = "unknown";
			break;
		}
		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"error\":\"%s\"}",
				cmd->correlId, error);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * \brief Print inhibits debug output.
 */
void dbgDisplayInhibits(unsigned char inhibits) {
	syslog(LOG_DEBUG, "dbgDisplayInhibits: inhibits are: 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d\n",
			(inhibits >> 0) & 1,
			(inhibits >> 1) & 1,
			(inhibits >> 2) & 1,
			(inhibits >> 3) & 1,
			(inhibits >> 4) & 1,
			(inhibits >> 5) & 1,
			(inhibits >> 6) & 1,
			(inhibits >> 7) & 1);
}

/**
 * \brief Handles the JSON "enable-channels" command.
 */
void handleEnableChannels(struct m_command *cmd) {
	json_t *jChannels = json_object_get(cmd->jsonMessage, "channels");
	if(! json_is_string(jChannels)) {
		replyWithPropertyError(cmd, "channels");
		return;
	}

	char *channels = (char *) json_string_value(jChannels);

	// this will be updated and written back to the device state
	// if the update succeeds
	unsigned char currentChannelInhibits = cmd->device->channelInhibits;
	unsigned char highChannels = 0xFF; // actually not in use

	// 8 channels for now, set the bit to 1 for each requested channel
	if(strstr(channels, "1") != NULL) {
		currentChannelInhibits |= 1 << 0;
	}
	if(strstr(channels, "2") != NULL) {
		currentChannelInhibits |= 1 << 1;
	}
	if(strstr(channels, "3") != NULL) {
		currentChannelInhibits |= 1 << 2;
	}
	if(strstr(channels, "4") != NULL) {
		currentChannelInhibits |= 1 << 3;
	}
	if(strstr(channels, "5") != NULL) {
		currentChannelInhibits |= 1 << 4;
	}
	if(strstr(channels, "6") != NULL) {
		currentChannelInhibits |= 1 << 5;
	}
	if(strstr(channels, "7") != NULL) {
		currentChannelInhibits |= 1 << 6;
	}
	if(strstr(channels, "8") != NULL) {
		currentChannelInhibits |= 1 << 7;
	}

	SSP_RESPONSE_ENUM resp = ssp6_set_inhibits(&cmd->device->sspC, currentChannelInhibits, highChannels);

	if(resp == SSP_RESPONSE_OK) {
		// okay, update the channelInhibits in the device structure with the new state
		cmd->device->channelInhibits = currentChannelInhibits;

		if(0) {
			syslog(LOG_DEBUG, "enable-channels:\n");
			dbgDisplayInhibits(currentChannelInhibits);
		}
	}

	replyWithSspResponse(cmd, resp);
}

/**
 * \brief Handles the JSON "disable-channels" command.
 */
void handleDisableChannels(struct m_command *cmd) {
	json_t *jChannels = json_object_get(cmd->jsonMessage, "channels");
	if(! json_is_string(jChannels)) {
		replyWithPropertyError(cmd, "channels");
		return;
	}

	char *channels = (char *) json_string_value(jChannels);

	// this will be updated and written back to the device state
	// if the update succeeds
	unsigned char currentChannelInhibits = cmd->device->channelInhibits;
	unsigned char highChannels = 0xFF; // actually not in use

	// 8 channels for now, set the bit to 0 for each requested channel
	if(strstr(channels, "1") != NULL) {
		currentChannelInhibits &= ~(1 << 0);
	}
	if(strstr(channels, "2") != NULL) {
		currentChannelInhibits &= ~(1 << 1);
	}
	if(strstr(channels, "3") != NULL) {
		currentChannelInhibits &= ~(1 << 2);
	}
	if(strstr(channels, "4") != NULL) {
		currentChannelInhibits &= ~(1 << 3);
	}
	if(strstr(channels, "5") != NULL) {
		currentChannelInhibits &= ~(1 << 4);
	}
	if(strstr(channels, "6") != NULL) {
		currentChannelInhibits &= ~(1 << 5);
	}
	if(strstr(channels, "7") != NULL) {
		currentChannelInhibits &= ~(1 << 6);
	}
	if(strstr(channels, "8") != NULL) {
		currentChannelInhibits &= ~(1 << 7);
	}

	SSP_RESPONSE_ENUM resp = ssp6_set_inhibits(&cmd->device->sspC, currentChannelInhibits, highChannels);

	if(resp == SSP_RESPONSE_OK) {
		// okay, update the channelInhibits in the device structure with the new state
		cmd->device->channelInhibits = currentChannelInhibits;

		if(0) {
			syslog(LOG_DEBUG, "disable-channels:\n");
			dbgDisplayInhibits(currentChannelInhibits);
		}
	}

	replyWithSspResponse(cmd, resp);
}

/**
 * \brief Handles the JSON "inhibit-channels" command.
 */
void handleInhibitChannels(struct m_command *cmd) {
	json_t *jChannels = json_object_get(cmd->jsonMessage, "channels");
	if(! json_is_string(jChannels)) {
		replyWithPropertyError(cmd, "channels");
		return;
	}

	char *channels = (char *) json_string_value(jChannels);

	unsigned char lowChannels = 0xFF;
	unsigned char highChannels = 0xFF;

	// 8 channels for now
	if(strstr(channels, "1") != NULL) {
		lowChannels &= ~(1 << 0);
	}
	if(strstr(channels, "2") != NULL) {
		lowChannels &= ~(1 << 1);
	}
	if(strstr(channels, "3") != NULL) {
		lowChannels &= ~(1 << 2);
	}
	if(strstr(channels, "4") != NULL) {
		lowChannels &= ~(1 << 3);
	}
	if(strstr(channels, "5") != NULL) {
		lowChannels &= ~(1 << 4);
	}
	if(strstr(channels, "6") != NULL) {
		lowChannels &= ~(1 << 5);
	}
	if(strstr(channels, "7") != NULL) {
		lowChannels &= ~(1 << 6);
	}
	if(strstr(channels, "8") != NULL) {
		lowChannels &= ~(1 << 7);
	}

	replyWithSspResponse(cmd, ssp6_set_inhibits(&cmd->device->sspC, lowChannels, highChannels));
}

/**
 * \brief Handles the JSON "enable" command.
 */
void handleEnable(struct m_command *cmd) {
	replyWithSspResponse(cmd, ssp6_enable(&cmd->device->sspC));
}

/**
 * \brief Handles the JSON "disable" command.
 */
void handleDisable(struct m_command *cmd) {
	replyWithSspResponse(cmd, ssp6_disable(&cmd->device->sspC));
}

/**
 * \brief Handles the JSON "set-denomination-levels" command.
 */
void handleSetDenominationLevels(struct m_command *cmd) {
	json_t *jLevel = json_object_get(cmd->jsonMessage, "level");
	if(! json_is_integer(jLevel)) {
		replyWithPropertyError(cmd, "level");
		return;
	}

	json_t *jAmount = json_object_get(cmd->jsonMessage, "amount");
	if(! json_is_integer(jAmount)) {
		replyWithPropertyError(cmd, "amount");
		return;
	}

	int level = json_integer_value(jLevel);
	int amount = json_integer_value(jAmount);

	if(level > 0) {
		/* Quote from the spec -.-
		 *
		 * A command to increment the level of coins of a denomination stored in the hopper.
		 * The command is formatted with the command byte first, amount of coins to *add*
		 * as a 2-byte little endian, the value of coin as 2-byte little endian and
		 * (if using protocol version 6) the country code of the coin as 3 byte ASCII. The level of coins for a
		 * denomination can be set to zero by sending a zero level for that value.
		 *
		 * In a nutshell: This command behaves only with a level of 0 as expected (setting the absolute value),
		 * otherwise it works like the not existing "increment denomination level" command.
		 */

		// ignore the result for now. we could not do much anyway now.
		mc_ssp_set_denomination_level(&cmd->device->sspC, amount, 0, CURRENCY);
	}

	replyWithSspResponse(cmd, mc_ssp_set_denomination_level(&cmd->device->sspC, amount, level, CURRENCY));
}

/**
 * \brief Handles the JSON "set-cashbox-payout-limit" command.
 */
void handleSetCashboxPayoutLimit(struct m_command *cmd) {
	json_t *jLevel = json_object_get(cmd->jsonMessage, "level");
	if(! json_is_integer(jLevel)) {
		replyWithPropertyError(cmd, "level");
		return;
	}

	json_t *jAmount = json_object_get(cmd->jsonMessage, "amount");
	if(! json_is_integer(jAmount)) {
		replyWithPropertyError(cmd, "amount");
		return;
	}

	int level = json_integer_value(jLevel);
	int amount = json_integer_value(jAmount);

	replyWithSspResponse(cmd, mc_ssp_set_cashbox_payout_limit(&cmd->device->sspC, level, amount, CURRENCY));
}

/**
 * \brief Handles the JSON "get-all-levels" command.
 */
void handleGetAllLevels(struct m_command *cmd) {
	char *json = NULL;

	SSP_RESPONSE_ENUM resp = mc_ssp_get_all_levels(&cmd->device->sspC, &json);

	if(resp == SSP_RESPONSE_OK) {
		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"levels\":[%s]}", cmd->correlId, json);
	} else {
		replyWithSspResponse(cmd, resp);
	}

	free(json);
}

/**
 * \brief Handles the JSON "cashbox-payout-operation-data" command.
 */
void handleCashboxPayoutOperationData(struct m_command *cmd) {
	char *json = NULL;

	SSP_RESPONSE_ENUM resp = mc_ssp_cashbox_payout_operation_data(&cmd->device->sspC, &json);

	if(resp == SSP_RESPONSE_OK) {
		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"levels\":[%s]}", cmd->correlId, json);
	} else {
		replyWithSspResponse(cmd, resp);
	}

	free(json);
}


/**
 * \brief Handles the JSON "get-firmware-version" command.
 */
void handleGetFirmwareVersion(struct m_command *cmd) {
	char firmwareVersion[100] = { 0 };

	SSP_RESPONSE_ENUM resp = mc_ssp_get_firmware_version(&cmd->device->sspC, &firmwareVersion[0]);

	if(resp == SSP_RESPONSE_OK) {
		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"version\":\"%s\"}", cmd->correlId, firmwareVersion);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * \brief Handles the JSON "get-dataset-version" command.
 */
void handleGetDatasetVersion(struct m_command *cmd) {
	char datasetVersion[100] = { 0 };

	SSP_RESPONSE_ENUM resp = mc_ssp_get_dataset_version(&cmd->device->sspC, &datasetVersion[0]);

	if(resp == SSP_RESPONSE_OK) {
		replyWith(cmd->responseTopic, "{\"correlId\":\"%s\",\"version\":\"%s\"}",
				cmd->correlId, datasetVersion);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * \brief Handles the JSON "last-reject-note" command.
 */
void handleLastRejectNote(struct m_command *cmd) {
	unsigned char reasonCode;

	SSP_RESPONSE_ENUM resp = mc_ssp_last_reject_note(&cmd->device->sspC, &reasonCode);

	if (resp == SSP_RESPONSE_OK) {
		char *reason = NULL;

		switch (reasonCode) {
		case 0x00: // Note accepted
			reason = "note accepted";
			break;
		case 0x01: // Note length incorrect
			reason = "note length incorrect";
			break;
		case 0x02: // Average fail
			reason = "internal validation failure: average fail";
			break;
		case 0x03: // Coastline fail
			reason = "internal validation failure: coastline fail";
			break;
		case 0x04: // Graph fail
			reason = "internal validation failure: graph fail";
			break;
		case 0x05: // Buried fail
			reason = "internal validation failure: buried fail";
			break;
		case 0x06: // Channel inhibited
			reason = "channel inhibited";
			break;
		case 0x07: // Second note inserted
			reason = "second note inserted";
			break;
		case 0x08: // Reject by host
			reason = "reject by host";
			break;
		case 0x09: // Note recognised in more than one channel
			reason = "note recognised in more than one channel";
			break;
		case 0x0A: // Reject reason 10
			reason = "rear sensor error";
			break;
		case 0x0B: // Note too long
			reason = "note too long";
			break;
		case 0x0C: // Disabled by host
			reason = "disabled by host";
			break;
		case 0x0D: // Mechanism slow/stalled
			reason = "mechanism slow/stalled";
			break;
		case 0x0E: // Strimming attempt detected
			reason = "strimming attempt detected";
			break;
		case 0x0F: // Fraud channel reject
			reason = "fraud channel reject";
			break;
		case 0x10: // No notes inserted
			reason = "no notes inserted";
			break;
		case 0x11: // Peak detect fail
			reason = "peak detect fail";
			break;
		case 0x12: // Twisted note detected
			reason = "twisted note detected";
			break;
		case 0x13: // Escrow time-out
			reason = "escrow time-out";
			break;
		case 0x14: // Bar code scan fail
			reason = "bar code scan fail";
			break;
		case 0x15: // Rear sensor 2 fail
			reason = "rear sensor 2 fail";
			break;
		case 0x16: // Slot fail 1
			reason = "slot fail 1";
			break;
		case 0x17: // Slot fail 2
			reason = "slot fail 2";
			break;
		case 0x18: // Lens over-sample
			reason = "lens over-sample";
			break;
		case 0x19: // Width detect fail
			reason = "width detect fail";
			break;
		case 0x1A: // Short note detected
			reason = "short note detected";
			break;
		case 0x1B: // Note payout
			reason = "note payout";
			break;
		case 0x1C: // Unable to stack note
			reason = "unable to stack note";
			break;
		default: // not defined in API doc
			reason = "undefined in API";
			break;
		}

		replyWith(cmd->responseTopic,
				"{\"correlId\":\"%s\",\"reason\":\"%s\",\"code\":%ld}",
				cmd->correlId, reason, reasonCode);
	} else {
		replyWithSspResponse(cmd, resp);
	}
}

/**
 * \brief Handles the JSON "channel-security" command.
 */
void handleChannelSecurityData(struct m_command *cmd) {
	mc_ssp_channel_security_data(&cmd->device->sspC);
}

/**
 * \brief Handles the JSON "test" command
 */
void handleTest(struct m_command *cmd) {
	replyWithSspResponse(cmd, SSP_RESPONSE_OK);
}

/**
 * \brief Handles the JSON "configure-bezel" command.
 */
void handleConfigureBezel(struct m_command *cmd) {
	json_t *jR = json_object_get(cmd->jsonMessage, "r");
	if(! json_is_integer(jR)) {
		replyWithPropertyError(cmd, "r");
		return;
	}
	unsigned char r = json_integer_value(jR);

	json_t *jG = json_object_get(cmd->jsonMessage, "g");
	if(! json_is_integer(jG)) {
		replyWithPropertyError(cmd, "g");
		return;
	}
	unsigned char g = json_integer_value(jG);

	json_t *jB = json_object_get(cmd->jsonMessage, "b");
	if(! json_is_integer(jB)) {
		replyWithPropertyError(cmd, "b");
		return;
	}
	unsigned char b = json_integer_value(jB);

	json_t *jType = json_object_get(cmd->jsonMessage, "type");
	if(! json_is_integer(jType)) {
		replyWithPropertyError(cmd, "type");
		return;
	}
	unsigned char type = json_integer_value(jType);

	replyWithSspResponse(cmd,
			mc_ssp_configure_bezel(&cmd->device->sspC, r, g, b, SSP_OPTION_NON_VOLATILE, type));
}

/**
 * \brief Callback function triggered by an incoming message in either
 * the "hopper-request" or "validator-request" topic.
 * \details Details only to get graph.
 * \callgraph
 */
void cbOnRequestMessage(redisAsyncContext *c, void *r, void *privdata) {
	if (r == NULL) {
		return;
	}

	hardwareWaitTime();

	struct m_metacash *m = c->data;
	redisReply *reply = r;

	// example from http://stackoverflow.com/questions/16213676/hiredis-waiting-for-message
	if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
		if (strcmp(reply->element[0]->str, "subscribe") != 0) {
			char *topic = reply->element[1]->str;
			struct m_command cmd;

			cmd.msgId = NULL;
			cmd.correlId = NULL;
			cmd.command = NULL;

			// decide to which topic the response should be sent to
			if (strcmp(topic, "validator-request") == 0) {
				cmd.device = &m->validator;
				cmd.responseTopic = "validator-response";
			} else if (strcmp(topic, "hopper-request") == 0) {
				cmd.device = &m->hopper;
				cmd.responseTopic = "hopper-response";
			} else {
				syslog(LOG_ERR, "cbOnRequestMessage subscribed for a topic we don't have a response topic\n");
				return;
			}

			// generate a new 'msgId' for the response itself
			uuid_t uuid;
			uuid_generate_time_safe(uuid);
			char msgId[37] = { 0 }; // ex. "1b4e28ba-2fa1-11d2-883f-0016d3cca427" + "\0"
			uuid_unparse_lower(uuid, msgId);
			cmd.msgId = (char *) &msgId; // points to a local variable. only valid until this block is exited!

			char *message = reply->element[2]->str;

			// try to parse the message as json
			json_error_t error;
			cmd.jsonMessage = json_loads(message, 0, &error);

			if(! cmd.jsonMessage) {
				syslog(LOG_WARNING, "unable to process message: could not parse json. reason: %s, line: %d",
						error.text, error.line);
				replyWith(cmd.responseTopic,
						"{\"error\":\"could not parse json\",\"reason\":\"%s\",\"line\":%d}",
						error.text, error.line);
				// no need to json_decref(cmd.jsonMessage) here
				return;
			}

			// extract the 'msgId' property (used as the 'correlId' in a response)
			// this will be the 'correlId' used in replies.
			json_t *jMsgId = json_object_get(cmd.jsonMessage, "msgId");
			if(! json_is_string(jMsgId)) {
				syslog(LOG_WARNING, "unable to process message: property 'msgId' missing or invalid");
				replyWithPropertyError(&cmd, "msgId");
				json_decref(cmd.jsonMessage);
				return;
			} else {
				cmd.correlId = (char *) json_string_value(jMsgId); // cast for now
			}

			// extract the 'cmd' property
			json_t *jCmd = json_object_get(cmd.jsonMessage, "cmd");
			if(! json_is_string(jCmd)) {
				syslog(LOG_WARNING, "unable to process message: property 'cmd' missing or invalid");
				replyWithPropertyError(&cmd, "cmd");
				json_decref(cmd.jsonMessage);
				return;
			} else {
				cmd.command = (char *) json_string_value(jCmd); // cast for now
			}

			// proper json structure, properties cmd and msgId have been verified here.
			// also we know which device is used and where we should send our response to.
			// finally try to dispatch the message to the appropriate command handler
			// function if any. in case we don't know that command we respond with a
			// generic error response.

			syslog(LOG_INFO, "processing cmd='%s' from msgId='%s' in topic='%s' for device='%s'\n",
					cmd.command, cmd.correlId, topic, cmd.device->name);

			if(isCommand(&cmd, "quit")) {
				handleQuit(&cmd);
			} else if(isCommand(&cmd, "test")) {
				handleTest(&cmd);
			} else {
				// commands in here need the actual hardware

				if(! m->sspAvailable) {
					// TODO: an unknown command without the actual hardware will also receive this response :-/
					syslog(LOG_WARNING, "rejecting cmd='%s' from msgId='%s', hardware unavailable!\n", cmd.command, cmd.correlId);
					replyWith(cmd.responseTopic, "{\"correlId\":\"%s\",\"error\":\"hardware unavailable\"}", cmd.correlId);
				} else {
					if(isCommand(&cmd, "configure-bezel")) {
						handleConfigureBezel(&cmd);
					} else if(isCommand(&cmd, "empty")) {
						handleEmpty(&cmd);
					} else if (isCommand(&cmd, "smart-empty")) {
						handleSmartEmpty(&cmd);
					} else if (isCommand(&cmd, "cashbox-payout-operation-data")) {
						handleCashboxPayoutOperationData(&cmd);
					} else if (isCommand(&cmd, "set-cashbox-payout-limit")) {
						handleSetCashboxPayoutLimit(&cmd);
					} else if (isCommand(&cmd, "enable")) {
						handleEnable(&cmd);
					} else if (isCommand(&cmd, "disable")) {
						handleDisable(&cmd);
					} else if(isCommand(&cmd, "enable-channels")) {
						handleEnableChannels(&cmd);
					} else if(isCommand(&cmd, "disable-channels")) {
						handleDisableChannels(&cmd);
					} else if(isCommand(&cmd, "inhibit-channels")) {
						handleInhibitChannels(&cmd);
					} else if (isCommand(&cmd, "test-float") || isCommand(&cmd, "do-float")) {
						handleFloat(&cmd);
					} else if (isCommand(&cmd, "test-payout") || isCommand(&cmd, "do-payout")) {
						handlePayout(&cmd);
					} else if (isCommand(&cmd, "get-firmware-version")) {
						handleGetFirmwareVersion(&cmd);
					} else if (isCommand(&cmd, "get-dataset-version")) {
						handleGetDatasetVersion(&cmd);
					} else if (isCommand(&cmd, "channel-security-data")) {
						handleChannelSecurityData(&cmd);
					} else if (isCommand(&cmd, "get-all-levels")) {
						handleGetAllLevels(&cmd);
					} else if (isCommand(&cmd, "set-denomination-level")) {
						handleSetDenominationLevels(&cmd);
					} else if (isCommand(&cmd, "last-reject-note")) {
						handleLastRejectNote(&cmd);
					} else {
						syslog(LOG_WARNING, "unable to process message: no handler for cmd='%s' found", cmd.command);
						replyWith(cmd.responseTopic, "{\"correlId\":\"%s\",\"error\":\"unknown command\",\"cmd\":\"%s\"}",
								cmd.correlId, cmd.command);
					}
				}
			}

			// this will also free the other json objects associated with it
			json_decref(cmd.jsonMessage);
		}
	}
}

/**
 * \brief Callback function triggered by the redis client on connecting with
 * the "publish" context.
 */
void cbOnConnectPublishContext(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		syslog(LOG_ERR, "cbOnConnectPublishContext: redis error: %s\n", c->errstr);
		return;
	}
	syslog(LOG_INFO, "cbOnConnectPublishContext: connected to redis\n");
}

/**
 * \brief Callback function triggered by the redis client on disconnecting with
 * the "publish" context.
 */
void cbOnDisconnectPublishContext(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		syslog(LOG_ERR, "cbOnDisconnectPublishContext: redis error: %s\n", c->errstr);
		return;
	}
	syslog(LOG_INFO, "cbOnDisconnectPublishContext: disconnected from redis\n");
}

/**
 * \brief Callback function triggered by the redis client on connecting with
 * the "subscribe" context.
 */
void cbOnConnectSubscribeContext(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		syslog(LOG_ERR, "cbOnConnectSubscribeContext - redis error: %s\n", c->errstr);
		return;
	}
	syslog(LOG_INFO, "cbOnConnectSubscribeContext - connected to redis\n");

	redisAsyncContext *cNotConst = (redisAsyncContext*) c; // get rids of discarding qualifier \"const\" warning

	// subscribe the topics in redis from which we want to receive messages
	redisAsyncCommand(cNotConst, cbOnMetacashMessage, NULL, "SUBSCRIBE metacash");

	// n.b: the same callback function handles both topics
	redisAsyncCommand(cNotConst, cbOnRequestMessage, NULL, "SUBSCRIBE validator-request");
	redisAsyncCommand(cNotConst, cbOnRequestMessage, NULL, "SUBSCRIBE hopper-request");
}

/**
 * \brief Callback function triggered by the redis client on disconnecting with
 * the "subscribe" context.
 */
void cbOnDisconnectSubscribeContext(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		syslog(LOG_INFO, "cbOnDisconnectSubscribeContext - redis error: %s\n", c->errstr);
		return;
	}
	syslog(LOG_INFO, "cbOnDisconnectSubscribeContext - disconnected from redis\n");
}

/**
 * \brief This function never returns, it displays the reason argument and return code
 * in the syslog and exits immediately.
 */
void die(char *reason, int rc) {
	syslog(LOG_EMERG, "fatal error occured: %s, rc=%d", reason, rc);
	syslog(LOG_EMERG, "exiting NOW");
	exit(rc);
}

/**
 * \brief Supports arguments -h (redis hostname), -p (redis port), -d (serial device name) and -?.
 * \details Warning: both "calls" to hopperEventHandler() and validatorEventHandler() in the callgraph are false positives!
 * \callgraph
 */
int main(int argc, char *argv[]) {
	// setup logging via syslog
	setlogmask(LOG_UPTO(LOG_INFO));
	openlog("payoutd", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
	syslog(LOG_NOTICE, "Program started by User %d", getuid());

	// register interrupt handler for signals
	signal(SIGTERM, signalHandler);
	signal(SIGINT, signalHandler);

	struct m_metacash metacash;
	metacash.sspAvailable = 0; // defaults to no initially
	metacash.quit = 0;
	metacash.logSyslogStderr = 0; // default, override using -e
	metacash.acceptCoins = 0; // default, override using -c

	metacash.serialDevice = "/dev/ttyACM0";	// default, override with -d argument
	metacash.redisHost = "127.0.0.1";	// default, override with -h argument
	metacash.redisPort = 6379;			// default, override with -p argument

	metacash.hopper.id = 0x10; // 0x10 -> Smart Hopper ("Münzer")
	metacash.hopper.sspDeviceAvailable = 0; // defaults to no initially
	metacash.hopper.name = "Mr. Coin";
	metacash.hopper.key = DEFAULT_KEY;
	metacash.hopper.eventHandlerFn = hopperEventHandler;

	metacash.validator.id = 0x00; // 0x00 -> Smart Payout NV200 ("Scheiner")
	metacash.validator.sspDeviceAvailable = 0; // defaults to no initially
	metacash.validator.name = "Ms. Note";
	metacash.validator.key = DEFAULT_KEY;
	metacash.validator.eventHandlerFn = validatorEventHandler;

	// parse the command line arguments
	if (parseCmdLine(argc, argv, &metacash)) {
		die("invalid command line", 1);
		// never reached, already exited
	}

	if(metacash.logSyslogStderr) {
		closelog();
		// also writeout syslog messages to stderr (intended for development purposes, you
		// can enable this with the -e argument)
		openlog("payoutd", LOG_PERROR | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
	}

	syslog(LOG_NOTICE, "using redis at %s:%d and hardware device %s",
			metacash.redisHost, metacash.redisPort, metacash.serialDevice);

	// open the serial device
	if (mcSspOpenSerialDevice(&metacash) == 0) {
		metacash.sspAvailable = 1;
	} else {
		syslog(LOG_ALERT, "ssp communication unavailable");
	}

	// setup the ssp commands, configure and initialize the hardware
	setup(&metacash);

	syslog(LOG_NOTICE, "open for business :D");

	publishPayoutEvent("{ \"event\":\"started\" }");

	event_base_dispatch(metacash.eventBase); // blocking until exited via api-call

	publishPayoutEvent("{ \"event\":\"exiting\" }");

	syslog(LOG_NOTICE, "shutting down");

	if (metacash.sspAvailable) {
		mcSspCloseSerialDevice(&metacash);
	}

	// cleanup stuff before exiting.

	// redis
	redisAsyncFree(redisPublishCtx);
	redisAsyncFree(redisSubscribeCtx);

	// libevent
	event_base_free(metacash.eventBase);

	// syslog
	syslog(LOG_NOTICE, "exiting NOW");
	closelog();

	return 0;
}

/**
 * \brief Parse the command line arguments.
 */
int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash) {
	opterr = 0;

	int c;
	while ((c = getopt(argc, argv, "ech:p:d:")) != -1) {
		switch (c) {
		case 'h':
			metacash->redisHost = optarg;
			break;
		case 'p':
			metacash->redisPort = atoi(optarg);
			break;
		case 'd':
			metacash->serialDevice = optarg;
			break;
		case 'c':
			metacash->acceptCoins = 1;
			break;
		case 'e':
			metacash->logSyslogStderr = 1;
			break;
		case '?':
			if (optopt == 'h' || optopt == 'p' || optopt == 'd') {
				fprintf(stderr, "Option -%c requires an argument.\n", optopt);
				syslog(LOG_ERR, "Option -%c requires an argument.\n", optopt);
			} else if (isprint(optopt)) {
				fprintf(stderr, "Unknown option '-%c'.\n", optopt);
				syslog(LOG_ERR, "Unknown option '-%c'.\n", optopt);
			} else {
				fprintf(stderr, "Unknown option character 'x%x'.\n", optopt);
				syslog(LOG_ERR, "Unknown option character 'x%x'.\n", optopt);
			}
			return 1;
		default:
			fprintf(stderr, "Unknown argument: %c", c);
			syslog(LOG_ERR, "Unknown argument: %c", c);
			return 1;
		}
	}

	return 0;
}

/**
 * \brief Callback function used for inspecting and publishing events reported by the Hopper hardware.
 */
void hopperEventHandler(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {
	for (unsigned char i = 0; i < poll->event_count; ++i) {
		switch (poll->events[i].event) {
		case SSP_POLL_RESET:
			publishHopperEvent("{\"event\":\"unit reset\"}");
			// Make sure we are using ssp version 6
			if (ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				die("hopperEventHandler: SSP Host Protocol Failed", 3);
				// never reached, already exited
			}
			break;
		case SSP_POLL_READ:
			// the \"read\" event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if (poll->events[i].data1 > 0) {
				publishHopperEvent("{\"event\":\"read\",\"channel\":%ld}", poll->events[i].data1);
			} else {
				// this is reported more than once for a single note
				publishHopperEvent("{\"event\":\"reading\"}");
			}
			break;
		case SSP_POLL_TIMEOUT:
			publishHopperEvent("{\"event\":\"timeout\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_DISPENSING:
			publishHopperEvent("{\"event\":\"dispensing\",\"amount\":%ld}", poll->events[i].data1);
			break;
		case SSP_POLL_DISPENSED:
			publishHopperEvent("{\"event\":\"dispensed\",\"amount\":%ld}", poll->events[i].data1);
			break;
		case SSP_POLL_FLOATING:
			publishHopperEvent("{\"event\":\"floating\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_FLOATED:
			publishHopperEvent("{\"event\":\"floated\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_CASHBOX_PAID:
			publishHopperEvent("{\"event\":\"cashbox paid\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_JAMMED:
			publishHopperEvent("{\"event\":\"jammed\"}");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			publishHopperEvent("{\"event\":\"fraud attempt\"}");
			break;
		case SSP_POLL_COIN_CREDIT:
			publishHopperEvent("{\"event\":\"coin credit\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_EMPTY:
			publishHopperEvent("{\"event\":\"empty\"}");
			break;
		case SSP_POLL_EMPTYING:
			publishHopperEvent("{\"event\":\"emptying\"}");
			break;
		case SSP_POLL_SMART_EMPTYING:
			publishHopperEvent("{\"event\":\"smart emptying\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_SMART_EMPTIED:
			publishHopperEvent("{\"event\":\"smart emptied\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_CREDIT:
			// The note which was in escrow has been accepted
			publishHopperEvent("{\"event\":\"credit\",\"channel\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			publishHopperEvent(
					"{\"event\":\"incomplete payout\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			publishHopperEvent(
					"{\"event\":\"incomplete float\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_DISABLED:
			// The unit has been disabled
			publishHopperEvent("{\"event\":\"disabled\"}");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			switch (poll->events[i].data1) {
			case NO_FAILUE:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"no error\"}");
				break;
			case SENSOR_FLAP:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"sensor flap\"}");
				break;
			case SENSOR_EXIT:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"sensor exit\"}");
				break;
			case SENSOR_COIL1:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"sensor coil 1\"}");
				break;
			case SENSOR_COIL2:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"sensor coil 2\"}");
				break;
			case NOT_INITIALISED:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"not initialized\"}");
				break;
			case CHECKSUM_ERROR:
				publishHopperEvent("{\"event\":\"calibration fail\",\"error\":\"checksum error\"}");
				break;
			case COMMAND_RECAL:
				publishHopperEvent("{\"event\":\"recalibrating\"}");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			// fallback only. in case we got a message which is not handled above.
			// if you can see this have a look in the SSP reference manual what
			// the message is about.
			publishHopperEvent("{\"event\":\"unknown\",\"id\":\"0x%02X\"}", poll->events[i].event);
			break;
		}
	}
}

/**
 * \brief Callback function used for inspecting and publishing events reported by the Validator hardware.
 */
void validatorEventHandler(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {
	for (unsigned char i = 0; i < poll->event_count; ++i) {
		switch (poll->events[i].event) {
		case SSP_POLL_RESET:
			publishValidatorEvent("{\"event\":\"unit reset\"}");
			// Make sure we are using ssp version 6
			if (ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				die("validatorEventHandler: SSP Host Protocol Failed", 3);
				// never reached, already exited
			}
			break;
		case SSP_POLL_READ:
			// the \"read\" event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if (poll->events[i].data1 > 0) {
				// The note which was in escrow has been accepted
				unsigned long amount =
						device->sspSetupReq.ChannelData[poll->events[i].data1 - 1].value
								* 100;
				publishValidatorEvent("{\"event\":\"read\",\"amount\":%ld,\"channel\":%ld}",
						amount, poll->events[i].data1);
			} else {
				publishValidatorEvent("{\"event\":\"reading\"}");
			}
			break;
		case SSP_POLL_EMPTY:
			publishValidatorEvent("{\"event\":\"empty\"}");
			break;
		case SSP_POLL_EMPTYING:
			publishValidatorEvent("{\"event\":\"emptying\"}");
			break;
		case SSP_POLL_SMART_EMPTYING:
			publishValidatorEvent("{\"event\":\"smart emptying\"}");
			break;
		case SSP_POLL_TIMEOUT:
			publishValidatorEvent("{\"event\":\"timeout\",\"amount\":%ld,\"cc\":\"%s\"}", poll->events[i].data1, poll->events[i].cc);
			break;
		case SSP_POLL_CREDIT:
			// The note which was in escrow has been accepted
		{
			unsigned long amount =
					device->sspSetupReq.ChannelData[poll->events[i].data1 - 1].value
							* 100;
			publishValidatorEvent("{\"event\":\"credit\",\"amount\":%ld,\"channel\":%ld}",
					amount, poll->events[i].data1);
		}
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			publishValidatorEvent(
					"{\"event\":\"incomplete payout\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			publishValidatorEvent(
					"{\"event\":\"incomplete float\",\"dispensed\":%ld,\"requested\":%ld,\"cc\":\"%s\"}",
					poll->events[i].data1, poll->events[i].data2,
					poll->events[i].cc);
			break;
		case SSP_POLL_REJECTING:
			publishValidatorEvent("{\"event\":\"rejecting\"}");
			break;
		case SSP_POLL_REJECTED:
			// The note was rejected
			publishValidatorEvent("{\"event\":\"rejected\"}");
			break;
		case SSP_POLL_STACKING:
			publishValidatorEvent("{\"event\":\"stacking\"}");
			break;
		case SSP_POLL_STORED:
			// The note has been stored in the payout unit
			publishValidatorEvent("{\"event\":\"stored\"}");
			break;
		case SSP_POLL_STACKED:
			// The note has been stacked in the cashbox
			publishValidatorEvent("{\"event\":\"stacked\"}");
			break;
		case SSP_POLL_SAFE_JAM:
			publishValidatorEvent("{\"event\":\"safe jam\"}");
			break;
		case SSP_POLL_UNSAFE_JAM:
			publishValidatorEvent("{\"event\":\"unsafe jam\"}");
			break;
		case SSP_POLL_DISABLED:
			// The validator has been disabled
			publishValidatorEvent("{\"event\":\"disabled\"}");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			// The validator has detected a fraud attempt
			publishValidatorEvent(
					"{\"event\":\"fraud attempt\",\"dispensed\":%ld}",
					poll->events[i].data1);
			break;
		case SSP_POLL_STACKER_FULL:
			// The cashbox is full
			publishValidatorEvent("{\"event\":\"stacker full\"}");
			break;
		case SSP_POLL_CASH_BOX_REMOVED:
			// The cashbox has been removed
			publishValidatorEvent("{\"event\":\"cashbox removed\"}");
			break;
		case SSP_POLL_CASH_BOX_REPLACED:
			// The cashbox has been replaced
			publishValidatorEvent("{\"event\":\"cashbox replaced\"}");
			break;
		case SSP_POLL_CLEARED_FROM_FRONT:
			// A note was in the notepath at startup and has been cleared from the front of the validator
			publishValidatorEvent("{\"event\":\"cleared from front\"}");
			break;
		case SSP_POLL_CLEARED_INTO_CASHBOX:
			// A note was in the notepath at startup and has been cleared into the cashbox
			publishValidatorEvent("{\"event\":\"cleared into cashbox\"}");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			switch (poll->events[i].data1) {
			case NO_FAILUE:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"no error\"}");
				break;
			case SENSOR_FLAP:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"sensor flap\"}");
				break;
			case SENSOR_EXIT:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"sensor exit\"}");
				break;
			case SENSOR_COIL1:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"sensor coil 1\"}");
				break;
			case SENSOR_COIL2:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"sensor coil 2\"}");
				break;
			case NOT_INITIALISED:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"not initialized\"}");
				break;
			case CHECKSUM_ERROR:
				publishValidatorEvent("{\"event\":\"calibration fail\",\"error\":\"checksum error\"}");
				break;
			case COMMAND_RECAL:
				publishValidatorEvent("{\"event\":\"recalibrating\"}");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			// fallback only. in case we got a message which is not handled above.
			// if you can see this have a look in the SSP reference manual what
			// the message is about.
			publishValidatorEvent("{\"event\":\"unknown\",\"id\":\"0x%02X\"}", poll->events[i].event);
			break;
		}
	}
}

/**
 * \brief Initializes and configures redis, libevent and the hardware.
 */
void setup(struct m_metacash *metacash) {
	// initialize libEvent
	metacash->eventBase = event_base_new();

	// connect to redis
	redisPublishCtx = connectRedis(metacash); // establish connection for publishing
	redisSubscribeCtx = connectRedis(metacash); // establich connection for subscribing

	// setup redis
	if (redisPublishCtx && redisSubscribeCtx) {
		redisLibeventAttach(redisPublishCtx, metacash->eventBase);
		redisAsyncSetConnectCallback(redisPublishCtx, cbOnConnectPublishContext);
		redisAsyncSetDisconnectCallback(redisPublishCtx, cbOnDisconnectPublishContext);

		redisLibeventAttach(redisSubscribeCtx, metacash->eventBase);
		redisAsyncSetConnectCallback(redisSubscribeCtx, cbOnConnectSubscribeContext);
		redisAsyncSetDisconnectCallback(redisSubscribeCtx, cbOnDisconnectSubscribeContext);
	} else {
		die("could not establish connection to redis", 1);
		// never reached, already exited
	}

	// setup libevent triggered check if we should quit (every 500ms more or less)
	{
		struct timeval interval;
		interval.tv_sec = 0;
		interval.tv_usec = 500000;

		metacash->evCheckQuit = event_new(metacash->eventBase, 0, EV_PERSIST, cbOnCheckQuitEvent, metacash); // provide metacash in privdata
		evtimer_add(metacash->evCheckQuit, &interval);
	}

	// try to initialize the hardware only if we successfully have opened the device
	if (metacash->sspAvailable) {
		// prepare the device structures
		mcSspSetupCommand(&metacash->validator.sspC, metacash->validator.id);
		mcSspSetupCommand(&metacash->hopper.sspC, metacash->hopper.id);

		// initialize the devices
		mcSspInitializeDevice(&metacash->validator.sspC,
				metacash->validator.key, &metacash->validator);
		mcSspInitializeDevice(&metacash->hopper.sspC, metacash->hopper.key,
				&metacash->hopper);

		// only try to configure the hopper if it is available
		if (metacash->hopper.sspDeviceAvailable) {
			syslog(LOG_INFO, "setup of device '%s' started", metacash->hopper.name);

			enum channel_state desiredChannelState = DISABLED;

			if(metacash->acceptCoins) {
				desiredChannelState = ENABLED;
				syslog(LOG_WARNING, "coins will be accepted");
			} else {
				syslog(LOG_NOTICE, "coins will not be accepted");
			}

			// SMART Hopper configuration
			for (unsigned int i = 0; i < metacash->hopper.sspSetupReq.NumberOfChannels; i++) {
				ssp6_set_coinmech_inhibits(&metacash->hopper.sspC,
						metacash->hopper.sspSetupReq.ChannelData[i].value,
						metacash->hopper.sspSetupReq.ChannelData[i].cc, desiredChannelState);
			}

			syslog(LOG_NOTICE, "setup of device '%s' finished successfully", metacash->hopper.name);
		} else {
			syslog(LOG_WARNING, "skipping setup of device '%s' as it is not available", metacash->hopper.name);
		}

		// only try to configure the validator if it is available
		if (metacash->validator.sspDeviceAvailable) {
			syslog(LOG_INFO, "setup of device '%s' started", metacash->validator.name);

			// reject notes unfit for storage.
			// if this is not enabled, notes unfit for storage will be silently redirected
			// to the cashbox of the validator from which no payout can be done.
			if (mc_ssp_set_refill_mode(&metacash->validator.sspC)
					!= SSP_RESPONSE_OK) {
				syslog(LOG_WARNING, "setting refill mode failed");
			}

			// setup the routing of the banknotes in the validator (amounts are in cent)
			ssp6_set_route(&metacash->validator.sspC, 500, CURRENCY,
					SSP_OPTION_ROUTE_CASHBOX); // 5 euro
			ssp6_set_route(&metacash->validator.sspC, 1000, CURRENCY,
					SSP_OPTION_ROUTE_CASHBOX); // 10 euro
			ssp6_set_route(&metacash->validator.sspC, 2000, CURRENCY,
					SSP_OPTION_ROUTE_CASHBOX); // 20 euro
			ssp6_set_route(&metacash->validator.sspC, 5000, CURRENCY,
					SSP_OPTION_ROUTE_STORAGE); // 50 euro
			ssp6_set_route(&metacash->validator.sspC, 10000, CURRENCY,
					SSP_OPTION_ROUTE_STORAGE); // 100 euro
			ssp6_set_route(&metacash->validator.sspC, 20000, CURRENCY,
					SSP_OPTION_ROUTE_STORAGE); // 200 euro
			ssp6_set_route(&metacash->validator.sspC, 50000, CURRENCY,
					SSP_OPTION_ROUTE_STORAGE); // 500 euro

			metacash->validator.channelInhibits = 0x0; // disable all channels

			// set the inhibits in the hardware
			if (ssp6_set_inhibits(&metacash->validator.sspC, metacash->validator.channelInhibits, 0x0)
					!= SSP_RESPONSE_OK) {
				syslog(LOG_ERR, "Inhibits Failed\n");
				return;
			}

			//enable the payout unit
			if (ssp6_enable_payout(&metacash->validator.sspC,
					metacash->validator.sspSetupReq.UnitType)
					!= SSP_RESPONSE_OK) {
				syslog(LOG_ERR, "Enable Payout Failed\n");
				return;
			}

			syslog(LOG_NOTICE, "setup of device '%s' finished successfully", metacash->validator.name);
		} else {
			syslog(LOG_WARNING, "skipping configuration of device '%s' as it is not available", metacash->validator.name);
		}
	} else {
		syslog(LOG_WARNING, "SSP communication unavailable, skipping hardware setup");
	}

	// setup libevent triggered polling of the hardware (every second more or less)
	{
		struct timeval interval;
		interval.tv_sec = 1;
		interval.tv_usec = 0;

		metacash->evPoll = event_new(metacash->eventBase, 0, EV_PERSIST, cbOnPollEvent, metacash); // provide metacash in privdata
		evtimer_add(metacash->evPoll, &interval);
	}
}

/**
 * \brief Opens the serial device.
 */
int mcSspOpenSerialDevice(struct m_metacash *metacash) {
	// open the serial device
	syslog(LOG_INFO, "opening serial device: %s\n", metacash->serialDevice);

	{
		struct stat buffer;
		int fildes = open(metacash->serialDevice, O_RDWR);
		if (fildes <= 0) {
			syslog(LOG_ERR, "opening device %s failed: %s\n", metacash->serialDevice, strerror(errno));
			return 1;
		}

		fstat(fildes, &buffer); // TODO: error handling

		close(fildes);

		switch (buffer.st_mode & S_IFMT) {
		case S_IFCHR:
			break;
		default:
			syslog(LOG_ERR, "file %s is not a device\n", metacash->serialDevice);
			return 1;
		}
	}

	if (open_ssp_port(metacash->serialDevice) == 0) {
		syslog(LOG_ERR, "could not open serial device %s\n",
				metacash->serialDevice);
		return 1;
	}
	return 0;
}

/**
 * \brief Closes the serial device.
 */
void mcSspCloseSerialDevice(struct m_metacash *metacash) {
	close_ssp_port();
}

/**
 * \brief Issues a poll command to the hardware and dispatches the response to the event handler function of the device.
 */
void mcSspPollDevice(struct m_device *device, struct m_metacash *metacash) {
	SSP_POLL_DATA6 poll;

	hardwareWaitTime();

	// poll the unit
	SSP_RESPONSE_ENUM resp;
	if ((resp = ssp6_poll(&device->sspC, &poll)) != SSP_RESPONSE_OK) {
		if (resp == SSP_RESPONSE_TIMEOUT) {
			// If the poll timed out, then give up
			syslog(LOG_WARNING, "SSP Poll Timeout\n");
			return;
		} else {
			if (resp == SSP_RESPONSE_KEY_NOT_SET) {
				// The unit has responded with key not set, so we should try to negotiate one
				if (ssp6_setup_encryption(&device->sspC, device->key)
						!= SSP_RESPONSE_OK) {
					syslog(LOG_ERR, "Encryption Failed\n");
				} else {
					syslog(LOG_INFO, "Encryption Setup\n");
				}
			} else {
				syslog(LOG_ERR, "SSP Poll Error: 0x%x\n", resp);
			}
		}
	} else {
		if (poll.event_count > 0) {
			syslog(LOG_INFO, "parsing poll response from \"%s\" now (%d events)\n",
					device->name, poll.event_count);
			device->eventHandlerFn(device, metacash, &poll);
		} else {
			//printf("polling \"%s\" returned no events\n", device->name);
		}
	}
}

/**
 * \brief Initializes an ITL hardware device via SSP
 */
void mcSspInitializeDevice(SSP_COMMAND *sspC, unsigned long long key,
		struct m_device *device) {
	SSP6_SETUP_REQUEST_DATA *sspSetupReq = &device->sspSetupReq;
	syslog(LOG_NOTICE, "initializing device (id=0x%02X, '%s')\n", sspC->SSPAddress, device->name);

	//check device is present
	if (ssp6_sync(sspC) != SSP_RESPONSE_OK) {
		syslog(LOG_ERR, "No device found\n");
		return;
	}
	syslog(LOG_INFO, "device found\n");

	//try to setup encryption using the default key
	if (ssp6_setup_encryption(sspC, key) != SSP_RESPONSE_OK) {
		syslog(LOG_ERR, "Encryption failed\n");
		return;
	}
	syslog(LOG_INFO, "encryption setup\n");

	// Make sure we are using ssp version 6
	if (ssp6_host_protocol(sspC, 0x06) != SSP_RESPONSE_OK) {
		syslog(LOG_ERR, "Host Protocol Failed\n");
		return;
	}
	syslog(LOG_INFO, "host protocol verified\n");

	// Collect some information about the device
	if (ssp6_setup_request(sspC, sspSetupReq) != SSP_RESPONSE_OK) {
		syslog(LOG_ERR, "Setup Request Failed\n");
		return;
	}

	syslog(LOG_INFO, "channels:\n");
	for (unsigned int i = 0; i < sspSetupReq->NumberOfChannels; i++) {
		syslog(LOG_INFO, "channel %d: %d %s\n", i + 1, sspSetupReq->ChannelData[i].value,
				sspSetupReq->ChannelData[i].cc);
	}

	char version[100];
	mc_ssp_get_firmware_version(sspC, &version[0]);
	syslog(LOG_INFO, "full firmware version: %s\n", version);

	mc_ssp_get_dataset_version(sspC, &version[0]);
	syslog(LOG_INFO, "full dataset version : %s\n", version);

	//enable the device
	if (ssp6_enable(sspC) != SSP_RESPONSE_OK) {
		syslog(LOG_ERR, "Enable Failed\n");
		return;
	}

	device->sspDeviceAvailable = 1;
	syslog(LOG_NOTICE, "device has been successfully initialized (id=0x%02X, '%s')\n", sspC->SSPAddress, device->name);
}

/**
 * \brief Initializes the SSP_COMMAND structure.
 */
void mcSspSetupCommand(SSP_COMMAND *sspC, int deviceId) {
	sspC->SSPAddress = deviceId;
	sspC->Timeout = 1000;
	sspC->EncryptionStatus = NO_ENCRYPTION;
	sspC->RetryLevel = 3;
	sspC->BaudRate = 9600;
}

/**
 * \brief Implements the "LAST REJECT NOTE" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_last_reject_note(SSP_COMMAND *sspC,
		unsigned char *reason) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_LAST_REJECT_NOTE;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	*reason = sspC->ResponseData[1];

	return resp;
}

/**
 * \brief Implements the "DISPLAY ON" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_display_on(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_DISPLAY_ON;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * \brief Implements the "DISPLAY OFF" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_display_off(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_DISPLAY_OFF;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * \brief Implements the "SET REFILL MODE" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_set_refill_mode(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 9;
	sspC->CommandData[0] = SSP_CMD_SET_REFILL_MODE;

	// these are all magic constants, no idea why ITL specified it that way
	// in the protocol.
	sspC->CommandData[1] = 0x05;
	sspC->CommandData[2] = 0x81;
	sspC->CommandData[3] = 0x10;
	sspC->CommandData[4] = 0x11;
	sspC->CommandData[5] = 0x01;
	sspC->CommandData[6] = 0x01;
	sspC->CommandData[7] = 0x52;
	sspC->CommandData[8] = 0xF5;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * \brief Implements the "EMPTY" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_empty(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_EMPTY;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * \brief Implements the "SMART EMPTY" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_smart_empty(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_SMART_EMPTY;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}


SSP_RESPONSE_ENUM mc_ssp_cashbox_payout_operation_data(SSP_COMMAND *sspC, char **json) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_CASHBOX_PAYOUT_OPERATION_DATA;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	if(resp != SSP_RESPONSE_OK) {
		return resp;
	}

	/* The first data byte in the response is the number of counters returned. Each counter consists of 9 bytes of
	 * data made up as: 2 bytes giving the denomination level, 4 bytes giving the value and 3 bytes of ASCII country
	 * code. The last 4 bytes of data indicate the quantity of coins which could not be identified.
	 */

	int i = 0;

	i++; // move onto numCounters
	int numCounters = sspC->ResponseData[i];

	/* Create StringBuffer 'object' (struct) */
	SB *sb = getStringBuffer();

	int j; // current counter
	for (j = 0; j < numCounters; ++j) {
		int k;

		int value = 0;
		int level = 0;
		char cc[4] = {0};

		for (k = 0; k < 2; ++k) {
			i++; //move through the 2 bytes of data
			level +=
					(((unsigned long) sspC->ResponseData[i])
							<< (8 * k));
		}
		for (k = 0; k < 4; ++k) {
			i++; //move through the 4 bytes of data
			value +=
					(((unsigned long) sspC->ResponseData[i])
							<< (8 * k));
		}
		for (k = 0; k < 3; ++k) {
			i++; //move through the 3 bytes of country code
			cc[k] =
					sspC->ResponseData[i];
		}

		char *response = NULL;
		asprintf(&response,
				"{\"value\":%d,\"level\":%d,\"cc\":\"%s\"}", value, level, cc);

		if(j > 0) {
			char *sep = ",";
			sb->append( sb, sep); // json array seperator
		}
		sb->append( sb, response);

		free(response);
	}

	/* quantity of unknown coins */
	{
		int qtyUnknown = 0;
		for (int k = 0; k < 3; ++k) {
			i++; //move through the 4 bytes of data
			qtyUnknown +=
					(((unsigned long) sspC->ResponseData[i])
							<< (8 * k));
		}
		char *response = NULL;
		asprintf(&response,
				",{\"value\":0,\"level\":%d}", qtyUnknown);
		// json array seperator and value are constant here

		sb->append( sb, response);

		free(response);
	}

	/* Call toString() function to get catenated list */
	char *result = sb->toString( sb );

	asprintf(json, "%s", result);

	/* Dispose of StringBuffer's memory */
	sb->dispose( &sb ); /* Note: Need to pass ADDRESS of struct pointer to dispose() */

	return resp;
}


/**
 * \brief Implements the "CONFIGURE BEZEL" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_configure_bezel(SSP_COMMAND *sspC, unsigned char r,
		unsigned char g, unsigned char b, unsigned char volatileOption, unsigned char bezelTypeOption) {
	sspC->CommandDataLength = 6;
	sspC->CommandData[0] = SSP_CMD_CONFIGURE_BEZEL;
	sspC->CommandData[1] = r;
	sspC->CommandData[2] = g;
	sspC->CommandData[3] = b;
	sspC->CommandData[4] = volatileOption;
	sspC->CommandData[5] = bezelTypeOption;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * \brief Implements the "SET DENOMINATION LEVEL" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_set_denomination_level(SSP_COMMAND *sspC, int amount, int level, const char *cc) {
	sspC->CommandDataLength = 10;
	sspC->CommandData[0] = SSP_CMD_SET_DENOMINATION_LEVEL;

	int j = 0;
	int i;

	for (i = 0; i < 2; i++) {
		sspC->CommandData[++j] = level >> (i * 8);
	}

	for (i = 0; i < 4; i++) {
		sspC->CommandData[++j] = amount >> (i * 8);
	}

	for (i = 0; i < 3; i++) {
		sspC->CommandData[++j] = cc[i];
	}

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// not data to parse

	return resp;
}

/**
 * \brief Implements the "SET CASHBOX PAYOUT LIMIT" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_set_cashbox_payout_limit(SSP_COMMAND *sspC, unsigned int limit, int denomination, const char *cc) {
	sspC->CommandDataLength = 11;
	sspC->CommandData[0] = SSP_CMD_SET_CASHBOX_PAYOUT_LIMIT;

	int j = 0;
	sspC->CommandData[++j] = 1; // only one limit can be set at once for now

	int i;

	for (i = 0; i < 2; i++) {
		sspC->CommandData[++j] = limit >> (i * 8);
	}

	for (i = 0; i < 4; i++) {
		sspC->CommandData[++j] = denomination >> (i * 8);
	}

	for (i = 0; i < 3; i++) {
		sspC->CommandData[++j] = cc[i];
	}

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// not data to parse

	return resp;
}


/**
 * \brief Implements the "GET ALL LEVELS" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_get_all_levels(SSP_COMMAND *sspC, char **json) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_GET_ALL_LEVELS;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	/* The first data byte in the response is the number of counters returned. Each counter consists of 9 bytes of
	 * data made up as: 2 bytes giving the denomination level, 4 bytes giving the value and 3 bytes of ASCII country
	 * code.
	 */

	int i = 0;

	i++; // move onto numCounters
	int numCounters = sspC->ResponseData[i];

	/* Create StringBuffer 'object' (struct) */
	SB *sb = getStringBuffer();

	int j; // current counter
	for (j = 0; j < numCounters; ++j) {
		int k;

		int value = 0;
		int level = 0;
		char cc[4] = {0};

		for (k = 0; k < 2; ++k) {
			i++; //move through the 2 bytes of data
			level +=
					(((unsigned long) sspC->ResponseData[i])
							<< (8 * k));
		}
		for (k = 0; k < 4; ++k) {
			i++; //move through the 4 bytes of data
			value +=
					(((unsigned long) sspC->ResponseData[i])
							<< (8 * k));
		}
		for (k = 0; k < 3; ++k) {
			i++; //move through the 3 bytes of country code
			cc[k] =
					sspC->ResponseData[i];
		}

		char *response = NULL;
		asprintf(&response,
				"{\"value\":%d,\"level\":%d,\"cc\":\"%s\"}", value, level, cc);

		if(j > 0) {
			char *sep = ",";
			sb->append( sb, sep); // json array seperator
		}
		sb->append( sb, response);

		free(response);
	}

	/* Call toString() function to get catenated list */
	char *result = sb->toString( sb );

	asprintf(json, "%s", result);

	/* Dispose of StringBuffer's memory */
	sb->dispose( &sb ); /* Note: Need to pass ADDRESS of struct pointer to dispose() */

	return resp;
}

/**
 * \brief Implements the "FLOAT" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_float(SSP_COMMAND *sspC, const int value,
		const char *cc, const char option) {
	int i;

	sspC->CommandDataLength = 11;
	sspC->CommandData[0] = SSP_CMD_FLOAT;

	int j = 0;

	// minimum requested value to float
	for (i = 0; i < 2; i++) {
		sspC->CommandData[++j] = 100 >> (i * 8); // min 1 euro
	}

	// amount to keep for payout
	for (i = 0; i < 4; i++) {
		sspC->CommandData[++j] = value >> (i * 8) ;
	}

	for (i = 0; i < 3; i++) {
		sspC->CommandData[++j] = cc[i];
	}

	sspC->CommandData[++j] = option;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

/**
 * \brief Implements the "GET FIRMWARE VERSION" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_get_firmware_version(SSP_COMMAND *sspC, char *firmwareVersion) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_GET_FIRMWARE_VERSION;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];
	if(resp == SSP_RESPONSE_OK) {
		for(int i = 0; i < 16; i++) {
			*(firmwareVersion + i) = sspC->ResponseData[1 + i];
		}
		*(firmwareVersion + 16) = 0;
	}

	return resp;
}

/**
 * \brief Implements the "GET DATASET VERSION" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_get_dataset_version(SSP_COMMAND *sspC, char *datasetVersion) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_GET_DATASET_VERSION;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];
	if(resp == SSP_RESPONSE_OK) {
		for(int i = 0; i < 8; i++) {
			*(datasetVersion + i) = sspC->ResponseData[1 + i];
		}
		*(datasetVersion + 8) = 0;
	}

	return resp;
}

/**
 * \brief Implements the "CHANNEL SECURITY DATA" command from the SSP Protocol.
 */
SSP_RESPONSE_ENUM mc_ssp_channel_security_data(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = SSP_CMD_CHANNEL_SECURITY;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];
	if(resp == SSP_RESPONSE_OK) {
		int numChannels = sspC->ResponseData[1];

		syslog(LOG_DEBUG, "security status: numChannels=%d\n", numChannels);
		syslog(LOG_DEBUG, "0 = unused, 1 = low, 2 = std, 3 = high, 4 = inhibited\n");
		for(int i = 0; i < numChannels; i++) {
			syslog(LOG_DEBUG, "security status: channel %d -> %d\n", 1 + i, sspC->ResponseData[2 + i]);
		}
	}

	return resp;
}
