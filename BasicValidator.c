#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "port_linux.h"
#include "ssp_defines.h"
#include "ssp_helpers.h"
#include "SSPComs.h"

#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libevent.h>

#include <syslog.h>

struct m_credit {
	unsigned long amount;
};

struct m_metacash;

struct m_device {
	int id;
	char *name;
	unsigned long long key;

	SSP_COMMAND sspC;
	SSP6_SETUP_REQUEST_DATA setup_req;
	void (*parsePoll)(struct m_device *device, struct m_metacash *metacash,
			SSP_POLL_DATA6 *poll);
};

struct m_metacash {
	int quit;
	char *serialDevice;

	int redisPort;
	char *redisHost;
	redisAsyncContext *db;		// redis context used for persistence
	redisAsyncContext *pubSub;	// redis context used for messaging (publish / subscribe)

	struct event_base *eventBase;	// libevent
	struct event evPoll; 			// event for periodically polling the cash hardware
	struct event evCheckQuit;		// event for periodically checking to quit

	struct m_credit credit;

	struct m_device hopper;
	struct m_device validator;
};

// mc_ssp_* : metacash ssp functions (cash hardware, low level)
int mc_ssp_open_serial_device(struct m_metacash *metacash);
void mc_ssp_close_serial_device(struct m_metacash *metacash);
void mc_ssp_setup_command(SSP_COMMAND *sspC, int deviceId);
void mc_ssp_initialize_device(SSP_COMMAND *sspC, unsigned long long key);
SSP_RESPONSE_ENUM mc_ssp_empty(SSP_COMMAND *sspC);
void mc_ssp_payout(SSP_COMMAND *sspC, int amount, char *cc);
void mc_ssp_poll_device(struct m_device *device, struct m_metacash *metacash);
SSP_RESPONSE_ENUM mc_ssp_configure_bezel(SSP_COMMAND *sspC, unsigned char r,
		unsigned char g, unsigned char b, unsigned char non_volatile);
SSP_RESPONSE_ENUM mc_ssp_display_on(SSP_COMMAND *sspC);
SSP_RESPONSE_ENUM mc_ssp_display_off(SSP_COMMAND *sspC);

// metacash
int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash);
void mc_setup(struct m_metacash *metacash);

// cash hardware result parsing
void mc_handle_events_hopper(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll);
void mc_handle_events_validator(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll);

static const char *CURRENCY = "EUR";
static const char ROUTE_CASHBOX = 0x01;
static const char ROUTE_STORAGE = 0x00;
static const unsigned long long DEFAULT_KEY = 0x123456701234567LL;

int receivedSignal = 0;

void interrupt(int signal) {
	receivedSignal = signal;
}

redisAsyncContext* mc_connect_redis(struct m_metacash *metacash) {
	redisAsyncContext *conn = redisAsyncConnect(metacash->redisHost, metacash->redisPort);

	if(conn == NULL || conn->err) {
		if(conn) {
			fprintf(stderr, "fatal: Connection error: %s\n", conn->errstr);
		} else {
			fprintf(stderr, "fatal: Connection error: can't allocate redis context\n");
		}
	} else {
		// reference the metcash struct in data for use in connect/disconnect callback
		conn->data = metacash;
	}

	return conn;
}

void cbPollEvent(int fd, short event, void *privdata) {
	struct m_metacash *metacash = privdata;

	// cash hardware
	unsigned long amountBeforePoll = metacash->credit.amount;

	mc_ssp_poll_device(&metacash->hopper, metacash);
	mc_ssp_poll_device(&metacash->validator, metacash);

	if (metacash->credit.amount != amountBeforePoll) {
		printf("current credit now: %ld cents\n", metacash->credit.amount);
		// TODO: publish new amount of credit
	}
}

void cbCheckQuit(int fd, short event, void *privdata) {
	// TODO: check quit flag for occured signal
}

void cbOnTestTopicMessage(redisAsyncContext *c, void *reply, void *privdata) {
	printf("onMessageInTestTopicFunction: received a message via test-topic\n");

	struct m_metacash *m = c->data;
	redisAsyncCommand(m->db, NULL, NULL, "INCR test-msg-counter");
}

