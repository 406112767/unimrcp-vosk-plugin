/*
 * Copyright 2008-2015 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */

#include "mrcp_recog_engine.h"
#include "mpf_activity_detector.h"
#include "apt_consumer_task.h"
#include "apt_log.h"
#include "vosk_api.h"
#include <apr_xml.h>
#include "string.h"
#include <regex.h>
#include <stdlib.h>


#define RECOG_ENGINE_TASK_NAME "Vosk Recog Engine"

typedef struct vosk_recog_engine_t vosk_recog_engine_t;
typedef struct vosk_recog_channel_t vosk_recog_channel_t;
typedef struct vosk_recog_msg_t vosk_recog_msg_t;

/** Declaration of recognizer engine methods */
static apt_bool_t vosk_recog_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t vosk_recog_engine_open(mrcp_engine_t *engine);
static apt_bool_t vosk_recog_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* vosk_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	vosk_recog_engine_destroy,
	vosk_recog_engine_open,
	vosk_recog_engine_close,
	vosk_recog_engine_channel_create
};

/** Declaration of recognizer channel methods */
static apt_bool_t vosk_recog_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t vosk_recog_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t vosk_recog_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t vosk_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	vosk_recog_channel_destroy,
	vosk_recog_channel_open,
	vosk_recog_channel_close,
	vosk_recog_channel_request_process
};

/** Declaration of recognizer audio stream methods */
static apt_bool_t vosk_recog_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t vosk_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t vosk_recog_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t vosk_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	vosk_recog_stream_destroy,
	NULL,
	NULL,
	NULL,
	vosk_recog_stream_open,
	vosk_recog_stream_close,
	vosk_recog_stream_write,
	NULL
};

/** Declaration of kaldi recognizer engine */
struct vosk_recog_engine_t {
	apt_consumer_task_t    *task;
	VoskModel              *model;
};

/** Declaration of kaldi recognizer channel */
struct vosk_recog_channel_t {
	/** Back pointer to engine */
	vosk_recog_engine_t     *kaldi_engine;
	/** Engine channel base */
	mrcp_engine_channel_t   *channel;

	/** Active (in-progress) recognition request */
	mrcp_message_t          *recog_request;
	/** Pending stop response */
	mrcp_message_t          *stop_response;
	/** Indicates whether input timers are started */
	apt_bool_t               timers_started;
	/** Voice activity detector */
	mpf_activity_detector_t *detector;
	/** File to write utterance to */
	FILE                    *audio_out;
	/** grammar */
	apr_xml_doc 			*grammar;
	/** Actual recognizer **/
	VoskRecognizer          *recognizer;
};

typedef enum {
	vosk_recog_MSG_OPEN_CHANNEL,
	vosk_recog_MSG_CLOSE_CHANNEL,
	vosk_recog_MSG_REQUEST_PROCESS
} vosk_recog_msg_type_e;

/** Declaration of kaldi recognizer task message */
struct vosk_recog_msg_t {
	vosk_recog_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};

