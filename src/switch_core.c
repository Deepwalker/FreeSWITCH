/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005/2006, Anthony Minessale II <anthmct@yahoo.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthmct@yahoo.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthmct@yahoo.com>
 *
 *
 * switch_core.c -- Main Core Library
 *
 */
#include <switch.h>

#ifdef EMBED_PERL
#include <EXTERN.h>
#include <perl.h>

static char *embedding[] = { "", "-e", ""};
EXTERN_C void xs_init (pTHX);
#endif

//#include <sys/types.h>
//#include <sys/stat.h>
//#include <fcntl.h>

struct switch_core_session {
	unsigned long id;
	char name[80];
	switch_memory_pool *pool;
	switch_channel *channel;
	switch_thread *thread;
	const switch_endpoint_interface *endpoint_interface;
	struct switch_io_event_hooks event_hooks;
	switch_codec *read_codec;
	switch_codec *write_codec;

	switch_buffer *raw_write_buffer;
	switch_frame raw_write_frame;
	switch_frame enc_write_frame;
	unsigned char *raw_write_buf[3200];
	unsigned char *enc_write_buf[3200];

	switch_buffer *raw_read_buffer;
	switch_frame raw_read_frame;
	switch_frame enc_read_frame;
	unsigned char *raw_read_buf[3200];
	unsigned char *enc_read_buf[3200];

	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;

	void *private;
};

struct switch_core_runtime {
	time_t initiated;
	unsigned long session_id;
	apr_pool_t *memory_pool;
	switch_hash *session_table;
#ifdef EMBED_PERL
	PerlInterpreter *my_perl;
#endif
	FILE *console;
};

/* Prototypes */
static int handle_SIGINT(int sig);
static int handle_SIGPIPE(int sig);
static void * SWITCH_THREAD_FUNC switch_core_session_thread(switch_thread *thread, void *obj);
static void switch_core_standard_on_init(switch_core_session *session);
static void switch_core_standard_on_hangup(switch_core_session *session);
static void switch_core_standard_on_ring(switch_core_session *session);
static void switch_core_standard_on_execute(switch_core_session *session);
static void switch_core_standard_on_loopback(switch_core_session *session);
static void switch_core_standard_on_transmit(switch_core_session *session);

/* The main runtime obj we keep this hidden for ourselves */
static struct switch_core_runtime runtime;


static int handle_SIGPIPE(int sig)
{
	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Sig Pipe!\n");
	return 0;
}

/* no ctl-c mofo */
static int handle_SIGINT(int sig)
{
	return 0;
}

SWITCH_DECLARE(FILE *) switch_core_data_channel(switch_text_channel channel)
{
	FILE *handle = stdout;

	switch (channel) {
	case SWITCH_CHANNEL_ID_CONSOLE:
	case SWITCH_CHANNEL_ID_CONSOLE_CLEAN:
		handle = runtime.console;
		break;
	default:
		handle = stdout;
		break;
	}

	return handle;
}

#ifdef EMBED_PERL
/* test frontend to the perl interpreter */
SWITCH_DECLARE(switch_status) switch_core_do_perl(char *txt)
{
	PerlInterpreter *my_perl = runtime.my_perl;
	eval_pv(txt, TRUE);
	return SWITCH_STATUS_SUCCESS;
}
#endif


SWITCH_DECLARE(switch_status) switch_core_session_set_read_codec(switch_core_session *session, switch_codec *codec)
{
	assert(session != NULL);

	session->read_codec = codec;
	return SWITCH_STATUS_SUCCESS;
}


SWITCH_DECLARE(switch_status) switch_core_session_set_write_codec(switch_core_session *session, switch_codec *codec)
{
	assert(session != NULL);

	session->write_codec = codec;
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status) switch_core_codec_init(switch_codec *codec, char *codec_name, int rate, int ms, switch_codec_flag flags, const switch_codec_settings *codec_settings)
{
	const switch_codec_interface *codec_interface;
	const switch_codec_implementation *iptr, *implementation = NULL;

	assert(codec != NULL);
	assert(codec_name != NULL);

	memset(codec, 0, sizeof(*codec));

	if (!(codec_interface = loadable_module_get_codec_interface(codec_name))) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "invalid codec %s!\n", codec_name);
		return SWITCH_STATUS_GENERR;
	}

	for(iptr = codec_interface->implementations; iptr; iptr = iptr->next) {
		if ((!rate || rate == iptr->samples_per_second) && (!ms || ms == (iptr->microseconds_per_frame / 1000))) {
			implementation = iptr;
			break;
		}
	}

	if (implementation) {
		switch_status status;
		codec->codec_interface = codec_interface;
		codec->implementation = implementation;
		codec->flags = flags;
		if ((status = switch_core_new_memory_pool(&codec->memory_pool)) != SWITCH_STATUS_SUCCESS) {
			return status;
		}
		implementation->init(codec, flags, codec_settings);

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_NOTIMPL;

}

SWITCH_DECLARE(switch_status) switch_core_codec_encode(switch_codec *codec,
								 switch_codec *other_codec,
								 void *decoded_data,
								 size_t decoded_data_len,
								 void *encoded_data,
								 size_t *encoded_data_len,
								 unsigned int *flag)
{
	assert(codec != NULL);
	assert(encoded_data != NULL);
	assert(decoded_data != NULL);

	if (!codec->implementation) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec is not initilized!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (!switch_test_flag(codec, SWITCH_CODEC_FLAG_ENCODE)) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec's encoder is not initilized!\n");
		return SWITCH_STATUS_GENERR;
	}

	return codec->implementation->encode(codec, other_codec, decoded_data, decoded_data_len, encoded_data, encoded_data_len, flag);
}

SWITCH_DECLARE(switch_status) switch_core_codec_decode(switch_codec *codec,
								 switch_codec *other_codec,
								 void *encoded_data,
								 size_t encoded_data_len,
								 void *decoded_data,
								 size_t *decoded_data_len,
								 unsigned int *flag)
{
	assert(codec != NULL);
	assert(encoded_data != NULL);
	assert(decoded_data != NULL);

	if (!codec->implementation) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec is not initilized!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (!switch_test_flag(codec, SWITCH_CODEC_FLAG_DECODE)) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec's decoder is not initilized!\n");
		return SWITCH_STATUS_GENERR;
	}

	return codec->implementation->decode(codec, other_codec, encoded_data, encoded_data_len, decoded_data, decoded_data_len, flag);
}