void cbConnectDb(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "Database error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "Connected to database...\n");

	// redisCommand(c->c, "GET credit"); // TODO: read the current credit sync and store in metacash
	// redisAsyncCommand(db, NULL, NULL, "SET component:ssp 1");
}

void cbDisconnectDb(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "Database error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "Disconnected from database\n");
}

void cbConnectPubSub(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "PubSub - Database error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "PubSub - Connected to database...\n");

	redisAsyncContext *cNotConst = (redisAsyncContext*) c; // get rids of discarding qualifier 'const' warning

	redisAsyncCommand(cNotConst, cbOnTestTopicMessage, NULL, "SUBSCRIBE test-topic");
}

void cbDisconnectPubSub(const redisAsyncContext *c, int status) {
	if (status != REDIS_OK) {
		fprintf(stderr, "PubSub - Database error: %s\n", c->errstr);
		return;
	}
	fprintf(stderr, "PubSub - Disconnected from database\n");
}

int main(int argc, char *argv[]) {
	setlogmask(LOG_UPTO (LOG_NOTICE));

	openlog("metacashd", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

	syslog(LOG_NOTICE, "Program started by User %d", getuid());
	syslog(LOG_INFO, "A tree falls in a forest");

	struct m_metacash metacash;
	metacash.quit = 0;
	metacash.credit.amount = 0;
	metacash.serialDevice = "/dev/ttyACM0";	// default, override with -d argument
	metacash.redisHost = "127.0.0.1";		// default, override with -h argument
	metacash.redisPort = 6379;				// default, override with -p argument

	metacash.hopper.id = 0x10; // 0X10 -> Smart Hopper ("Münzer")
	metacash.hopper.name = "Mr. Coin";
	metacash.hopper.key = DEFAULT_KEY;
	metacash.hopper.parsePoll = mc_handle_events_hopper;

	metacash.validator.id = 0x00; // 0x00 -> Smart Payout NV200 ("Scheiner")
	metacash.validator.name = "Ms. Note";
	metacash.validator.key = DEFAULT_KEY;
	metacash.validator.parsePoll = mc_handle_events_validator;

	// parse the command line arguments
	if (parseCmdLine(argc, argv, &metacash)) {
		return 1;
	}

	syslog(LOG_NOTICE, "using redis at %s:%d and hardware device %s",
			metacash.redisHost, metacash.redisPort, metacash.serialDevice);

	// open the serial device
	if (mc_ssp_open_serial_device(&metacash)) {
		return 1;
	}

	// setup the ssp commands, configure and initialize the hardware
	mc_setup(&metacash);

	printf("metacash open for business :D\n\n");

	event_base_dispatch(metacash.eventBase); // TODO: will return if broken via libevent_breakloop

	mc_ssp_close_serial_device(&metacash);

	closelog ();

	return 0;
}

int parseCmdLine(int argc, char *argv[], struct m_metacash *metacash) {
	opterr = 0;

	char c;
	while ((c = getopt(argc, argv, "h:p:d:")) != -1)
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
		case '?':
			if (optopt == 'h' || optopt == 'p' || optopt == 'd')
				fprintf(stderr, "Option -%c requires an argument.\n", optopt);
			else if (isprint(optopt))
				fprintf(stderr, "Unknown option `-%c'.\n", optopt);
			else
				fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
			return 1;
		default:
			return 1;
		}

	return 0;
}