static apt_bool_t vosk_recog_msg_signal(vosk_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t vosk_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="RECOG-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN,"RECOG-PLUGIN")

/** Use custom log source mark */
#define RECOG_LOG_MARK   APT_LOG_MARK_DECLARE(RECOG_PLUGIN)

/** Create kaldi recognizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	vosk_recog_engine_t *kaldi_engine = (vosk_recog_engine_t*)apr_palloc(pool,sizeof(vosk_recog_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(vosk_recog_msg_t),pool);
	kaldi_engine->task = apt_consumer_task_create(kaldi_engine,msg_pool,pool);
	if(!kaldi_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(kaldi_engine->task);
	apt_task_name_set(task,RECOG_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = vosk_recog_msg_process;
	}

	kaldi_engine->model = vosk_model_new("/opt/kaldi/model");

	/* create engine base */
	return mrcp_engine_create(
				MRCP_RECOGNIZER_RESOURCE,  /* MRCP resource identifier */
				kaldi_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy recognizer engine */
static apt_bool_t vosk_recog_engine_destroy(mrcp_engine_t *engine)
{
	vosk_recog_engine_t *kaldi_engine = (vosk_recog_engine_t*)engine->obj;
	if(kaldi_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(kaldi_engine->task);
		apt_task_destroy(task);
		kaldi_engine->task = NULL;
	}
	if (kaldi_engine->model) {
		vosk_model_free(kaldi_engine->model);
		kaldi_engine->model = NULL;
	}
	return TRUE;
}

/** Open recognizer engine */
static apt_bool_t vosk_recog_engine_open(mrcp_engine_t *engine)
{
	vosk_recog_engine_t *kaldi_engine = (vosk_recog_engine_t*)engine->obj;
	if(kaldi_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(kaldi_engine->task);
		apt_task_start(task);
	}
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close recognizer engine */
static apt_bool_t vosk_recog_engine_close(mrcp_engine_t *engine)
{
	vosk_recog_engine_t *kaldi_engine = (vosk_recog_engine_t*)engine->obj;
	if(kaldi_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(kaldi_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

static mrcp_engine_channel_t* vosk_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create kaldi recog channel */
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)apr_palloc(pool,sizeof(vosk_recog_channel_t));
	recog_channel->kaldi_engine = (vosk_recog_engine_t*)engine->obj;
    recog_channel->recognizer = NULL;
	recog_channel->recog_request = NULL;
	recog_channel->stop_response = NULL;
	recog_channel->grammar = NULL;
	recog_channel->detector = mpf_activity_detector_create(pool);
	recog_channel->audio_out = NULL;

	capabilities = mpf_sink_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			recog_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	recog_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			recog_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	return recog_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t vosk_recog_channel_destroy(mrcp_engine_channel_t *channel)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"channel destory %s", channel->id.buf);
	/* nothing to destrtoy */
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t vosk_recog_channel_open(mrcp_engine_channel_t *channel)
{
	return vosk_recog_msg_signal(vosk_recog_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t vosk_recog_channel_close(mrcp_engine_channel_t *channel)
{
	return vosk_recog_msg_signal(vosk_recog_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t vosk_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	return vosk_recog_msg_signal(vosk_recog_MSG_REQUEST_PROCESS,channel,request);
}

/** Process RECOGNIZE request */
static apt_bool_t vosk_recog_channel_recognize(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process RECOGNIZE request */
	mrcp_recog_header_t *recog_header;
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_sink_stream_codec_get(channel);

	if(!descriptor) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	recog_channel->timers_started = TRUE;

	/* get recognizer header */
	recog_header = (mrcp_recog_header_t*)mrcp_resource_header_get(request);
	if(recog_header) {
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_START_INPUT_TIMERS) == TRUE) {
			recog_channel->timers_started = recog_header->start_input_timers;
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_NO_INPUT_TIMEOUT) == TRUE) {
			mpf_activity_detector_noinput_timeout_set(recog_channel->detector,recog_header->no_input_timeout);
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT) == TRUE) {
			mpf_activity_detector_silence_timeout_set(recog_channel->detector,recog_header->speech_complete_timeout);
		}
	}

	if(!recog_channel->audio_out) {
		const apt_dir_layout_t *dir_layout = channel->engine->dir_layout;
		char *file_name = apr_psprintf(channel->pool,"utter-%dkHz-%s.pcm",
							descriptor->sampling_rate/1000,
							request->channel_id.session_id.buf);
		char *file_path = apt_vardir_filepath_get(dir_layout,file_name,channel->pool);
		if(file_path) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Open Utterance Output File [%s] for Writing",file_path);
			recog_channel->audio_out = fopen(file_path,"wb");
			if(!recog_channel->audio_out) {
				apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Open Utterance Output File [%s] for Writing",file_path);
			}
		}
	}
	if(!recog_channel->recognizer) {
		vosk_recog_engine_t *kaldi_engine = recog_channel->kaldi_engine;
		recog_channel->recognizer = vosk_recognizer_new(kaldi_engine->model, 8000.0f);
		vosk_recognizer_set_max_alternatives(recog_channel->recognizer, 5);
		vosk_recognizer_set_nlsml(recog_channel->recognizer, 1);
	}

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	recog_channel->recog_request = request;
	return TRUE;
}

/** Process STOP request */
static apt_bool_t vosk_recog_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process STOP request */
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)channel->method_obj;
	/* store STOP request, make sure there is no more activity and only then send the response */
	recog_channel->stop_response = response;
	return TRUE;
}