SWITCH_DECLARE(switch_status) switch_core_codec_destroy(switch_codec *codec)
{
	assert(codec != NULL);

	if (!codec->implementation) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec is not initilized!\n");
		return SWITCH_STATUS_GENERR;
	}

	codec->implementation->destroy(codec);
	switch_core_destroy_memory_pool(&codec->memory_pool);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status) switch_core_timer_init(switch_timer *timer, char *timer_name, int interval, int samples)
{
	switch_timer_interface *timer_interface;
	switch_status status;
	memset(timer, 0, sizeof(*timer));
	if (!(timer_interface = loadable_module_get_timer_interface(timer_name))) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "invalid timer %s!\n", timer_name);
		return SWITCH_STATUS_GENERR;
	}

	timer->interval = interval;
	timer->samples = samples;
	timer->samplecount = 0;
	timer->timer_interface = timer_interface;
	if ((status = switch_core_new_memory_pool(&timer->memory_pool)) != SWITCH_STATUS_SUCCESS) {
		return status;
	}

	timer->timer_interface->timer_init(timer);
	return SWITCH_STATUS_SUCCESS;

}

SWITCH_DECLARE(int) switch_core_timer_next(switch_timer *timer)
{
	if (!timer->timer_interface) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Timer is not initilized!\n");
		return SWITCH_STATUS_GENERR;
	}

	if (timer->timer_interface->timer_next(timer) == SWITCH_STATUS_SUCCESS) {
		return timer->samplecount;
	} else {
		return -1;
	}

}


SWITCH_DECLARE(switch_status) switch_core_timer_destroy(switch_timer *timer)
{
	if (!timer->timer_interface) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Timer is not initilized!\n");
		return SWITCH_STATUS_GENERR;
	}

	timer->timer_interface->timer_destroy(timer);
	switch_core_destroy_memory_pool(&timer->memory_pool);
	return SWITCH_STATUS_SUCCESS;
}

static void *switch_core_service_thread(switch_thread *thread, void *obj)
{
	switch_core_thread_session *data = obj;
	switch_core_session *session = data->objs[0];
	switch_channel *channel;
	switch_frame *read_frame;

	assert(session != NULL);
	channel = switch_core_session_get_channel(session);
	assert(channel != NULL);


	while(data->running > 0) {
		switch(switch_core_session_read_frame(session, &read_frame, -1)) {
		case SWITCH_STATUS_SUCCESS:
			break;
		case SWITCH_STATUS_TIMEOUT:
			break;
		default:
			data->running = -1;
			continue;
			break;
		}

		switch_yield(100);
	}

	data->running = 0;
	return NULL;
}

/* Either add a timeout here or make damn sure the thread cannot get hung somehow (my preference) */
SWITCH_DECLARE(void) switch_core_thread_session_end(switch_core_thread_session *thread_session)
{
	switch_core_session *session = thread_session->objs[0];

	switch_core_session_kill_channel(session, SWITCH_SIG_KILL);

	if (thread_session->running > 0) {
		thread_session->running = -1;

		while(thread_session->running) {
			switch_yield(1000);
		}
	}
}

SWITCH_DECLARE(void) switch_core_service_session(switch_core_session *session, switch_core_thread_session *thread_session)
{
	thread_session->running = 1;
	thread_session->objs[0] = session;
	switch_core_session_launch_thread(session, switch_core_service_thread, thread_session);
}

SWITCH_DECLARE(switch_memory_pool *) switch_core_session_get_pool(switch_core_session *session)
{
	return session->pool;
}

/* **ONLY** alloc things with this function that **WILL NOT** outlive
   the session itself or expect an earth shattering KABOOM!*/
SWITCH_DECLARE(void *)switch_core_session_alloc(switch_core_session *session, size_t memory)
{
	void *ptr = NULL;
	assert(session != NULL);
	assert(session->pool != NULL);

	if ((ptr = apr_palloc(session->pool, memory))) {
		memset(ptr, 0, memory);
	}
	return ptr;
}

/* **ONLY** alloc things with these functions that **WILL NOT** need
   to be freed *EVER* ie this is for *PERMENANT* memory allocation */

SWITCH_DECLARE(void *) switch_core_permenant_alloc(size_t memory)
{
	void *ptr = NULL;
	assert(runtime.memory_pool != NULL);

	if ((ptr = apr_palloc(runtime.memory_pool, memory))) {
		memset(ptr, 0, memory);
	}
	return ptr;
}

SWITCH_DECLARE(char *) switch_core_permenant_strdup(char *todup)
{
	char *duped = NULL;

	assert(runtime.memory_pool != NULL);

	if (todup && (duped = apr_palloc(runtime.memory_pool, strlen(todup)+1))) {
		strcpy(duped, todup);
	}
	return duped;
}


SWITCH_DECLARE(char *) switch_core_session_strdup(switch_core_session *session, char *todup)
{
	char *duped = NULL;

	assert(session != NULL);
	assert(session->pool != NULL);

	if (todup && (duped = apr_palloc(session->pool, strlen(todup)+1))) {
		strcpy(duped, todup);
	}
	return duped;
}


SWITCH_DECLARE(char *) switch_core_strdup(switch_memory_pool *pool, char *todup)
{
	char *duped = NULL;

	assert(pool != NULL);
	assert(todup != NULL);

	if (todup && (duped = apr_palloc(pool, strlen(todup)+1))) {
		strcpy(duped, todup);
	}
	return duped;
}

SWITCH_DECLARE(void *) switch_core_session_get_private(switch_core_session *session)
{
	assert(session != NULL);
	return session->private;
}