// business stuff
void mc_handle_events_hopper(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {
	int i;
	for (i = 0; i < poll->event_count; ++i) {
		printf("processing event #%03d (0x%02X): ", i, poll->events[i].event);

		switch (poll->events[i].event) {
		case SSP_POLL_RESET:
			printf("Unit Reset\n");
			// Make sure we are using ssp version 6
			if (ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				printf("Host Protocol Failed\n");
				return;
			}
			break;
		case SSP_POLL_COIN_CREDIT:
			printf("Credit coin\n");
			metacash->credit.amount++; // hopper will report all coins as 1 cent m(
			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			printf("Incomplete payout %ld of %ld %s\n", poll->events[i].data1,
					poll->events[i].data2, poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			printf("Incomplete float %ld of %ld %s\n", poll->events[i].data1,
					poll->events[i].data2, poll->events[i].cc);
			break;
		case SSP_POLL_DISPENSING:
			printf("Dispensing\n");
			break;
		case SSP_POLL_DISPENSED:
			printf("Dispensed\n");
			break;
		case SSP_POLL_DISABLED:
			printf("DISABLED\n");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			printf("Calibration fail: ");

			switch (poll->events[i].data1) {
			case NO_FAILUE:
				printf("No failure\n");
				break;
			case SENSOR_FLAP:
				printf("Optical sensor flap\n");
				break;
			case SENSOR_EXIT:
				printf("Optical sensor exit\n");
				break;
			case SENSOR_COIL1:
				printf("Coil sensor 1\n");
				break;
			case SENSOR_COIL2:
				printf("Coil sensor 2\n");
				break;
			case NOT_INITIALISED:
				printf("Unit not initialised\n");
				break;
			case CHECKSUM_ERROR:
				printf("Data checksum error\n");
				break;
			case COMMAND_RECAL:
				printf("Recalibration by command required\n");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			printf("unknown event\n");
			break;
		}
	}
}

void mc_handle_events_validator(struct m_device *device,
		struct m_metacash *metacash, SSP_POLL_DATA6 *poll) {
	int i;
	for (i = 0; i < poll->event_count; ++i) {
		printf("processing event #%03d (0x%02X): ", i, poll->events[i].event);

		switch (poll->events[i].event) {
		case SSP_POLL_RESET:
			printf("Unit Reset\n");
			// Make sure we are using ssp version 6
			if (ssp6_host_protocol(&device->sspC, 0x06) != SSP_RESPONSE_OK) {
				printf("Host Protocol Failed\n");
				return;
			}
			break;
		case SSP_POLL_DISPENSING:
			printf("Dispensing\n");
			break;
		case SSP_POLL_DISPENSED:
			printf("Dispensed\n");
			break;
		case SSP_POLL_READ:
			// the 'read' event contains 1 data value, which if >0 means a note has been validated and is in escrow
			if (poll->events[i].data1 > 0) {
				printf("Note Read %ld\n", poll->events[i].data1);
			} else {
				printf("Note Read (still scanning)\n");
			}
			break;
		case SSP_POLL_CREDIT:
			printf("Credit: Note %ld %s\n", poll->events[i].data1,
					poll->events[i].cc);

			int euro = 0;
			switch (poll->events[i].data1) {
			case 1:
				euro = 5;
				break;
			case 2:
				euro = 10;
				break;
			case 3:
				euro = 20;
				break;
			case 4:
				euro = 50;
				break;
			case 5:
				euro = 100;
				break;
			case 6:
				euro = 200;
				break;
			case 7:
				euro = 500;
				break;
			default:
				break;
			}

			metacash->credit.amount += euro * 100;

			break;
		case SSP_POLL_INCOMPLETE_PAYOUT:
			// the validator shutdown during a payout, this event is reporting that some value remains to payout
			printf("Incomplete payout %ld of %ld %s\n", poll->events[i].data1,
					poll->events[i].data2, poll->events[i].cc);
			break;
		case SSP_POLL_INCOMPLETE_FLOAT:
			// the validator shutdown during a float, this event is reporting that some value remains to float
			printf("Incomplete float %ld of %ld %s\n", poll->events[i].data1,
					poll->events[i].data2, poll->events[i].cc);
			break;
		case SSP_POLL_REJECTING:
			printf("Note Rejecting\n");
			break;
		case SSP_POLL_REJECTED:
			printf("Note Rejected\n");
			break;
		case SSP_POLL_STACKING:
			printf("Stacking\n");
			break;
		case SSP_POLL_STORED:
			printf("Stored\n");
			break;
		case SSP_POLL_STACKED:
			printf("Stacked\n");
			break;
		case SSP_POLL_SAFE_JAM:
			printf("Safe Jam\n");
			break;
		case SSP_POLL_UNSAFE_JAM:
			printf("Unsafe Jam\n");
			break;
		case SSP_POLL_DISABLED:
			printf("DISABLED\n");
			break;
		case SSP_POLL_FRAUD_ATTEMPT:
			printf("Fraud Attempt %ld %s\n", poll->events[i].data1,
					poll->events[i].cc);
			break;
		case SSP_POLL_STACKER_FULL:
			printf("Stacker Full\n");
			break;
		case SSP_POLL_CASH_BOX_REMOVED:
			printf("Cashbox Removed\n");
			break;
		case SSP_POLL_CASH_BOX_REPLACED:
			printf("Cashbox Replaced\n");
			break;
		case SSP_POLL_CLEARED_FROM_FRONT:
			printf("Cleared from front\n");
			break;
		case SSP_POLL_CLEARED_INTO_CASHBOX:
			printf("Cleared Into Cashbox\n");
			break;
		case SSP_POLL_CALIBRATION_FAIL:
			// the hopper calibration has failed. An extra byte is available with an error code.
			printf("Calibration fail: ");

			switch (poll->events[i].data1) {
			case NO_FAILUE:
				printf("No failure\n");
				break;
			case SENSOR_FLAP:
				printf("Optical sensor flap\n");
				break;
			case SENSOR_EXIT:
				printf("Optical sensor exit\n");
				break;
			case SENSOR_COIL1:
				printf("Coil sensor 1\n");
				break;
			case SENSOR_COIL2:
				printf("Coil sensor 2\n");
				break;
			case NOT_INITIALISED:
				printf("Unit not initialised\n");
				break;
			case CHECKSUM_ERROR:
				printf("Data checksum error\n");
				break;
			case COMMAND_RECAL:
				printf("Recalibration by command required\n");
				ssp6_run_calibration(&device->sspC);
				break;
			}
			break;
		default:
			printf("unknown event\n");
			break;
		}
	}
}

void mc_setup(struct m_metacash *metacash) {
	// initialize libEvent
	metacash->eventBase = event_base_new();

	// connect to redis
	metacash->db = mc_connect_redis(metacash);		// establish connection for persistence
	metacash->pubSub = mc_connect_redis(metacash);	// establich connection for pub/sub

	// setup redis
	if(metacash->db && metacash->pubSub) {
		redisLibeventAttach(metacash->db, metacash->eventBase);
		redisAsyncSetConnectCallback(metacash->db, cbConnectDb);
		redisAsyncSetDisconnectCallback(metacash->db, cbDisconnectDb);

		redisLibeventAttach(metacash->pubSub, metacash->eventBase);
		redisAsyncSetConnectCallback(metacash->pubSub, cbConnectPubSub);
		redisAsyncSetDisconnectCallback(metacash->pubSub, cbDisconnectPubSub);
	}

	// prepare the device structures
	mc_ssp_setup_command(&metacash->validator.sspC, metacash->validator.id);
	mc_ssp_setup_command(&metacash->hopper.sspC, metacash->hopper.id);

	// initialize the devices
	printf("\n");
	mc_ssp_initialize_device(&metacash->validator.sspC,
			metacash->validator.key);
	printf("\n");
	mc_ssp_initialize_device(&metacash->hopper.sspC, metacash->hopper.key);
	printf("\n");

	// setup the routing of the banknotes in the validator (amounts are in cent)
	ssp6_set_route(&metacash->validator.sspC, 500, CURRENCY, ROUTE_STORAGE); // 5 euro
	ssp6_set_route(&metacash->validator.sspC, 1000, CURRENCY, ROUTE_STORAGE); // 10 euro
	ssp6_set_route(&metacash->validator.sspC, 2000, CURRENCY, ROUTE_STORAGE); // 20 euro
	ssp6_set_route(&metacash->validator.sspC, 5000, CURRENCY, ROUTE_CASHBOX); // 50 euro
	ssp6_set_route(&metacash->validator.sspC, 10000, CURRENCY, ROUTE_CASHBOX); // 100 euro
	ssp6_set_route(&metacash->validator.sspC, 20000, CURRENCY, ROUTE_CASHBOX); // 200 euro
	ssp6_set_route(&metacash->validator.sspC, 50000, CURRENCY, ROUTE_CASHBOX); // 500 euro

	// setup libevent triggered polling of the hardware (every second more or less)
	{
		struct timeval interval;
		interval.tv_sec = 1;
		interval.tv_usec = 0;

		event_set(&metacash->evPoll, 0, EV_PERSIST, cbPollEvent, &metacash); // provide metacash in privdata
		event_base_set(metacash->eventBase, &metacash->evPoll);
		evtimer_add(&metacash->evPoll, &interval);
	}

	// setup libevent triggered check if we should quit (every 200ms more or less)
	{
		struct timeval interval;
		interval.tv_sec = 0;
		interval.tv_usec = 200;

		event_set(&metacash->evCheckQuit, 0, EV_PERSIST, cbCheckQuit, &metacash); // provide metacash in privdata
		event_base_set(metacash->eventBase, &metacash->evCheckQuit);
		evtimer_add(&metacash->evCheckQuit, &interval);
	}
}

int mc_ssp_open_serial_device(struct m_metacash *metacash) {
	// open the serial device
	printf("opening serial device: %s\n", metacash->serialDevice);

	{
		struct stat buffer;
		int fildes = open(metacash->serialDevice, O_RDWR);
		if (fildes == 0) {
			printf("ERROR: device %s not found\n", metacash->serialDevice);

			return 1;
		}

		fstat(fildes, &buffer); // TODO: error handling

		close(fildes);

		switch (buffer.st_mode & S_IFMT) {
		case S_IFCHR:
			break;
		default:
			printf("ERROR: %s is not a device\n", metacash->serialDevice);
			return 1;
		}
	}

	if (open_ssp_port(metacash->serialDevice) == 0) {
		printf("ERROR: could not open serial device %s\n",
				metacash->serialDevice);
		return 1;
	}
	return 0;
}

void mc_ssp_close_serial_device(struct m_metacash *metacash) {
	close_ssp_port();
}

void mc_ssp_poll_device(struct m_device *device, struct m_metacash *metacash) {
	SSP_POLL_DATA6 poll;

	// poll the unit
	SSP_RESPONSE_ENUM resp;
	if ((resp = ssp6_poll(&device->sspC, &poll)) != SSP_RESPONSE_OK) {
		if (resp == SSP_RESPONSE_TIMEOUT) {
			// If the poll timed out, then give up
			printf("SSP Poll Timeout\n");
			return;
		} else {
			if (resp == SSP_RESPONSE_KEY_NOT_SET) {
				// The unit has responded with key not set, so we should try to negotiate one
				if (ssp6_setup_encryption(&device->sspC, device->key)
						!= SSP_RESPONSE_OK) {
					printf("Encryption Failed\n");
				} else {
					printf("Encryption Setup\n");
				}
			} else {
				printf("SSP Poll Error: 0x%x\n", resp);
			}
		}
	} else {
		if (poll.event_count > 0) {
			printf("parsing poll response from '%s' now (%d events)\n",
					device->name, poll.event_count);
			device->parsePoll(device, metacash, &poll);
		} else {
			//printf("polling '%s' returned no events\n", device->name);
		}
	}
}

void mc_ssp_initialize_device(SSP_COMMAND *sspC, unsigned long long key) {
	SSP6_SETUP_REQUEST_DATA setup_req;
	unsigned int i = 0;

	printf("initializing device (id=0x%02X)\n", sspC->SSPAddress);

	//check device is present
	if (ssp6_sync(sspC) != SSP_RESPONSE_OK) {
		printf("ERROR: No device found\n");
		return;
	}
	printf("device found\n");

	//try to setup encryption using the default key
	if (ssp6_setup_encryption(sspC, key) != SSP_RESPONSE_OK) {
		printf("ERROR: Encryption failed\n");
		return;
	}
	printf("encryption setup\n");

	// Make sure we are using ssp version 6
	if (ssp6_host_protocol(sspC, 0x06) != SSP_RESPONSE_OK) {
		printf("ERROR: Host Protocol Failed\n");
		return;
	}
	printf("host protocol verified\n");

	// Collect some information about the device
	if (ssp6_setup_request(sspC, &setup_req) != SSP_RESPONSE_OK) {
		printf("ERROR: Setup Request Failed\n");
		return;
	}

	printf("firmware: %s\n", setup_req.FirmwareVersion);
	printf("channels:\n");
	for (i = 0; i < setup_req.NumberOfChannels; i++) {
		printf("channel %d: %d %s\n", i + 1, setup_req.ChannelData[i].value,
				setup_req.ChannelData[i].cc);
	}

	//enable the device
	if (ssp6_enable(sspC) != SSP_RESPONSE_OK) {
		printf("ERROR: Enable Failed\n");
		return;
	}

	if (setup_req.UnitType == 0x03) {
		// SMART Hopper requires different inhibit commands
		for (i = 0; i < setup_req.NumberOfChannels; i++) {
			ssp6_set_coinmech_inhibits(sspC, setup_req.ChannelData[i].value,
					setup_req.ChannelData[i].cc, ENABLED);
		}
	} else {
		if (setup_req.UnitType == 0x06 || setup_req.UnitType == 0x07) {
			//enable the payout unit
			if (ssp6_enable_payout(sspC, setup_req.UnitType)
					!= SSP_RESPONSE_OK) {
				printf("ERROR: Enable Failed\n");
				return;
			}
		}

		// set the inhibits (enable all note acceptance)
		if (ssp6_set_inhibits(sspC, 0xFF, 0xFF) != SSP_RESPONSE_OK) {
			printf("ERROR: Inhibits Failed\n");
			return;
		}
	}

	printf("device has been successfully initialized\n");
}

void mc_ssp_setup_command(SSP_COMMAND *sspC, int deviceId) {
	sspC->SSPAddress = deviceId;
	sspC->Timeout = 1000;
	sspC->EncryptionStatus = NO_ENCRYPTION;
	sspC->RetryLevel = 3;
	sspC->BaudRate = 9600;
}

void mc_ssp_payout(SSP_COMMAND *sspC, int amount, char *cc) {
	if (amount > 0) {
		// send the test payout command
		if (ssp6_payout(sspC, amount, cc, SSP6_OPTION_BYTE_TEST)
				!= SSP_RESPONSE_OK) {

			printf("Test: Payout would fail");
			// when the payout fails it should return 0xf5 0xNN, where 0xNN is an error code
			switch (sspC->ResponseData[1]) {
			case 0x01:
				printf(": Not enough value in Smart Payout\n");
				break;
			case 0x02:
				printf(": Cant pay exact amount\n");
				break;
			case 0x03:
				printf(": Smart Payout Busy\n");
				break;
			case 0x04:
				printf(": Smart Payout Disabled\n");
				break;
			default:
				printf("\n");
			}

			return;
		}

		// send the payout command
		if (ssp6_payout(sspC, amount, cc, SSP6_OPTION_BYTE_DO)
				!= SSP_RESPONSE_OK) {

			printf("ERROR: Payout failed");
			// when the payout fails it should return 0xf5 0xNN, where 0xNN is an error code
			switch (sspC->ResponseData[1]) {
			case 0x01:
				printf(": Not enough value in Smart Payout\n");
				break;
			case 0x02:
				printf(": Cant pay exact amount\n");
				break;
			case 0x03:
				printf(": Smart Payout Busy\n");
				break;
			case 0x04:
				printf(": Smart Payout Disabled\n");
				break;
			default:
				printf("\n");
			}
		}
	}
}

SSP_RESPONSE_ENUM mc_ssp_display_on(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = 0x3;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

SSP_RESPONSE_ENUM mc_ssp_display_off(SSP_COMMAND *sspC) {
	sspC->CommandDataLength = 1;
	sspC->CommandData[0] = 0x4;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}

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

SSP_RESPONSE_ENUM mc_ssp_configure_bezel(SSP_COMMAND *sspC, unsigned char r,
		unsigned char g, unsigned char b, unsigned char non_volatile) {
	sspC->CommandDataLength = 5;
	sspC->CommandData[0] = 0x54;
	sspC->CommandData[1] = r;
	sspC->CommandData[2] = g;
	sspC->CommandData[3] = b;
	sspC->CommandData[4] = non_volatile;

	//CHECK FOR TIMEOUT
	if (send_ssp_command(sspC) == 0) {
		return SSP_RESPONSE_TIMEOUT;
	}

	// extract the device response code
	SSP_RESPONSE_ENUM resp = (SSP_RESPONSE_ENUM) sspC->ResponseData[0];

	// no data to parse

	return resp;
}