/** Process START-INPUT-TIMERS request */
static apt_bool_t vosk_recog_channel_timers_start(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)channel->method_obj;
	recog_channel->timers_started = TRUE;
	return mrcp_engine_channel_message_send(channel,response);
}

static apt_bool_t check_grammar(apr_xml_doc *doc)
{
	const apr_xml_elem *elem;
	const apr_xml_elem *child;
	const apr_xml_elem *root;
	const apr_xml_attr *attr;

	if(!doc) {
		apt_log(APT_LOG_MARK,APT_PRIO_ERROR,"doc is null");
		return FALSE;
	}

	root = doc->root;

	/* Match document name */
	if(!root || strcasecmp(root->name,"grammar") != 0) {
		apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Document <%s>",root ? root->name : "null");
		return FALSE;
	}

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Document <%s>",root ? root->name : "null");
	return TRUE;
}

/** Dispatch MRCP request */
static apt_bool_t vosk_recog_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case RECOGNIZER_SET_PARAMS:
			break;
		case RECOGNIZER_GET_PARAMS:
			break;
		case RECOGNIZER_DEFINE_GRAMMAR: {
				const char * pattern = "<grammar.*</grammar>";
				regmatch_t pmatch[1];
				char res[4096] = {'\0'};
				const size_t nmatch = 1;
				regex_t reg;
				int cflags = REG_EXTENDED;
				int status;

				apr_pool_t *pool = request->pool;
				apr_xml_parser *parser;
				char errbuf[4096];
				apr_status_t rv;

				vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)channel->method_obj;

				// 只获取有效的音频
				status = regcomp(&reg, pattern, cflags);
				if(status != REG_NOERROR){
					apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Error regcomp");
					return FALSE;
				}
				status = regexec(&reg, request->body.buf, nmatch, pmatch, 0);
				if(status == REG_NOERROR){
					// grammar 超过字符长度限制
					if ((pmatch[0].rm_eo-pmatch[0].rm_so)>=4096) {
						apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Grammar length over size");
						return FALSE;
					}
					for (size_t i = pmatch[0].rm_so, j = 0; i < pmatch[0].rm_eo; i++,j++){
						res[j] = request->body.buf[i];
					}
					apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "Match: %s", res);
				} else {
					apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Error No Match: %s", request->body.buf);
					return FALSE;
				}
				regfree(&reg);

				parser = apr_xml_parser_create(pool);
				if (!parser) {
					apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Error create xml paser");
					return FALSE;
				}
				rv = apr_xml_parser_feed(parser, request->body.buf, strlen(request->body.buf));
				if (rv != APR_SUCCESS) {
					apr_xml_parser_geterror(parser, errbuf, sizeof(errbuf));
					apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Error parsing grammar XML: %d %pm: %s",
						rv, &rv, errbuf);
					return FALSE;
				}
				rv = apr_xml_parser_done(parser, &(recog_channel->grammar));
				if (rv != APR_SUCCESS) {
					apr_xml_parser_geterror(parser, errbuf, sizeof(errbuf));
					apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Error parsing grammar XML done: %d %pm: %s",
						rv, &rv, errbuf);
					return FALSE;
				}
				// grammar 不匹配
				if (!check_grammar(recog_channel->grammar)){
					return FALSE;
				}
			}
			break;
		case RECOGNIZER_RECOGNIZE:
			processed = vosk_recog_channel_recognize(channel,request,response);
			break;
		case RECOGNIZER_GET_RESULT:
			break;
		case RECOGNIZER_START_INPUT_TIMERS:
			processed = vosk_recog_channel_timers_start(channel,request,response);
			break;
		case RECOGNIZER_STOP:
			processed = vosk_recog_channel_stop(channel,request,response);
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t vosk_recog_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t vosk_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t vosk_recog_stream_close(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/* Raise kaldi START-OF-INPUT event */
static apt_bool_t vosk_recog_start_of_input(vosk_recog_channel_t *recog_channel)
{
	/* create START-OF-INPUT event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_START_OF_INPUT,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}

	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}


/* Raise kaldi RECOGNITION-COMPLETE event */
static apt_bool_t vosk_recog_recognition_complete(vosk_recog_channel_t *recog_channel, mrcp_recog_completion_cause_e cause, char *early)
{
	mrcp_recog_header_t *recog_header;
	/* create RECOGNITION-COMPLETE event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_RECOGNITION_COMPLETE,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}

	/* get/allocate recognizer header */
	recog_header = (mrcp_recog_header_t*)mrcp_resource_header_prepare(message);
	if(recog_header) {
		/* set completion cause */
		recog_header->completion_cause = cause;
		mrcp_resource_header_property_add(message,RECOGNIZER_HEADER_COMPLETION_CAUSE);
	}
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

	if(cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS) {
		{
			const char *result = vosk_recognizer_result(recog_channel->recognizer);
			if(early){
				char * buffer = (char *)malloc(strlen(result)+strlen(early));
				strcpy(buffer,result);
				strcat(buffer,early);
				apt_string_assign_n(&message->body,buffer,strlen(buffer),message->pool);
				free((void *)buffer);
			}else{
				apt_string_assign_n(&message->body,result,strlen(result),message->pool);
			}
		}
		{
			/* get/allocate generic header */
			mrcp_generic_header_t *generic_header = mrcp_generic_header_prepare(message);
			if(generic_header) {
				/* set content types */
				apt_string_assign(&generic_header->content_type,"application/x-nlsml",message->pool);
				mrcp_generic_header_property_add(message,GENERIC_HEADER_CONTENT_TYPE);
			}
		}
	}

	recog_channel->recog_request = NULL;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

/** Process parsed XML document */
static char* prase_grammar(apr_xml_doc *doc,const char *result)
{
	const apr_xml_elem *elem;
	const apr_xml_elem *child;
	const apr_xml_attr *attr;

	
    regmatch_t pmatch[1];
    const size_t nmatch = 1;
    regex_t reg;
    int cflags = REG_EXTENDED;
    int status;
    
	/* Navigate through document */
	for (elem = doc->root->first_child; elem; elem = elem->next) {
		char *id = NULL;
		if(!elem || strcasecmp(elem->name,"rule") == 0) {
			for (attr = elem->attr; attr; attr = attr->next) {
				if(strcasecmp(attr->name,"id") == 0) {
					id = attr->value;
				}
			}
			if(elem->first_child){
				for(child = elem->first_child; child; child = child->next) {
					if(!child->first_cdata.first || !child->first_cdata.first->text){
						continue;
					}
					char *pattern = (char *)malloc(strlen(child->first_cdata.first->text)+1);
					strcpy(pattern, child->first_cdata.first->text);
					strcat(pattern,".");
					// if(strstr(result,child->first_cdata.first->text)){
					// 	apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "Match id <%s> child <%s> result <%s>", id, child->first_cdata.first->text, result);
					// 	regfree(&reg);
					// 	return id;
					// }
					// init regex
					status = regcomp(&reg, pattern, cflags);
					if(status != REG_NOERROR){
						return NULL;
					}
					// exec regex
					status = regexec(&reg, result, nmatch, pmatch, 0);
					regfree(&reg);
					free((void*)pattern);
					if(status == REG_NOERROR){
						return id;
					}
				}
			}
		}
	}
	return NULL;
}

/** Callback is called from MPF engine context to write/send new frame */
static apt_bool_t vosk_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
	vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)stream->obj;
	if(recog_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(recog_channel->channel,recog_channel->stop_response);
		recog_channel->stop_response = NULL;
		recog_channel->recog_request = NULL;
		return TRUE;
	}

	if(recog_channel->recog_request) {
		mpf_detector_event_e det_event = mpf_activity_detector_process(recog_channel->detector,frame);
		int end = 0;
		switch(det_event) {
			case MPF_DETECTOR_EVENT_ACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Activity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				vosk_recog_start_of_input(recog_channel);
				break;
			case MPF_DETECTOR_EVENT_INACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Inactivity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				vosk_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_SUCCESS,NULL);
				end = 1;
				break;
			case MPF_DETECTOR_EVENT_NOINPUT:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Noinput " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				if(recog_channel->timers_started == TRUE) {
					vosk_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT,NULL);
					end = 1;
				}
				break;
			default:
				break;
		}

		if(recog_channel->recog_request) {
			if((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
				if(frame->marker == MPF_MARKER_START_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Start of Event " APT_SIDRES_FMT " id:%d",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id);
				}
				else if(frame->marker == MPF_MARKER_END_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected End of Event " APT_SIDRES_FMT " id:%d duration:%d ts",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id,
						frame->event_frame.duration);
				}
			}
		}

		if(recog_channel->audio_out) {
			fwrite(frame->codec_frame.buffer,1,frame->codec_frame.size,recog_channel->audio_out);
		}
		if(recog_channel->recognizer) {
			int ret = -1;
			ret = vosk_recognizer_accept_waveform(recog_channel->recognizer, (const char*)frame->codec_frame.buffer, frame->codec_frame.size);
			if (ret) {
				vosk_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_SUCCESS,NULL);
			} else if (ret == 0 && end == 0) {
				const char *result = vosk_recognizer_partial_result(recog_channel->recognizer);
				const char *early = prase_grammar(recog_channel->grammar, result);
				if (early) {
					// apt_log(APT_LOG_MARK, APT_PRIO_INFO, "Match id <%s>", early);
					char *buffer = (char *)malloc(strlen(early)+21);
					strcpy(buffer,"<earlyres>");
					strcat(buffer, early);
					strcat(buffer,"</earlyres>");
					vosk_recog_recognition_complete(recog_channel, RECOGNIZER_COMPLETION_CAUSE_SUCCESS, buffer);
					free((void *)buffer);
				} 
			}
		}
	}
	return TRUE;
}