SWITCH_DECLARE(switch_status) switch_core_session_set_private(switch_core_session *session, void *private)
{
	assert(session != NULL);
	session->private = private;
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status) switch_core_session_outgoing_channel(switch_core_session *session,
											 char *endpoint_name,
											 switch_caller_profile *caller_profile,
											 switch_core_session **new_session)
{
	struct switch_io_event_hook_outgoing_channel *ptr;
	switch_status status = SWITCH_STATUS_FALSE;
	const switch_endpoint_interface *endpoint_interface;

	if (!(endpoint_interface = loadable_module_get_endpoint_interface(endpoint_name))) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Could not locate channel type %s\n", endpoint_name);
		return SWITCH_STATUS_FALSE;
	}

	if (endpoint_interface->io_routines->outgoing_channel) {
		if ((status = endpoint_interface->io_routines->outgoing_channel(session, caller_profile, new_session)) == SWITCH_STATUS_SUCCESS) {
			for (ptr = session->event_hooks.outgoing_channel; ptr ; ptr = ptr->next) {
				if ((status = ptr->outgoing_channel(session, caller_profile, *new_session)) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			}
		}
	}

	return status;
}

SWITCH_DECLARE(switch_status) switch_core_session_answer_channel(switch_core_session *session)
{
	struct switch_io_event_hook_answer_channel *ptr;
	switch_status status = SWITCH_STATUS_FALSE;

	assert(session != NULL);
	if (session->endpoint_interface->io_routines->answer_channel) {
		if ((status = session->endpoint_interface->io_routines->answer_channel(session)) == SWITCH_STATUS_SUCCESS) {
			for (ptr = session->event_hooks.answer_channel; ptr ; ptr = ptr->next) {
				if ((status = ptr->answer_channel(session)) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			}
		}
	} else {
		status = SWITCH_STATUS_SUCCESS;
	}

	return status;
}

SWITCH_DECLARE(switch_status) switch_core_session_read_frame(switch_core_session *session, switch_frame **frame, int timeout)
{
	struct switch_io_event_hook_read_frame *ptr;
	switch_status status = SWITCH_STATUS_FALSE;
	int need_codec = 0, perfect = 0;


	if (session->endpoint_interface->io_routines->read_frame) {
		if ((status = session->endpoint_interface->io_routines->read_frame(session, frame, timeout, SWITCH_IO_FLAG_NOOP)) == SWITCH_STATUS_SUCCESS) {
			for (ptr = session->event_hooks.read_frame; ptr ; ptr = ptr->next) {
				if ((status = ptr->read_frame(session, frame, timeout, SWITCH_IO_FLAG_NOOP)) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			}
		}
	}

	if (status != SWITCH_STATUS_SUCCESS || !(*frame)) {
		return status;
	}

	/* if you think this code is redundant.... too bad! I like to understand what I'm doing */
	if ((session->read_codec && (*frame)->codec && session->read_codec->implementation != (*frame)->codec->implementation)) {
		need_codec = TRUE;
	}

	if (session->read_codec && !(*frame)->codec) {
		need_codec = TRUE;
	}

	if (!session->read_codec && (*frame)->codec) {
		need_codec = TRUE;
	}

	if (status == SWITCH_STATUS_SUCCESS && need_codec) {
		switch_frame *enc_frame, *read_frame = *frame;

		if (read_frame->codec) {
			unsigned int flag = 0;
			session->raw_read_frame.datalen = session->raw_read_frame.buflen;
			status = switch_core_codec_decode(read_frame->codec,
										   session->read_codec,
										   read_frame->data,
										   read_frame->datalen,
										   session->raw_read_frame.data,
										   &session->raw_read_frame.datalen,
										   &flag);

			switch (status) {
			case SWITCH_STATUS_SUCCESS:
				read_frame = &session->raw_read_frame;
				break;
			case SWITCH_STATUS_NOOP:
				status = SWITCH_STATUS_SUCCESS;
				break;
			default:
				switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec %s decoder error!\n", session->read_codec->codec_interface->interface_name);
				return status;
				break;
			}
		}

		if (session->read_codec) {
			if ((*frame)->datalen == session->read_codec->implementation->bytes_per_frame) {
				perfect = TRUE;
			} else {
				if (! session->raw_read_buffer) {
					int bytes = session->read_codec->implementation->bytes_per_frame * 10;
					switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Engaging Read Buffer at %d bytes\n", bytes);
					switch_buffer_create(session->pool, &session->raw_read_buffer, bytes);
				}
				if (!switch_buffer_write(session->raw_read_buffer, read_frame->data, read_frame->datalen)) {
					return SWITCH_STATUS_MEMERR;
				}
			}

			if (switch_buffer_inuse(session->raw_read_buffer) >= session->read_codec->implementation->bytes_per_frame) {
				unsigned int flag = 0;

				if (perfect) {
					enc_frame = *frame;
				} else {
					session->raw_read_frame.datalen = switch_buffer_read(session->raw_read_buffer,
																				  session->raw_read_frame.data,
																				  session->read_codec->implementation->bytes_per_frame);
					enc_frame = &session->raw_read_frame;
				}
				session->enc_read_frame.datalen = session->enc_read_frame.buflen;
				status = switch_core_codec_encode(session->read_codec,
											   (*frame)->codec,
											   session->raw_read_frame.data,
											   session->raw_read_frame.datalen,
											   session->enc_read_frame.data,
											   &session->enc_read_frame.datalen,
											   &flag);


				switch (status) {
				case SWITCH_STATUS_SUCCESS:
					*frame = &session->enc_read_frame;
					break;
				case SWITCH_STATUS_NOOP:
					*frame = &session->raw_read_frame;
					status = SWITCH_STATUS_SUCCESS;
					break;
				default:
					switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec %s encoder error!\n", session->read_codec->codec_interface->interface_name);
					*frame = NULL;
					status = SWITCH_STATUS_GENERR;
					break;
				}
			}
		}
	}

	return status;
}

static switch_status perform_write(switch_core_session *session, switch_frame *frame, int timeout, switch_io_flag flags) {
	struct switch_io_event_hook_write_frame *ptr;
	switch_status status = SWITCH_STATUS_FALSE;

	if (session->endpoint_interface->io_routines->write_frame) {
		if ((status = session->endpoint_interface->io_routines->write_frame(session, frame, timeout, flags)) == SWITCH_STATUS_SUCCESS) {
			for (ptr = session->event_hooks.write_frame; ptr ; ptr = ptr->next) {
				if ((status = ptr->write_frame(session, frame, timeout, flags)) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			}
		}
	}
	return status;
}

SWITCH_DECLARE(switch_status) switch_core_session_write_frame(switch_core_session *session, switch_frame *frame, int timeout)
{

	switch_status status = SWITCH_STATUS_FALSE;
	switch_frame *enc_frame, *write_frame = frame;
	unsigned int flag = 0, need_codec = 0, perfect = 0;
	switch_io_flag io_flag = SWITCH_IO_FLAG_NOOP;


	/* if you think this code is redundant.... too bad! I like to understand what I'm doing */
	if ((session->write_codec && frame->codec && session->write_codec->implementation != frame->codec->implementation)) {
		need_codec = TRUE;
	}

	if (session->write_codec && !frame->codec) {
		need_codec = TRUE;
	}

	if (!session->write_codec && frame->codec) {
		need_codec = TRUE;
	}

	if (need_codec) {
		if (frame->codec) {

			session->raw_write_frame.datalen = session->raw_write_frame.buflen;
			status = switch_core_codec_decode(frame->codec,
										   session->write_codec,
										   frame->data,
										   frame->datalen,
										   session->raw_write_frame.data,
										   &session->raw_write_frame.datalen,
										   &flag);

			switch (status) {
			case SWITCH_STATUS_SUCCESS:
				write_frame = &session->raw_write_frame;
				break;
			case SWITCH_STATUS_NOOP:
				write_frame = frame;
				status = SWITCH_STATUS_SUCCESS;
				break;
			default:
				switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec %s decoder error!\n", frame->codec->codec_interface->interface_name);
				return status;
				break;
			}
		}

		if (session->write_codec) {
			if (write_frame->datalen == session->write_codec->implementation->bytes_per_frame) {
				perfect = TRUE;
			} else {
				if (!session->raw_write_buffer) {
					int bytes = session->write_codec->implementation->bytes_per_frame * 10;
					switch_console_printf(SWITCH_CHANNEL_CONSOLE,
									   "Engaging Write Buffer at %d bytes to accomidate %d->%d\n",
									   bytes,
									   write_frame->datalen,
									   session->write_codec->implementation->bytes_per_frame);
					if ((status = switch_buffer_create(session->pool, &session->raw_write_buffer, bytes)) != SWITCH_STATUS_SUCCESS) {
						switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Write Buffer Failed!\n");
						return status;
					}
				}
				if (!(switch_buffer_write(session->raw_write_buffer, write_frame->data, write_frame->datalen))) {
					return SWITCH_STATUS_MEMERR;
				}
			}

			if (perfect) {
				enc_frame = write_frame;
				session->enc_write_frame.datalen = session->enc_write_frame.buflen;
				status = switch_core_codec_encode(session->write_codec,
											   frame->codec,
											   enc_frame->data,
											   enc_frame->datalen,
											   session->enc_write_frame.data,
											   &session->enc_write_frame.datalen,
											   &flag);


				switch (status) {
				case SWITCH_STATUS_SUCCESS:
					write_frame = &session->enc_write_frame;
					break;
				case SWITCH_STATUS_NOOP:
					write_frame = enc_frame;
					status = SWITCH_STATUS_SUCCESS;
					break;
				default:
					switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec %s encoder error!\n", session->read_codec->codec_interface->interface_name);
					write_frame = NULL;
					return status;
					break;
				}

				status = perform_write(session, write_frame, timeout, io_flag);
				return status;
			} else {
				int used = switch_buffer_inuse(session->raw_write_buffer);
				int bytes = session->write_codec->implementation->bytes_per_frame;
				int frames = (used / bytes);


				if (frames) {
					int x;
					for (x = 0; x < frames; x++) {
						if ((session->raw_write_frame.datalen =
							 switch_buffer_read(session->raw_write_buffer,
											 session->raw_write_frame.data,
											 bytes))) {

							enc_frame = &session->raw_write_frame;
							session->enc_write_frame.datalen = session->enc_write_frame.buflen;
							status = switch_core_codec_encode(session->write_codec,
														   frame->codec,
														   enc_frame->data,
														   enc_frame->datalen,
														   session->enc_write_frame.data,
														   &session->enc_write_frame.datalen,
														   &flag);



							switch (status) {
							case SWITCH_STATUS_SUCCESS:
								write_frame = &session->enc_write_frame;
								break;
							case SWITCH_STATUS_NOOP:
								write_frame = enc_frame;
								status = SWITCH_STATUS_SUCCESS;
								break;
							default:
								switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Codec %s encoder error!\n", session->read_codec->codec_interface->interface_name);
								write_frame = NULL;
								return status;
								break;
							}
							status = perform_write(session, write_frame, timeout, io_flag);
						}
					}
					return status;
				}
			}
		}
	} else {
		status = perform_write(session, frame, timeout, io_flag);
	}
	return status;
}

SWITCH_DECLARE(switch_status) switch_core_session_kill_channel(switch_core_session *session, switch_signal sig)
{
	struct switch_io_event_hook_kill_channel *ptr;
	switch_status status = SWITCH_STATUS_FALSE;

	if (session->endpoint_interface->io_routines->kill_channel) {
		if ((status = session->endpoint_interface->io_routines->kill_channel(session, sig)) == SWITCH_STATUS_SUCCESS) {
			for (ptr = session->event_hooks.kill_channel; ptr ; ptr = ptr->next) {
				if ((status = ptr->kill_channel(session, sig)) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			}
		}
	}

	return status;

}

SWITCH_DECLARE(switch_status) switch_core_session_waitfor_read(switch_core_session *session, int timeout)
{
	struct switch_io_event_hook_waitfor_read *ptr;
	switch_status status = SWITCH_STATUS_FALSE;

	if (session->endpoint_interface->io_routines->waitfor_read) {
		if ((status = session->endpoint_interface->io_routines->waitfor_read(session, timeout)) == SWITCH_STATUS_SUCCESS) {
			for (ptr = session->event_hooks.waitfor_read; ptr ; ptr = ptr->next) {
				if ((status = ptr->waitfor_read(session, timeout)) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			}
		}
	}

	return status;

}

SWITCH_DECLARE(switch_status) switch_core_session_waitfor_write(switch_core_session *session, int timeout)
{
	struct switch_io_event_hook_waitfor_write *ptr;
	switch_status status = SWITCH_STATUS_FALSE;

	if (session->endpoint_interface->io_routines->waitfor_write) {
		if ((status = session->endpoint_interface->io_routines->waitfor_write(session, timeout)) == SWITCH_STATUS_SUCCESS) {
			for (ptr = session->event_hooks.waitfor_write; ptr ; ptr = ptr->next) {
				if ((status = ptr->waitfor_write(session, timeout)) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			}
		}
	}

	return status;
}


SWITCH_DECLARE(switch_status) switch_core_session_send_dtmf(switch_core_session *session, char *dtmf) 
{
	struct switch_io_event_hook_send_dtmf *ptr;
	switch_status status = SWITCH_STATUS_FALSE;

	if (session->endpoint_interface->io_routines->send_dtmf) {
		if ((status = session->endpoint_interface->io_routines->send_dtmf(session, dtmf)) == SWITCH_STATUS_SUCCESS) {
			for (ptr = session->event_hooks.send_dtmf; ptr ; ptr = ptr->next) {
				if ((status = ptr->send_dtmf(session, dtmf)) != SWITCH_STATUS_SUCCESS) {
					break;
				}
			}
		}
	}

	return status;
}

SWITCH_DECLARE(switch_status) switch_core_session_add_event_hook_outgoing(switch_core_session *session, switch_outgoing_channel_hook outgoing_channel)
{
	switch_io_event_hook_outgoing_channel *hook, *ptr;

	assert(outgoing_channel != NULL);
	if ((hook = switch_core_session_alloc(session, sizeof(*hook)))) {
		hook->outgoing_channel = outgoing_channel;
		if (!session->event_hooks.outgoing_channel) {
			session->event_hooks.outgoing_channel = hook;
		} else {
			for(ptr = session->event_hooks.outgoing_channel ; ptr && ptr->next; ptr = ptr->next);
			ptr->next = hook;

		}

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;
}

SWITCH_DECLARE(switch_status) switch_core_session_add_event_hook_answer_channel(switch_core_session *session, switch_answer_channel_hook answer_channel)
{
	switch_io_event_hook_answer_channel *hook, *ptr;

	assert(answer_channel != NULL);
	if ((hook = switch_core_session_alloc(session, sizeof(*hook)))) {
		hook->answer_channel = answer_channel;
		if (!session->event_hooks.answer_channel) {
			session->event_hooks.answer_channel = hook;
		} else {
			for(ptr = session->event_hooks.answer_channel ; ptr && ptr->next; ptr = ptr->next);
			ptr->next = hook;

		}

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;

}

SWITCH_DECLARE(switch_status) switch_core_session_add_event_hook_read_frame(switch_core_session *session, switch_read_frame_hook read_frame)
{
	switch_io_event_hook_read_frame *hook, *ptr;

	assert(read_frame != NULL);
	if ((hook = switch_core_session_alloc(session, sizeof(*hook)))) {
		hook->read_frame = read_frame;
		if (!session->event_hooks.read_frame) {
			session->event_hooks.read_frame = hook;
		} else {
			for(ptr = session->event_hooks.read_frame ; ptr && ptr->next; ptr = ptr->next);
			ptr->next = hook;

		}

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;

}

SWITCH_DECLARE(switch_status) switch_core_session_add_event_hook_write_frame(switch_core_session *session, switch_write_frame_hook write_frame)
{
	switch_io_event_hook_write_frame *hook, *ptr;

	assert(write_frame != NULL);
	if ((hook = switch_core_session_alloc(session, sizeof(*hook)))) {
		hook->write_frame = write_frame;
		if (!session->event_hooks.write_frame) {
			session->event_hooks.write_frame = hook;
		} else {
			for(ptr = session->event_hooks.write_frame ; ptr && ptr->next; ptr = ptr->next);
			ptr->next = hook;

		}

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;

}

SWITCH_DECLARE(switch_status) switch_core_session_add_event_hook_kill_channel(switch_core_session *session, switch_kill_channel_hook kill_channel)
{
	switch_io_event_hook_kill_channel *hook, *ptr;

	assert(kill_channel != NULL);
	if ((hook = switch_core_session_alloc(session, sizeof(*hook)))) {
		hook->kill_channel = kill_channel;
		if (!session->event_hooks.kill_channel) {
			session->event_hooks.kill_channel = hook;
		} else {
			for(ptr = session->event_hooks.kill_channel ; ptr && ptr->next; ptr = ptr->next);
			ptr->next = hook;

		}

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;

}

SWITCH_DECLARE(switch_status) switch_core_session_add_event_hook_waitfor_read(switch_core_session *session, switch_waitfor_read_hook waitfor_read)
{
	switch_io_event_hook_waitfor_read *hook, *ptr;

	assert(waitfor_read != NULL);
	if ((hook = switch_core_session_alloc(session, sizeof(*hook)))) {
		hook->waitfor_read = waitfor_read;
		if (!session->event_hooks.waitfor_read) {
			session->event_hooks.waitfor_read = hook;
		} else {
			for(ptr = session->event_hooks.waitfor_read ; ptr && ptr->next; ptr = ptr->next);
			ptr->next = hook;

		}

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;

}

SWITCH_DECLARE(switch_status) switch_core_session_add_event_hook_waitfor_write(switch_core_session *session, switch_waitfor_write_hook waitfor_write)
{
	switch_io_event_hook_waitfor_write *hook, *ptr;

	assert(waitfor_write != NULL);
	if ((hook = switch_core_session_alloc(session, sizeof(*hook)))) {
		hook->waitfor_write = waitfor_write;
		if (!session->event_hooks.waitfor_write) {
			session->event_hooks.waitfor_write = hook;
		} else {
			for(ptr = session->event_hooks.waitfor_write ; ptr && ptr->next; ptr = ptr->next);
			ptr->next = hook;

		}

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;

}


SWITCH_DECLARE(switch_status) switch_core_session_add_event_hook_send_dtmf(switch_core_session *session, switch_send_dtmf_hook send_dtmf)
{
	switch_io_event_hook_send_dtmf *hook, *ptr;

	assert(send_dtmf != NULL);
	if ((hook = switch_core_session_alloc(session, sizeof(*hook)))) {
		hook->send_dtmf = send_dtmf;
		if (!session->event_hooks.send_dtmf) {
			session->event_hooks.send_dtmf = hook;
		} else {
			for(ptr = session->event_hooks.send_dtmf ; ptr && ptr->next; ptr = ptr->next);
			ptr->next = hook;

		}

		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_MEMERR;

}


SWITCH_DECLARE(switch_status) switch_core_new_memory_pool(switch_memory_pool **pool)
{
	assert(runtime.memory_pool != NULL);

	if ((apr_pool_create(pool, runtime.memory_pool)) != APR_SUCCESS) {
		*pool = NULL;
		return SWITCH_STATUS_MEMERR;
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status) switch_core_destroy_memory_pool(switch_memory_pool **pool)
{
	apr_pool_destroy(*pool);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_channel *) switch_core_session_get_channel(switch_core_session *session)
{
	return session->channel;
}

static void switch_core_standard_on_init(switch_core_session *session)
{
	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Standard INIT\n");
}

static void switch_core_standard_on_hangup(switch_core_session *session)
{
	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Standard HANGUP\n");
}

static void switch_core_standard_on_ring(switch_core_session *session)
{
	switch_dialplan_interface *dialplan_interface;
	switch_caller_profile *caller_profile;
	switch_caller_extension *extension;

	if (!(caller_profile = switch_channel_get_caller_profile(session->channel))) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Can't get profile!\n");
		switch_channel_set_state(session->channel, CS_HANGUP);
	} else {
		if (!(dialplan_interface = loadable_module_get_dialplan_interface(caller_profile->dialplan))) {
			switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Can't get dialplan %s!\n", caller_profile->dialplan);
			switch_channel_set_state(session->channel, CS_HANGUP);
		} else {
			if ((extension = dialplan_interface->hunt_function(session))) {
				switch_channel_set_caller_extension(session->channel, extension);
			}
		}
	}

	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Standard RING\n");
}

static void switch_core_standard_on_execute(switch_core_session *session)
{
	switch_caller_extension *extension;
	const switch_application_interface *application_interface;


	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Standard EXECUTE\n");
	if (!(extension = switch_channel_get_caller_extension(session->channel))) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "No Extension!\n");
		switch_channel_set_state(session->channel, CS_HANGUP);
		return;
	}

	while (switch_channel_get_state(session->channel) == CS_EXECUTE && extension->current_application) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Execute %s(%s)\n", extension->current_application->application_name,
						   extension->current_application->application_data);
		if (!(application_interface = loadable_module_get_application_interface(extension->current_application->application_name))) {
			switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Invalid Application %s\n", extension->current_application->application_name);
			switch_channel_set_state(session->channel, CS_HANGUP);
			return;
		}

		if (!application_interface->application_function) {
			switch_console_printf(SWITCH_CHANNEL_CONSOLE, "No Function for %s\n", extension->current_application->application_name);
			switch_channel_set_state(session->channel, CS_HANGUP);
			return;
		}

		application_interface->application_function(session, extension->current_application->application_data);
		extension->current_application = extension->current_application->next;
	}

	switch_channel_set_state(session->channel, CS_HANGUP);
}

static void switch_core_standard_on_loopback(switch_core_session *session)
{
	switch_channel_state state;
	switch_frame *frame;

	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Standard LOOPBACK\n");

	while ((state = switch_channel_get_state(session->channel)) == CS_LOOPBACK) {
		if (switch_core_session_read_frame(session, &frame, -1) == SWITCH_STATUS_SUCCESS) {
			switch_core_session_write_frame(session, frame, -1);
		}
	}
}

static void switch_core_standard_on_transmit(switch_core_session *session)
{
	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Standard TRANSMIT\n");
}


SWITCH_DECLARE(void) pbx_core_session_signal_state_change(switch_core_session *session)
{
	switch_thread_cond_signal(session->cond);
}

SWITCH_DECLARE(void) switch_core_session_run(switch_core_session *session)
{
	switch_channel_state state = CS_NEW, laststate = CS_HANGUP, midstate = CS_DONE;
	const switch_endpoint_interface *endpoint_interface;
	const switch_event_handler_table *driver_event_handlers = NULL;
	const switch_event_handler_table *application_event_handlers = NULL;

	/*
	   Life of the channel. you have channel and pool in your session
	   everywhere you go you use the session to malloc with
	   switch_core_session_alloc(session, <size>)

	   The enpoint module gets the first crack at implementing the state
	   if it wants to, it can cancel the default behaviour by returning SWITCH_STATUS_FALSE

	   Next comes the channel's event handler table that can be set by an application
	   which also can veto the next behaviour in line by returning SWITCH_STATUS_FALSE

	   Finally the default state behaviour is called.


	*/
	assert(session != NULL);
	application_event_handlers = switch_channel_get_event_handlers(session->channel);

	endpoint_interface = session->endpoint_interface;
	assert(endpoint_interface != NULL);

	driver_event_handlers = endpoint_interface->event_handlers;
	assert(driver_event_handlers != NULL);

	switch_mutex_lock(session->mutex);

	while ((state = switch_channel_get_state(session->channel)) != CS_DONE) {
		if (state != laststate) {

			midstate = state;

			switch ( state ) {
			case CS_NEW: /* Just created, Waiting for first instructions */
				switch_console_printf(SWITCH_CHANNEL_CONSOLE, "State NEW\n");
				break;
			case CS_DONE:
				continue;
				break;
			case CS_HANGUP: /* Deactivate and end the thread */
				switch_console_printf(SWITCH_CHANNEL_CONSOLE, "State HANGUP\n");
				if (!driver_event_handlers->on_hangup ||
					(driver_event_handlers->on_hangup &&
					 driver_event_handlers->on_hangup(session) == SWITCH_STATUS_SUCCESS && 
					 midstate == switch_channel_get_state(session->channel))) {
					if (!application_event_handlers || !application_event_handlers->on_hangup ||
						(application_event_handlers->on_hangup &&
						 application_event_handlers->on_hangup(session) == SWITCH_STATUS_SUCCESS && 
						 midstate == switch_channel_get_state(session->channel))) {
						switch_core_standard_on_hangup(session);
					}
				}
				switch_channel_set_state(session->channel, CS_DONE);
				break;
			case CS_INIT: /* Basic setup tasks */
				switch_console_printf(SWITCH_CHANNEL_CONSOLE, "State INIT\n");
				if (!driver_event_handlers->on_init ||
					(driver_event_handlers->on_init &&
					 driver_event_handlers->on_init(session) == SWITCH_STATUS_SUCCESS && 
					 midstate == switch_channel_get_state(session->channel))) {
					if (!application_event_handlers || !application_event_handlers->on_init ||
						(application_event_handlers->on_init &&
						 application_event_handlers->on_init(session) == SWITCH_STATUS_SUCCESS && 
						 midstate == switch_channel_get_state(session->channel))) {
						switch_core_standard_on_init(session);
					}
				}
				break;
			case CS_RING: /* Look for a dialplan and find something to do */
				switch_console_printf(SWITCH_CHANNEL_CONSOLE, "State RING\n");
				if (!driver_event_handlers->on_ring ||
					(driver_event_handlers->on_ring &&
					 driver_event_handlers->on_ring(session) == SWITCH_STATUS_SUCCESS && 
					 midstate == switch_channel_get_state(session->channel))) {
					if (!application_event_handlers || !application_event_handlers->on_ring ||
						(application_event_handlers->on_ring &&
						 application_event_handlers->on_ring(session) == SWITCH_STATUS_SUCCESS && 
						 midstate == switch_channel_get_state(session->channel))) {
						switch_core_standard_on_ring(session);
					}
				}
				break;
			case CS_EXECUTE: /* Execute an Operation*/
				switch_console_printf(SWITCH_CHANNEL_CONSOLE, "State EXECUTE\n");
				if (!driver_event_handlers->on_execute ||
					(driver_event_handlers->on_execute &&
					 driver_event_handlers->on_execute(session) == SWITCH_STATUS_SUCCESS && 
					 midstate == switch_channel_get_state(session->channel))) {
					if (!application_event_handlers || !application_event_handlers->on_execute ||
						(application_event_handlers->on_execute &&
						 application_event_handlers->on_execute(session) == SWITCH_STATUS_SUCCESS && 
						 midstate == switch_channel_get_state(session->channel))) {
						switch_core_standard_on_execute(session);
					}
				}
				break;
			case CS_LOOPBACK: /* loop all data back to source */
				switch_console_printf(SWITCH_CHANNEL_CONSOLE, "State LOOPBACK\n");
				if (!driver_event_handlers->on_loopback ||
					(driver_event_handlers->on_loopback &&
					 driver_event_handlers->on_loopback(session) == SWITCH_STATUS_SUCCESS && 
					 midstate == switch_channel_get_state(session->channel))) {
					if (!application_event_handlers || !application_event_handlers->on_loopback ||
						(application_event_handlers->on_loopback &&
						 application_event_handlers->on_loopback(session) == SWITCH_STATUS_SUCCESS && 
						 midstate == switch_channel_get_state(session->channel))) {
						switch_core_standard_on_loopback(session);
					}
				}
				break;
			case CS_TRANSMIT: /* send/recieve data to/from another channel */
				switch_console_printf(SWITCH_CHANNEL_CONSOLE, "State TRANSMIT\n");
				if (!driver_event_handlers->on_transmit ||
					(driver_event_handlers->on_transmit &&
					 driver_event_handlers->on_transmit(session) == SWITCH_STATUS_SUCCESS && 
					 midstate == switch_channel_get_state(session->channel))) {
					if (!application_event_handlers || !application_event_handlers->on_transmit ||
						(application_event_handlers->on_transmit &&
						 application_event_handlers->on_transmit(session) == SWITCH_STATUS_SUCCESS && 
						 midstate == switch_channel_get_state(session->channel))) {
						switch_core_standard_on_transmit(session);
					}
				}
				break;
			}

			laststate = midstate;
		}

		if (state < CS_DONE && midstate == switch_channel_get_state(session->channel)) {
			switch_thread_cond_wait(session->cond, session->mutex);
		}
	}
}

SWITCH_DECLARE(void) switch_core_session_destroy(switch_core_session **session)
{
	switch_memory_pool *pool;

	pool = (*session)->pool;
	*session = NULL;
	apr_pool_destroy(pool);
	pool = NULL;

}

SWITCH_DECLARE(switch_status) switch_core_hash_init(switch_hash **hash, switch_memory_pool *pool)
{
	if ((*hash = apr_hash_make(pool))) {
		return SWITCH_STATUS_SUCCESS;
	}
	return SWITCH_STATUS_GENERR;
}

SWITCH_DECLARE(switch_status) switch_core_hash_destroy(switch_hash *hash)
{
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status) switch_core_hash_insert(switch_hash *hash, char *key, void *data)
{
	apr_hash_set(hash, key, APR_HASH_KEY_STRING, data);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status) switch_core_hash_delete(switch_hash *hash, char *key)
{
	apr_hash_set(hash, key, APR_HASH_KEY_STRING, NULL);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(void *) switch_core_hash_find(switch_hash *hash, char *key)
{
	return apr_hash_get(hash, key, APR_HASH_KEY_STRING);
}

/* This function abstracts the thread creation for modules by allowing you to pass a function ptr and
 a void object and trust that that the function will be run in a thread with arg  This lets
 you request and activate a thread without giving up any knowledge about what is in the thread
 neither the core nor the calling module know anything about each other.

 This thread is expected to never exit until the application exits so the func is responsible
 to make sure that is the case.

 The typical use for this is so switch_loadable_module.c can start up a thread for each module
 passing the table of module methods as a session obj into the core without actually allowing
 the core to have any clue and keeping switch_loadable_module.c from needing any thread code.

*/

SWITCH_DECLARE(void) switch_core_launch_module_thread(switch_thread_start_t func, void *obj)
{
	switch_thread *thread;
	switch_threadattr_t *thd_attr;;
	switch_threadattr_create(&thd_attr, runtime.memory_pool);
	switch_threadattr_detach_set(thd_attr, 1);

	switch_thread_create(&thread,
					  thd_attr,
					  func,
					  obj,
					  runtime.memory_pool
					  );

}

static void * SWITCH_THREAD_FUNC switch_core_session_thread(switch_thread *thread, void *obj)
{
	switch_core_session *session = obj;


	session->thread = thread;

	session->id = runtime.session_id++;
	if(runtime.session_id >= sizeof(unsigned long))
		runtime.session_id = 1;

	snprintf(session->name, sizeof(session->name), "%ld", session->id);

	switch_core_hash_insert(runtime.session_table, session->name, session);
	switch_core_session_run(session);
	switch_core_session_destroy(&session);


	return NULL;
}


SWITCH_DECLARE(void) switch_core_session_thread_launch(switch_core_session *session)
{
	switch_thread *thread;
	switch_threadattr_t *thd_attr;;
	switch_threadattr_create(&thd_attr, session->pool);
	switch_threadattr_detach_set(thd_attr, 1);

	if (switch_thread_create(&thread,
						  thd_attr,
						  switch_core_session_thread,
						  session,
						  session->pool
						  ) != APR_SUCCESS) {
		switch_core_session_destroy(&session);
	}

}

SWITCH_DECLARE(void) switch_core_session_launch_thread(switch_core_session *session, switch_thread_start_t func, void *obj)
{
	switch_thread *thread;
	switch_threadattr_t *thd_attr;;
	switch_threadattr_create(&thd_attr, session->pool);
	switch_threadattr_detach_set(thd_attr, 1);

	switch_thread_create(&thread,
					  thd_attr,
					  func,
					  obj,
					  session->pool
					  );

}


SWITCH_DECLARE(void *) switch_core_alloc(switch_memory_pool *pool, size_t memory)
{
	void *ptr = NULL;
	assert(pool != NULL);

	if ((ptr = apr_palloc(pool, memory))) {
		memset(ptr, 0, memory);
	}
	return ptr;
}

SWITCH_DECLARE(switch_core_session *) switch_core_session_request(const switch_endpoint_interface *endpoint_interface, switch_memory_pool *pool)
{
	switch_memory_pool *usepool;
	switch_core_session *session;

	assert(endpoint_interface != NULL);

	if (pool) {
		usepool = pool;
	} else if (switch_core_new_memory_pool(&usepool) != SWITCH_STATUS_SUCCESS) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Could not allocate memory pool\n");
		return NULL;
	}

	if (!(session = switch_core_alloc(usepool, sizeof(switch_core_session)))) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Could not allocate session\n");
		apr_pool_destroy(usepool);
		return NULL;
	}

	if (switch_channel_alloc(&session->channel, usepool) != SWITCH_STATUS_SUCCESS) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Could not allocate channel structure\n");
		apr_pool_destroy(usepool);
		return NULL;
	}

	switch_channel_init(session->channel, session, CS_NEW, CF_SEND_AUDIO | CF_RECV_AUDIO);

	/* The session *IS* the pool you may not alter it because you have no idea how
	   its all private it will be passed to the thread run function */

	session->pool = usepool;
	session->endpoint_interface = endpoint_interface;

	session->raw_write_frame.data = session->raw_write_buf;
	session->raw_write_frame.buflen = sizeof(session->raw_write_buf);
	session->raw_read_frame.data = session->raw_read_buf;
	session->raw_read_frame.buflen = sizeof(session->raw_read_buf);
	

	session->enc_write_frame.data = session->enc_write_buf;
	session->enc_write_frame.buflen = sizeof(session->enc_write_buf);
	session->enc_read_frame.data = session->enc_read_buf;
	session->enc_read_frame.buflen = sizeof(session->enc_read_buf);

	switch_mutex_init(&session->mutex, SWITCH_MUTEX_NESTED ,session->pool);
	switch_thread_cond_create(&session->cond, session->pool);
	
	return session;
}

SWITCH_DECLARE(switch_core_session *) switch_core_session_request_by_name(char *endpoint_name, switch_memory_pool *pool)
{
	const switch_endpoint_interface *endpoint_interface;

	if (!(endpoint_interface = loadable_module_get_endpoint_interface(endpoint_name))) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Could not locate channel type %s\n", endpoint_name);
		return NULL;
	}

	return switch_core_session_request(endpoint_interface, pool);
}

SWITCH_DECLARE(switch_status) switch_core_init(void)
{
#ifdef EMBED_PERL
	PerlInterpreter *my_perl;
#endif

	runtime.console = stdout;

	/* INIT APR and Create the pool context */
	if (apr_initialize() != APR_SUCCESS) {
		apr_terminate();
		return SWITCH_STATUS_MEMERR;
	}

	if (apr_pool_create(&runtime.memory_pool, NULL) != APR_SUCCESS) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Could not allocate memory pool\n");
		switch_core_destroy();
		return SWITCH_STATUS_MEMERR;
	}
	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Allocated memory pool.\n");
	switch_event_init(runtime.memory_pool);
	
#ifdef EMBED_PERL
	if (! (my_perl = perl_alloc())) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Could not allocate perl intrepreter\n");
		switch_core_destroy();
		return SWITCH_STATUS_MEMERR;
	}
	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Allocated perl intrepreter.\n");

	PERL_SET_CONTEXT(my_perl);
	perl_construct(my_perl);
	perl_parse(my_perl, xs_init, 3, embedding, NULL);
	perl_run(my_perl);
	runtime.my_perl = my_perl;
#endif

	runtime.session_id = 1;

	switch_core_hash_init(&runtime.session_table, runtime.memory_pool);

	/* set signal handlers and startup time */
	(void) signal(SIGINT,(void *) handle_SIGINT);
#ifdef SIGPIPE
	(void) signal(SIGPIPE,(void *) handle_SIGPIPE);
#endif
	time(&runtime.initiated);

	return SWITCH_STATUS_SUCCESS;
}


SWITCH_DECLARE(switch_status) switch_core_destroy(void)
{

#ifdef EMBED_PERL
	if (runtime.my_perl) {
		perl_destruct(runtime.my_perl);
		perl_free(runtime.my_perl);
		runtime.my_perl = NULL;
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Unallocated perl interpreter.\n");
	}
#endif

	switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Closing Event Engine.\n");
	switch_event_shutdown();

	if (runtime.memory_pool) {
		switch_console_printf(SWITCH_CHANNEL_CONSOLE, "Unallocating memory pool.\n");
		apr_pool_destroy(runtime.memory_pool);
		apr_terminate();
	}

	return SWITCH_STATUS_SUCCESS;
}