static apt_bool_t vosk_recog_msg_signal(vosk_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	vosk_recog_channel_t *kaldi_channel = (vosk_recog_channel_t*)channel->method_obj;
	vosk_recog_engine_t *kaldi_engine = kaldi_channel->kaldi_engine;
	apt_task_t *task = apt_consumer_task_base_get(kaldi_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		vosk_recog_msg_t *kaldi_msg;
		msg->type = TASK_MSG_USER;
		kaldi_msg = (vosk_recog_msg_t*) msg->data;

		kaldi_msg->type = type;
		kaldi_msg->channel = channel;
		kaldi_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

static apt_bool_t vosk_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	vosk_recog_msg_t *kaldi_msg = (vosk_recog_msg_t*)msg->data;
	switch(kaldi_msg->type) {
		case vosk_recog_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(kaldi_msg->channel,TRUE);
			break;
		case vosk_recog_MSG_CLOSE_CHANNEL:
		{
			/* close channel, make sure there is no activity and send asynch response */
			vosk_recog_channel_t *recog_channel = (vosk_recog_channel_t*)kaldi_msg->channel->method_obj;
			if(recog_channel->audio_out) {
				fclose(recog_channel->audio_out);
				recog_channel->audio_out = NULL;
			}
			if(recog_channel->recognizer) {
				vosk_recognizer_free(recog_channel->recognizer);
				recog_channel->recognizer = NULL;
			}

			mrcp_engine_channel_close_respond(kaldi_msg->channel);
			break;
		}
		case vosk_recog_MSG_REQUEST_PROCESS:
			vosk_recog_channel_request_dispatch(kaldi_msg->channel,kaldi_msg->request);
			break;
		default:
			break;
	}
	return TRUE;
}
