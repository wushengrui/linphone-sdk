/*
 * Copyright (c) 2010-2022 Belledonne Communications SARL.
 *
 * This file is part of mediastreamer2
 * (see https://gitlab.linphone.org/BC/public/mediastreamer2).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>

#include <bctoolbox/defs.h>

#include "mediastreamer2/mediastream.h"
#include "mediastreamer2/mseventqueue.h"
#include "mediastreamer2/msextdisplay.h"
#include "mediastreamer2/msfilter.h"
#include "mediastreamer2/msinterfaces.h"
#include "mediastreamer2/msitc.h"
#include "mediastreamer2/msqrcodereader.h"
#include "mediastreamer2/msrtp.h"
#include "mediastreamer2/mstee.h"
#include "mediastreamer2/msvideo.h"
#include "mediastreamer2/msvideoout.h"
#include "mediastreamer2/msvideopresets.h"
#include "mediastreamer2/video-aggregator.h"
#include "mediastreamer2/zrtp.h"
#include "private.h"

#if __APPLE__
#include "TargetConditionals.h"
#endif

#define MS2_NO_VIDEO_RESCALING 1

static void configure_recorder_output(VideoStream *stream);
static int video_stream_start_with_source_and_output(VideoStream *stream,
                                                     RtpProfile *profile,
                                                     const char *rem_rtp_ip,
                                                     int rem_rtp_port,
                                                     const char *rem_rtcp_ip,
                                                     int rem_rtcp_port,
                                                     int payload,
                                                     int jitt_comp,
                                                     MSWebCam *cam,
                                                     MSFilter *source,
                                                     MSFilter *output);
static void _configure_video_preview_source(VideoPreview *stream, bool_t change_source);
static void configure_video_preview_source(VideoPreview *stream);

static void assign_value_to_mirroring_flag_to_preview(VideoStream *stream) {
	if (stream && stream->output2) {
		MSWebCam *cam = stream->cam;
		int mirroring = TRUE;
		if (cam) {
			const char *cam_name = ms_web_cam_get_string_id(cam);
			if (cam_name) {
				// If the camera does't show a static image, then set mirroring to 1, 0 otherwise
				mirroring = (strstr(cam_name, "Static picture") == NULL);
			}
		} else if (stream->source && ms_filter_get_id(stream->source) == MS_SCREEN_SHARING_ID) {
			mirroring = FALSE;
		}
		ms_filter_call_method(stream->output2, MS_VIDEO_DISPLAY_ENABLE_MIRRORING, &mirroring);
	}
}

static void configure_sink(VideoStream *stream, MSFilter *sink) {
	MSPinFormat pf = {0};
	ms_filter_call_method(stream->ms.decoder, MS_FILTER_GET_OUTPUT_FMT, &pf);
	if (pf.fmt) {
		MSPinFormat pinfmt = {0};
		RtpSession *session = stream->ms.sessions.rtp_session;
		PayloadType *pt =
		    rtp_profile_get_payload(rtp_session_get_profile(session), rtp_session_get_recv_payload_type(session));
		if (!pt)
			pt = rtp_profile_get_payload(rtp_session_get_profile(session), rtp_session_get_send_payload_type(session));
		if (pt) {
			MSFmtDescriptor tmp = *pf.fmt;
			tmp.encoding = pt->mime_type;
			tmp.rate = pt->clock_rate;
			pinfmt.pin = 0;
			pinfmt.fmt = ms_factory_get_format(stream->ms.factory, &tmp);
			ms_filter_call_method(sink, MS_FILTER_SET_INPUT_FMT, &pinfmt);
			ms_message("configure_itc(): format set to %s", ms_fmt_descriptor_to_string(pinfmt.fmt));
		}
	} else ms_warning("configure_itc(): video decoder doesn't give output format.");
}

static void video_stream_update_jitter_for_nack(const OrtpEventData *evd, VideoStream *stream) {
	if (stream->audiostream != NULL) {
		JBParameters jitter_params;
		RtpSession *session = audio_stream_get_rtp_session(stream->audiostream);

		rtp_session_get_jitter_buffer_params(session, &jitter_params);
		jitter_params.min_size = evd->info.jitter_min_size_for_nack;

		rtp_session_get_jitter_buffer_params(session, &jitter_params);
	}
}

static void on_incoming_ssrc_in_bundle(RtpSession *session, void *mp, void *s, void *userData) {
	mblk_t *m = (mblk_t *)mp;
	uint32_t ssrc = rtp_get_ssrc(m);
	RtpSession **newSession = (RtpSession **)s;
	VideoStream *stream = (VideoStream *)userData;

	/* fetch the MID from the packet and check it is in sync with the current RtpSession one
	 * Do not create a new session (-> packet drop) if :
	 * - no MID in packet
	 * - current session and packet MID do not match
	 */
	int midId = rtp_bundle_get_mid_extension_id(session->bundle);
	uint8_t *mid = NULL;
	char *sMid = NULL;
	size_t midSize = rtp_get_extension_header(m, midId != -1 ? midId : RTP_EXTENSION_MID, &mid);
	if (midSize == (size_t)-1) {
		/* there is no MID in the incoming packet */
		ms_warning("New incoming SSRC %u on session %p but no MID found in the incoming packet", ssrc, session);
		return;
	} else {
		sMid = bctbx_malloc0(midSize + 1);
		memcpy(sMid, mid, midSize);
		/* Check the mid in packet matches the stream's session one */
		char *streamMid = rtp_bundle_get_session_mid(session->bundle, stream->ms.sessions.rtp_session);
		if ((strlen(streamMid) != midSize) || (memcmp(mid, streamMid, midSize) != 0)) {
			ms_warning("New incoming SSRC %u on session %p but packet Mid %s differs from session mid %s", ssrc,
			           session, sMid, streamMid);
			bctbx_free(streamMid);
			bctbx_free(sMid);
			return;
		}
		if (streamMid != NULL) bctbx_free(streamMid);
	}

	// Do nothing if the aggregator is not created
	if (stream->aggregator == NULL) {
		ms_warning("New incoming SSRC %u on session %p but aggregator has not yet been instanciated.", ssrc, session);
		bctbx_free(sMid);
		return;
	}

	// If a branch slot is available, create a new session and assign it
	bool_t available = FALSE;
	for (int i = 0; i < VIDEO_STREAM_MAX_BRANCHES; i++) {
		if (stream->branches[i].session == NULL) {
			*newSession =
			    media_stream_rtp_session_new_from_session(stream->ms.sessions.rtp_session, RTP_SESSION_RECVONLY);
			stream->ms.sessions.auxiliary_sessions =
			    bctbx_list_append(stream->ms.sessions.auxiliary_sessions, *newSession);

			stream->branches[i].session = *newSession;
			ms_filter_call_method(stream->branches[i].recv, MS_RTP_RECV_SET_SESSION, *newSession);

			ms_message(
			    "New incoming SSRC %u on session [%p] detected, create a new session [%p] attach it to branch %d", ssrc,
			    session, *newSession, i);
			available = TRUE;
			break;
		}
	}

	// If not, check for the last session used and use this one instead
	if (!available) {
		VideoStreamRecvBranch *recycled_branch = &stream->branches[0];
		struct timeval oldest_recv_ts;
		rtp_session_get_last_recv_time(recycled_branch->session, &oldest_recv_ts);

		for (int i = 1; i < VIDEO_STREAM_MAX_BRANCHES; i++) {
			struct timeval tv;
			rtp_session_get_last_recv_time(stream->branches[i].session, &tv);
			if (tv.tv_sec < oldest_recv_ts.tv_sec) { /* selection is performed on tv_sec only, no need to be precise */
				oldest_recv_ts.tv_sec = tv.tv_sec;
				recycled_branch = &stream->branches[i];
			}
		}

		// This method will resync the session, but it will be done by the RtpRecv filter and not this thread.
		ms_filter_call_method_noarg(recycled_branch->recv, MS_RTP_RECV_RESET_JITTER_BUFFER);

		*newSession = recycled_branch->session;

		ms_message(
		    "No free branch found on session [%p], so recycle session [%p] used to receive SSRC %u switched to %u",
		    session, recycled_branch->session, recycled_branch->session->rcv.ssrc, ssrc);
	}

	bctbx_free(sMid);
}

void video_stream_free(VideoStream *stream) {
	bool_t rtp_source = FALSE;
	bool_t rtp_output = FALSE;

	ortp_ev_dispatcher_disconnect(stream->ms.evd, ORTP_EVENT_JITTER_UPDATE_FOR_NACK, 0,
	                              (OrtpEvDispatcherCb)video_stream_update_jitter_for_nack);

	if ((stream->source != NULL) && (ms_filter_get_id(stream->source) == MS_RTP_RECV_ID)) rtp_source = TRUE;
	if ((stream->output != NULL) && (ms_filter_get_id(stream->output) == MS_RTP_SEND_ID)) rtp_output = TRUE;

	/* Prevent filters from being destroyed two times */
	if ((stream->source_performs_encoding == TRUE) || (rtp_source == TRUE)) {
		stream->ms.encoder = NULL;
	}
	if ((stream->output_performs_decoding == TRUE) || (rtp_output == TRUE)) {
		stream->ms.decoder = NULL;
	}

	if (stream->nack_context) video_stream_enable_retransmission_on_nack(stream, FALSE);

	if (stream->ms.video_quality_controller) ms_video_quality_controller_destroy(stream->ms.video_quality_controller);

	if (stream->active_speaker_mode == TRUE) {
		RtpBundle *bundle = stream->ms.sessions.rtp_session->bundle;
		if (bundle) {
			rtp_session_signal_disconnect_by_callback_and_user_data(rtp_bundle_get_primary_session(bundle),
			                                                        "new_incoming_ssrc_found_in_bundle",
			                                                        on_incoming_ssrc_in_bundle, stream);
		}
	}

	if (stream->ms.transfer_mode == TRUE) {
		RtpBundle *bundle = stream->ms.sessions.rtp_session->bundle;
		if (bundle) {
			rtp_session_signal_disconnect_by_callback_and_user_data(
			    rtp_bundle_get_primary_session(bundle), "new_outgoing_ssrc_found_in_bundle",
			    media_stream_on_outgoing_ssrc_in_bundle, &stream->ms);
		}
	}

	media_stream_free(&stream->ms);

	if (stream->jpegwriter) ms_filter_destroy(stream->jpegwriter);
	if (stream->local_jpegwriter) ms_filter_destroy(stream->local_jpegwriter);
	if (stream->output) ms_filter_destroy(stream->output);
	if (stream->output2) ms_filter_destroy(stream->output2);
	if (stream->pixconv) ms_filter_destroy(stream->pixconv);
	if (stream->qrcode) ms_filter_destroy(stream->qrcode);
	if (stream->recorder_output) ms_filter_destroy(stream->recorder_output);
	if (stream->rtp_io_session) rtp_session_destroy(stream->rtp_io_session);
	if (stream->sizeconv) ms_filter_destroy(stream->sizeconv);
	if (stream->source) ms_filter_destroy(stream->source);
	if (stream->tee) ms_filter_destroy(stream->tee);
	if (stream->tee2) ms_filter_destroy(stream->tee2);
	if (stream->tee3) ms_filter_destroy(stream->tee3);
	if (stream->void_source) ms_filter_destroy(stream->void_source);
	if (stream->itcsink) ms_filter_destroy(stream->itcsink);
	if (stream->forward_sink) ms_filter_destroy(stream->forward_sink);
	if (stream->aggregator) ms_filter_destroy(stream->aggregator);

	for (int i = 0; i < VIDEO_STREAM_MAX_BRANCHES; i++) {
		if (stream->branches[i].recv) ms_filter_destroy(stream->branches[i].recv);
	}

	if (stream->display_name) ms_free(stream->display_name);
	if (stream->preset) ms_free(stream->preset);
	if (stream->label) ms_free(stream->label);

	ms_free(stream);
}

static void source_event_cb(void *ud, MSFilter *f, unsigned int event, BCTBX_UNUSED(void *eventdata)) {
	VideoStream *st = (VideoStream *)ud;
	MSVideoSize size;
	switch (event) { // Allow a source to reinitialize all tree formats
		case MS_FILTER_OUTPUT_FMT_CHANGED:
			if (ms_filter_get_id(f) == MS_SIZE_CONV_ID) {
				ms_filter_call_method(f, MS_FILTER_GET_VIDEO_SIZE, &size);
				video_stream_set_sent_video_size(st, size);
			}
			video_stream_update_video_params(st);
			break;
		default: {
		}
	}
}

static void preview_source_event_cb(void *ud, MSFilter *f, unsigned int event, BCTBX_UNUSED(void *eventdata)) {
	VideoStream *st = (VideoStream *)ud;
	MSVideoSize size;
	switch (event) { // Allow a source to reinitialize all tree formats
		case MS_FILTER_OUTPUT_FMT_CHANGED:
			if (ms_filter_get_id(f) == MS_SIZE_CONV_ID) {
				ms_filter_call_method(f, MS_FILTER_GET_VIDEO_SIZE, &size);
				video_stream_set_sent_video_size(st, size);
			}
			video_preview_stream_update_video_params(st);
			break;
		default: {
		}
	}
}

static void event_cb(void *ud, MSFilter *f, unsigned int event, void *eventdata) {
	VideoStream *st = (VideoStream *)ud;
	if (st->eventcb != NULL) {
		st->eventcb(st->event_pointer, f, event, eventdata);
	}
}

static void internal_event_cb(void *ud, BCTBX_UNUSED(MSFilter *f), unsigned int event, void *eventdata) {
	VideoStream *stream = (VideoStream *)ud;
	const MSVideoCodecSLI *sli;
	const MSVideoCodecRPSI *rpsi;

	switch (event) {
		case MS_VIDEO_DECODER_SEND_FIR:
			ms_message("Request sending of FIR on videostream [%p]", stream);
			video_stream_send_fir(stream);
			break;
		case MS_VIDEO_DECODER_SEND_PLI:
			ms_message("Request sending of PLI on videostream [%p]", stream);
			video_stream_send_pli(stream);
			break;
		case MS_VIDEO_DECODER_SEND_SLI:
			sli = (const MSVideoCodecSLI *)eventdata;
			ms_message("Request sending of SLI on videostream [%p]", stream);
			video_stream_send_sli(stream, sli->first, sli->number, sli->picture_id);
			break;
		case MS_VIDEO_DECODER_SEND_RPSI:
			rpsi = (const MSVideoCodecRPSI *)eventdata;
			ms_message("Request sending of RPSI on videostream [%p]", stream);
			video_stream_send_rpsi(stream, rpsi->bit_string, rpsi->bit_string_len);
			break;
		case MS_FILTER_OUTPUT_FMT_CHANGED:
			if (stream->recorder_output) configure_recorder_output(stream);
			if (stream->forward_sink) configure_sink(stream, stream->forward_sink);
			break;
		case MS_CAMERA_PREVIEW_SIZE_CHANGED:
			ms_message("Camera video preview size changed on videostream [%p]", stream);
			break;
	}
}

static void display_cb(void *ud, BCTBX_UNUSED(MSFilter *f), unsigned int event, void *eventdata) {
	VideoStream *st = (VideoStream *)ud;
	if (st->displaycb != NULL) {
		st->displaycb(st->display_pointer, event, eventdata);
	}
}

static void video_stream_process_encoder_control(VideoStream *stream,
                                                 unsigned int method_id,
                                                 void *arg,
                                                 BCTBX_UNUSED(void *user_data)) {
	ms_filter_call_method(stream->ms.encoder, method_id, arg);
}

static bool_t video_stream_is_rtcp_ssrc_valid(VideoStream *stream, uint32_t ssrc) {
	// Check first the send ssrc of the current RtpSession
	if (ssrc == rtp_session_get_send_ssrc(stream->ms.sessions.rtp_session)) return TRUE;

	// Then check auxiliary RtpSessions
	for (bctbx_list_t *it = stream->ms.sessions.auxiliary_sessions; it != NULL; it = it->next) {
		RtpSession *aux = (RtpSession *)it->data;
		if (aux != NULL && ssrc == rtp_session_get_send_ssrc(aux)) return TRUE;
	}

	return FALSE;
}

static void video_stream_process_rtcp(MediaStream *media_stream, const mblk_t *m) {
	VideoStream *stream = (VideoStream *)media_stream;
	int i;

	if (rtcp_is_PSFB(m) && (stream->ms.encoder != NULL)) {
		/* Ignore PSFB goog-remb messages */
		if (rtcp_PSFB_get_type(m) == RTCP_PSFB_AFB && rtcp_PSFB_is_goog_remb(m)) return;

		/* The PSFB messages are to be notified to the encoder, so if we have no encoder simply ignore them. */

		if (rtcp_PSFB_get_type(m) == RTCP_PSFB_FIR) {
			/* For FIR message, media source SSRC is to be ignored and replaced by the FCI SSRC */
			for (i = 0;; i++) {
				rtcp_fb_fir_fci_t *fci = rtcp_PSFB_fir_get_fci(m, i);
				if (fci == NULL) break;
				if (video_stream_is_rtcp_ssrc_valid(stream, rtcp_fb_fir_fci_get_ssrc(fci))) {
					uint8_t seq_nr = rtcp_fb_fir_fci_get_seq_nr(fci);
					/* TODO: manage seq_nr and ignore FIR repeats to avoid flooding the encoder */
					stream->encoder_control_cb(stream, MS_VIDEO_ENCODER_NOTIFY_FIR, &seq_nr,
					                           stream->encoder_control_cb_user_data);
					stream->ms_video_stat.counter_rcvd_fir++;
					ms_message("Got RTCP FIR on video stream [%p] SSRC [%x] count %d, seq %u", stream,
					           rtcp_fb_fir_fci_get_ssrc(fci), stream->ms_video_stat.counter_rcvd_fir, seq_nr);

					break;
				} else {
					ms_message("Ignoring RTCP FIR on SSRC [%x]. SSRC of video sender is [%x]",
					           rtcp_fb_fir_fci_get_ssrc(fci),
					           rtp_session_get_send_ssrc(stream->ms.sessions.rtp_session));
				}
			}
			return;
		}

		if (video_stream_is_rtcp_ssrc_valid(stream, rtcp_PSFB_get_media_source_ssrc(m))) {
			switch (rtcp_PSFB_get_type(m)) {
				case RTCP_PSFB_PLI:

					stream->ms_video_stat.counter_rcvd_pli++;
					stream->encoder_control_cb(stream, MS_VIDEO_ENCODER_NOTIFY_PLI, NULL,
					                           stream->encoder_control_cb_user_data);
					ms_message("Got RTCP PLI on video stream [%p] SSRC [%x] count %d", stream,
					           rtcp_PSFB_get_media_source_ssrc(m), stream->ms_video_stat.counter_rcvd_pli);
					break;
				case RTCP_PSFB_SLI:
					for (i = 0;; i++) {

						rtcp_fb_sli_fci_t *fci = rtcp_PSFB_sli_get_fci(m, i);
						MSVideoCodecSLI sli;
						if (fci == NULL) break;
						sli.first = rtcp_fb_sli_fci_get_first(fci);
						sli.number = rtcp_fb_sli_fci_get_number(fci);
						sli.picture_id = rtcp_fb_sli_fci_get_picture_id(fci);
						stream->encoder_control_cb(stream, MS_VIDEO_ENCODER_NOTIFY_SLI, &sli,
						                           stream->encoder_control_cb_user_data);
						stream->ms_video_stat.counter_rcvd_sli++;
						ms_message("video_stream_process_rtcp stream [%p] SLI count %d", stream,
						           stream->ms_video_stat.counter_rcvd_sli);
					}
					break;
				case RTCP_PSFB_RPSI: {
					rtcp_fb_rpsi_fci_t *fci = rtcp_PSFB_rpsi_get_fci(m);
					MSVideoCodecRPSI rpsi;
					rpsi.bit_string = rtcp_fb_rpsi_fci_get_bit_string(fci);
					rpsi.bit_string_len = rtcp_PSFB_rpsi_get_fci_bit_string_len(m);
					stream->encoder_control_cb(stream, MS_VIDEO_ENCODER_NOTIFY_RPSI, &rpsi,
					                           stream->encoder_control_cb_user_data);
					stream->ms_video_stat.counter_rcvd_rpsi++;
					ms_message("video_stream_process_rtcp stream [%p] RPSI count %d", stream,
					           stream->ms_video_stat.counter_rcvd_rpsi);
				} break;
				default:
					break;
			}
		} else {
			ms_message("RTCP payload specific feedback of type %d for unknown SSRC %x was ignored. Our SSRC is %x",
			           rtcp_PSFB_get_type(m), rtcp_PSFB_get_media_source_ssrc(m),
			           rtp_session_get_send_ssrc(stream->ms.sessions.rtp_session));
		}
	}
}

void video_stream_set_encoder_control_callback(VideoStream *stream, VideoStreamEncoderControlCb cb, void *user_data) {
	if (cb == NULL) {
		cb = video_stream_process_encoder_control;
		user_data = NULL;
	}
	stream->encoder_control_cb = cb;
	stream->encoder_control_cb_user_data = user_data;
}

static void stop_preload_graph(VideoStream *stream) {
	ms_ticker_detach(stream->ms.sessions.ticker, stream->ms.rtprecv);
	ms_filter_unlink(stream->ms.rtprecv, 0, stream->ms.voidsink, 0);
	ms_filter_destroy(stream->ms.voidsink);
	ms_filter_destroy(stream->ms.rtprecv);
	stream->ms.voidsink = stream->ms.rtprecv = NULL;
}

static void video_stream_track_fps_changes(VideoStream *stream) {
	uint64_t curtime = bctbx_get_cur_time_ms();
	if (stream->last_fps_check == (uint64_t)-1) {
		stream->last_fps_check = curtime;
		return;
	}
	if (curtime - stream->last_fps_check >= 2000 && stream->configured_fps > 0 && stream->ms.sessions.ticker) {
		MSTickerLateEvent late_ev = {0};
		/*we must check that no late tick occured during the last 2 seconds, otherwise the fps measurement is severely
		 * biased.*/
		ms_ticker_get_last_late_tick(stream->ms.sessions.ticker, &late_ev);

		if (curtime > late_ev.time + 2000) {
			if (stream->source && stream->ms.encoder && ms_filter_has_method(stream->source, MS_FILTER_GET_FPS) &&
			    ms_filter_has_method(stream->ms.encoder, MS_VIDEO_ENCODER_GET_CONFIGURATION) &&
			    ms_filter_has_method(stream->ms.encoder, MS_VIDEO_ENCODER_SET_CONFIGURATION)) {
				float fps = 0;

				if (ms_filter_call_method(stream->source, MS_FILTER_GET_FPS, &fps) == 0 && fps >= 1.0) {
					if (fabsf(fps - stream->configured_fps) / stream->configured_fps > 0.2) {
						MSVideoConfiguration vconf;
						ms_warning("Measured and target fps significantly different (%f<->%f), updating encoder.", fps,
						           stream->configured_fps);
						stream->real_fps = fps;

						ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_GET_CONFIGURATION, &vconf);
						vconf.fps = stream->real_fps;
						ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_SET_CONFIGURATION, &vconf);
					}
				}
			}
			stream->last_fps_check = curtime;
		}
	}
}

#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif // _MSC_VER
static void video_stream_check_camera(VideoStream *stream) {
#if !defined(__ANDROID__) && !TARGET_OS_IPHONE
	uint64_t curtime = bctbx_get_cur_time_ms();
	if (stream->last_camera_check == (uint64_t)-1) {
		stream->last_camera_check = curtime;
		return;
	}

	if (curtime - stream->last_camera_check >= 1000) {
		stream->last_camera_check = curtime;
		const MSWebCam *camera = video_stream_get_camera(stream);
		if (!camera || strcmp("StaticImage", camera->desc->driver_type) == 0) {
			return;
		}
		if (stream->source && ms_filter_has_method(stream->source, MS_FILTER_GET_FPS)) {
			float fps;
			if (ms_filter_call_method(stream->source, MS_FILTER_GET_FPS, &fps) == 0 && fps == 0) {
				stream->dead_camera_check_count++;
				if (stream->dead_camera_check_count >= 5) {
					MSWebCam *nowebcam = ms_web_cam_manager_get_cam(camera->wbcmanager, "StaticImage: Static picture");
					ms_error(
					    "Camera is not delivering any frames over last 5 seconds, switching to no-webcam placeholder.");
					video_stream_change_camera(stream, nowebcam);
					stream->dead_camera_check_count = 0;
					if (stream->cameracb != NULL) {
						stream->cameracb(stream->camera_pointer, camera);
					}
				}
			} else {
				stream->dead_camera_check_count = 0;
			}
		}
	}
#endif
}
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif // _MSC_VER

void video_stream_iterate(VideoStream *stream) {
	media_stream_iterate(&stream->ms);
	video_stream_track_fps_changes(stream);
	video_stream_check_camera(stream);

	if (stream->ms.video_quality_controller) {
		ms_video_quality_controller_process_timers(stream->ms.video_quality_controller);
	}
	if (stream->nack_context) {
		ortp_nack_context_process_timer(stream->nack_context);
	}
}

static void choose_display_name(VideoStream *stream) {
#if defined(__ANDROID__)
	MSDevicesInfo *devices = ms_factory_get_devices_info(stream->ms.factory);
	SoundDeviceDescription *description = ms_devices_info_get_sound_device_description(devices);
	if (description->flags & DEVICE_HAS_CRAPPY_OPENGL) stream->display_name = ms_strdup("MSAndroidDisplay");
	else stream->display_name = ms_strdup(ms_factory_get_default_video_renderer(stream->ms.factory));
#else
	stream->display_name = ms_strdup(ms_factory_get_default_video_renderer(stream->ms.factory));
#endif
}

static float video_stream_get_rtcp_xr_average_quality_rating(void *userdata) {
	VideoStream *stream = (VideoStream *)userdata;
	return stream ? media_stream_get_average_quality_rating(&stream->ms) : -1;
}

static float video_stream_get_rtcp_xr_average_lq_quality_rating(void *userdata) {
	VideoStream *stream = (VideoStream *)userdata;
	return stream ? media_stream_get_average_lq_quality_rating(&stream->ms) : -1;
}

VideoStream *video_stream_new(MSFactory *factory, int loc_rtp_port, int loc_rtcp_port, bool_t use_ipv6) {
	return video_stream_new2(factory, use_ipv6 ? "::" : "0.0.0.0", loc_rtp_port, loc_rtcp_port);
}

VideoStream *video_stream_new2(MSFactory *factory, const char *ip, int loc_rtp_port, int loc_rtcp_port) {
	MSMediaStreamSessions sessions = {0};
	VideoStream *obj;
	sessions.rtp_session = ms_create_duplex_rtp_session(ip, loc_rtp_port, loc_rtcp_port, ms_factory_get_mtu(factory));
	obj = video_stream_new_with_sessions(factory, &sessions);
	obj->ms.owns_sessions = TRUE;
	obj->display_mode = MSVideoDisplayHybrid;
	obj->preview_display_mode = MSVideoDisplayHybrid;
	return obj;
}

VideoStream *video_stream_new_with_sessions(MSFactory *factory, const MSMediaStreamSessions *sessions) {
	VideoStream *stream = (VideoStream *)ms_new0(VideoStream, 1);
	const OrtpRtcpXrMediaCallbacks rtcp_xr_media_cbs = {NULL,
	                                                    NULL,
	                                                    NULL,
	                                                    video_stream_get_rtcp_xr_average_quality_rating,
	                                                    video_stream_get_rtcp_xr_average_lq_quality_rating,
	                                                    stream};

	stream->ms.type = MSVideo;
	stream->ms.sessions = *sessions;

	media_stream_init(&stream->ms, factory, sessions);

	rtp_session_resync(stream->ms.sessions.rtp_session);
	stream->ms.qi = ms_quality_indicator_new(stream->ms.sessions.rtp_session);
	ms_quality_indicator_set_label(stream->ms.qi, "video");

	stream->ms.rtpsend = ms_factory_create_filter(stream->ms.factory, MS_RTP_SEND_ID);

	stream->ms.ice_check_list = NULL;
	MS_VIDEO_SIZE_ASSIGN(stream->sent_vsize, CIF);
	stream->forced_fps = 0;
	stream->real_fps = 0;
	stream->dir = MediaStreamSendRecv;
	media_stream_set_direction(&stream->ms, MediaStreamSendRecv);
	stream->display_filter_auto_rotate_enabled = 0;
	stream->freeze_on_error = FALSE;
	stream->source_performs_encoding = FALSE;
	stream->output_performs_decoding = FALSE;
	stream->content = MSVideoContentDefault;
	choose_display_name(stream);
	stream->ms.process_rtcp = video_stream_process_rtcp;
	video_stream_set_encoder_control_callback(stream, NULL, NULL);
	/*
	 * In practice, these filters are needed only for audio+video recording.
	 */
	if (ms_factory_lookup_filter_by_id(stream->ms.factory, MS_MKV_RECORDER_ID)) {

		stream->tee3 = ms_factory_create_filter(stream->ms.factory, MS_TEE_ID);
		stream->recorder_output = ms_factory_create_filter(stream->ms.factory, MS_ITC_SINK_ID);
	}

	rtp_session_set_rtcp_xr_media_callbacks(stream->ms.sessions.rtp_session, &rtcp_xr_media_cbs);

	stream->staticimage_webcam_fps_optimization = TRUE;
	stream->vconf_list = NULL;
	stream->frame_marking_extension_id = 0;

	stream->is_forwarding = FALSE;

	stream->csrc_changed_cb = NULL;
	stream->csrc_changed_cb_user_data = NULL;
	stream->new_csrc = 0;
	stream->wait_for_frame_decoded = FALSE;

	ortp_ev_dispatcher_connect(stream->ms.evd, ORTP_EVENT_JITTER_UPDATE_FOR_NACK, 0,
	                           (OrtpEvDispatcherCb)video_stream_update_jitter_for_nack, stream);
	stream->fallback_to_dummy_codec = TRUE;

	if (!stream->ms.video_quality_controller) {
		stream->ms.video_quality_controller = ms_video_quality_controller_new(stream);
	}

	stream->active_speaker_mode = FALSE;

	return stream;
}

void video_stream_set_sent_video_size(VideoStream *stream, MSVideoSize vsize) {
	ms_message("Setting video size %dx%d on stream [%p]", vsize.width, vsize.height, stream);
	stream->sent_vsize = vsize;
}

void video_stream_set_preview_size(VideoStream *stream, MSVideoSize vsize) {
	ms_message("Setting preview video size %dx%d", vsize.width, vsize.height);
	stream->preview_vsize = vsize;
}

void video_stream_set_fps(VideoStream *stream, float fps) {
	stream->forced_fps = fps;
}

MSVideoSize video_stream_get_sent_video_size(const VideoStream *stream) {
	MSVideoConfiguration vconf;
	MS_VIDEO_SIZE_ASSIGN(vconf.vsize, UNKNOWN);
	if (stream->ms.encoder != NULL) {
		ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_GET_CONFIGURATION, &vconf);
	}
	return vconf.vsize;
}

MSVideoSize video_stream_get_received_video_size(const VideoStream *stream) {
	MSVideoSize vsize;
	MS_VIDEO_SIZE_ASSIGN(vsize, UNKNOWN);
	if (stream->ms.decoder != NULL) {
		ms_filter_call_method(stream->ms.decoder, MS_FILTER_GET_VIDEO_SIZE, &vsize);
	}
	return vsize;
}

float video_stream_get_sent_framerate(const VideoStream *stream) {
	float fps = 0;
	if (stream->source) {
		if (ms_filter_has_method(stream->source, MS_FILTER_GET_FPS)) {
			ms_filter_call_method(stream->source, MS_FILTER_GET_FPS, &fps);
		} else if (stream->pixconv && ms_filter_has_method(stream->pixconv, MS_FILTER_GET_FPS)) {
			ms_filter_call_method(stream->pixconv, MS_FILTER_GET_FPS, &fps);
		}
	}
	return fps;
}

float video_stream_get_received_framerate(const VideoStream *stream) {
	float fps = 0;
	if (stream->ms.decoder != NULL && ms_filter_has_method(stream->ms.decoder, MS_FILTER_GET_FPS)) {
		ms_filter_call_method(stream->ms.decoder, MS_FILTER_GET_FPS, &fps);
	}
	return fps;
}

void video_stream_set_relay_session_id(VideoStream *stream, const char *id) {
	ms_filter_call_method(stream->ms.rtpsend, MS_RTP_SEND_SET_RELAY_SESSION_ID, (void *)id);
}

void video_stream_enable_retransmission_on_nack(VideoStream *stream, bool_t enable) {
	if (enable) {
		if (stream->nack_context) return;
		stream->nack_context = ortp_nack_context_new(stream->ms.evd);
	} else {
		if (stream->nack_context) ortp_nack_context_destroy(stream->nack_context);
		stream->nack_context = NULL;
	}
}

void video_stream_retransmission_on_nack_set_max_packet(VideoStream *stream, unsigned int max) {
	ortp_nack_context_set_max_packet(stream->nack_context, max);
}

void video_stream_enable_self_view(VideoStream *stream, bool_t val) {
	MSFilter *out = stream->output;
	stream->corner = val ? 0 : -1;
	if (out) {
		ms_filter_call_method(out, MS_VIDEO_DISPLAY_SET_LOCAL_VIEW_MODE, &stream->corner);
	}
}

void video_stream_set_render_callback(VideoStream *s, VideoStreamRenderCallback cb, void *user_pointer) {
	s->rendercb = cb;
	s->render_pointer = user_pointer;
}

void video_stream_set_display_callback(VideoStream *s, VideoStreamDisplayCallback cb, void *user_pointer) {
	s->displaycb = cb;
	s->display_pointer = user_pointer;
}

void video_stream_set_event_callback(VideoStream *s, VideoStreamEventCallback cb, void *user_pointer) {
	s->eventcb = cb;
	s->event_pointer = user_pointer;
}

void video_stream_set_camera_not_working_callback(VideoStream *s,
                                                  VideoStreamCameraNotWorkingCallback cb,
                                                  void *user_pointer) {
	s->cameracb = cb;
	s->camera_pointer = user_pointer;
}

void video_stream_set_display_filter_name(VideoStream *s, const char *fname) {
	if (s->display_name != NULL) {
		ms_free(s->display_name);
		s->display_name = NULL;
	}
	if (fname != NULL) s->display_name = ms_strdup(fname);
}

void video_stream_set_label(VideoStream *s, const char *label) {
	if (s->label != NULL) {
		ms_free(s->label);
		s->label = NULL;
	}
	if (label != NULL) s->label = ms_strdup(label);
}

void video_stream_set_content(VideoStream *s, MSVideoContent content) {
	s->content = content;
	if (s->content == MSVideoContentThumbnail && s->ms.bandwidth_controller) {
		ms_bandwidth_controller_elect_controlled_streams(s->ms.bandwidth_controller);
	}
}

MSVideoContent video_stream_get_content(const VideoStream *vs) {
	return vs->content;
}

static void ext_display_cb(void *ud, BCTBX_UNUSED(MSFilter *f), BCTBX_UNUSED(unsigned int event), void *eventdata) {
	MSExtDisplayOutput *output = (MSExtDisplayOutput *)eventdata;
	VideoStream *st = (VideoStream *)ud;
	if (st->rendercb != NULL) {
		st->rendercb(st->render_pointer, output->local_view.w != 0 ? &output->local_view : NULL,
		             output->remote_view.w != 0 ? &output->remote_view : NULL);
	}
}

void video_stream_set_direction(VideoStream *vs, MediaStreamDir dir) {
	media_stream_set_direction(&vs->ms, dir);
	vs->dir = dir;
}

// static MSVideoSize get_compatible_size(MSVideoSize maxsize, MSVideoSize wished_size){
// 	int max_area=maxsize.width*maxsize.height;
// 	int whished_area=wished_size.width*wished_size.height;
// 	if (whished_area>max_area){
// 		return maxsize;
// 	}
// 	return wished_size;
// }

#if !TARGET_IPHONE_SIMULATOR && !defined(MS_HAS_ARM) && !defined(MS2_NO_VIDEO_RESCALING)
static MSVideoSize get_with_same_orientation_and_ratio(MSVideoSize size, MSVideoSize refsize) {
	if (ms_video_size_get_orientation(refsize) != ms_video_size_get_orientation(size)) {
		int tmp;
		tmp = size.width;
		size.width = size.height;
		size.height = tmp;
	}
	size.height = (size.width * refsize.height) / refsize.width;
	return size;
}
#endif

static void configure_video_source(VideoStream *stream, bool_t skip_bitrate, bool_t source_changed) {
	MSVideoSize cam_vsize = {320, 240};
	MSVideoConfiguration vconf = {0};
	MSPixFmt format = MS_PIX_FMT_UNKNOWN;
	MSVideoEncoderPixFmt encoder_supports_source_format;
	int ret;
	MSVideoSize preview_vsize;
	MSPinFormat pf = {0};
	bool_t is_player =
	    stream->content != MSVideoContentThumbnail &&
	    (ms_filter_get_id(stream->source) == MS_ITC_SOURCE_ID || ms_filter_get_id(stream->source) == MS_MKV_PLAYER_ID);

	if (source_changed) {
		ms_filter_add_notify_callback(stream->source, event_cb, stream, FALSE);
		if (!is_player) ms_filter_add_notify_callback(stream->source, source_event_cb, stream, FALSE);
		/* It is important that the internal_event_cb is called synchronously! */
		ms_filter_add_notify_callback(stream->source, internal_event_cb, stream, TRUE);
	}

	/* transmit orientation to source filter */
	if (ms_filter_has_method(stream->source, MS_VIDEO_CAPTURE_SET_DEVICE_ORIENTATION))
		ms_filter_call_method(stream->source, MS_VIDEO_CAPTURE_SET_DEVICE_ORIENTATION, &stream->device_orientation);
	/* initialize the capture device orientation for preview */
	if (!stream->display_filter_auto_rotate_enabled &&
	    ms_filter_has_method(stream->source, MS_VIDEO_DISPLAY_SET_DEVICE_ORIENTATION))
		ms_filter_call_method(stream->source, MS_VIDEO_DISPLAY_SET_DEVICE_ORIENTATION, &stream->device_orientation);

	/* transmit its preview window id if any to source filter*/
	if (stream->preview_window_id != 0) {
		video_stream_set_native_preview_window_id(stream, stream->preview_window_id);
	}

	ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_GET_CONFIGURATION, &vconf);

	if (vconf.required_bitrate == 0) {
		vconf.required_bitrate = ms_factory_get_expected_bandwidth(stream->ms.factory);
		ms_message("Encoder current bitrate is 0, using expected bandwidth %i", vconf.required_bitrate);
		if (vconf.required_bitrate == 0) {
			MSVideoConfiguration *vconf_list = NULL;
			ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_GET_CONFIGURATION_LIST, &vconf_list);
			vconf = ms_video_find_best_configuration_for_size(vconf_list, vconf.vsize, stream->ms.factory->cpu_count);
		}
	}

	if (!ms_filter_implements_interface(stream->source, MSFilterVideoEncoderInterface)) {
		vconf.vsize = stream->sent_vsize; // get_compatible_size(vconf.vsize,stream->sent_vsize);
		if (stream->preview_vsize.width != 0) {
			preview_vsize = stream->preview_vsize;
		} else {
			preview_vsize = vconf.vsize;
		}
	}

	if (is_player) {
		ms_filter_call_method(stream->source, MS_FILTER_GET_OUTPUT_FMT, &pf);
		if (pf.fmt == NULL || pf.fmt->vsize.width == 0) {
			MSVideoSize vsize = {640, 480};
			ms_error("Player does not give its format correctly [%s]", ms_fmt_descriptor_to_string(pf.fmt));
			/*put a default format as the error handling is complicated here*/
			pf.fmt = ms_factory_get_video_format(stream->ms.factory, "VP8", vsize, 0, NULL);
		}
		cam_vsize = pf.fmt->vsize;
	} else {
		if (ms_filter_implements_interface(stream->source, MSFilterVideoEncoderInterface)) {
			// MS_FILTER_GET_VIDEO_SIZE and MS_FILTER_SET_VIDEO_SIZE are deprecated for MSFilterVideoEncoderInterface
			// types Use the size returned by the above call to MS_VIDEO_ENCODER_GET_CONFIGURATION directly
			cam_vsize = vconf.vsize;
		} else {
			vconf.vsize = preview_vsize;
			ms_filter_call_method(stream->source, MS_FILTER_SET_VIDEO_SIZE, &preview_vsize);
			/*the camera may not support the target size and suggest a one close to the target */
			ms_filter_call_method(stream->source, MS_FILTER_GET_VIDEO_SIZE, &cam_vsize);
		}
	}

	if (cam_vsize.width * cam_vsize.height <= vconf.vsize.width * vconf.vsize.height) {
		vconf.vsize = cam_vsize;
		ms_message("Output video size adjusted to match camera resolution (%ix%i)", vconf.vsize.width,
		           vconf.vsize.height);
	} else if (stream->content != MSVideoContentThumbnail) {
#if TARGET_IPHONE_SIMULATOR || defined(MS_HAS_ARM) || defined(MS2_WINDOWS_UNIVERSAL) || defined(MS2_NO_VIDEO_RESCALING)
		ms_error("Camera is proposing a size bigger than encoder's suggested size (%ix%i > %ix%i) "
		         "Using the camera size as fallback because cropping or resizing is not implemented for this device.",
		         cam_vsize.width, cam_vsize.height, vconf.vsize.width, vconf.vsize.height);
		vconf.vsize = cam_vsize;
#else
		if (ms_video_get_scaler_impl() == NULL) {
			vconf.vsize = cam_vsize;
		} else {
			/* Libyuv can resize the video itself */
#if !defined(HAVE_LIBYUV_H)
			MSVideoSize resized = get_with_same_orientation_and_ratio(vconf.vsize, cam_vsize);
			if (resized.width & 0x1 || resized.height & 0x1) {
				ms_warning("Resizing avoided because downsizing to an odd number of pixels (%ix%i)", resized.width,
				           resized.height);
				vconf.vsize = cam_vsize;
			} else {
				vconf.vsize = resized;
				ms_warning("Camera video size greater than encoder one. A scaling filter will be used!");
			}
#endif
		}
#endif
	}

	if (!skip_bitrate && stream->ms.target_bitrate > 0) {
		vconf.required_bitrate = stream->ms.target_bitrate;
	}

	if (is_player) {
		vconf.fps = pf.fmt->fps;
		if (vconf.fps == 0) vconf.fps = 15;
	} else {
		if (stream->forced_fps != 0) vconf.fps = stream->forced_fps;
		ms_message("Setting sent vsize=%ix%i, fps=%f", vconf.vsize.width, vconf.vsize.height, vconf.fps);
		/* configure the filters */
		if (ms_filter_get_id(stream->source) != MS_STATIC_IMAGE_ID || !stream->staticimage_webcam_fps_optimization) {
			ms_filter_call_method(stream->source, MS_FILTER_SET_FPS, &vconf.fps);
		}
		/* get the output format for webcam reader */
		ms_filter_call_method(stream->source, MS_FILTER_GET_PIX_FMT, &format);
	}
	stream->configured_fps = vconf.fps;

	if (stream->ms.encoder) ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_SET_CONFIGURATION, &vconf);

	encoder_supports_source_format.supported = FALSE;
	encoder_supports_source_format.pixfmt = format;

	if (stream->ms.encoder && ms_filter_has_method(stream->ms.encoder, MS_VIDEO_ENCODER_SUPPORTS_PIXFMT) == TRUE) {
		ret = ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_SUPPORTS_PIXFMT,
		                            &encoder_supports_source_format);
	} else {
		ret = -1;
	}
	if (ret == -1) {
		/*encoder doesn't have MS_VIDEO_ENCODER_SUPPORTS_PIXFMT method*/
		/*we prefer in this case consider that it is not required to get the optimization of not going through pixconv
		 * and sizeconv*/
		encoder_supports_source_format.supported = FALSE;
	}

	if ((encoder_supports_source_format.supported == TRUE) || (stream->source_performs_encoding == TRUE)) {
		ms_filter_call_method(stream->ms.encoder, MS_FILTER_SET_PIX_FMT, &format);
	} else {
		if (stream->content == MSVideoContentThumbnail) {
			stream->sizeconv = ms_factory_create_filter(stream->ms.factory, MS_SIZE_CONV_ID);
			ms_filter_call_method(stream->sizeconv, MS_FILTER_SET_VIDEO_SIZE, &vconf.vsize);
			ms_filter_add_notify_callback(stream->sizeconv, source_event_cb, stream, FALSE);
			if (ms_filter_get_id(stream->source) != MS_ITC_SOURCE_ID) {
			}
		} else {
#ifndef MS2_NO_VIDEO_RESCALING
			if (ms_video_get_scaler_impl() != NULL) {
				stream->sizeconv = ms_factory_create_filter(stream->ms.factory, MS_SIZE_CONV_ID);
				ms_filter_call_method(stream->sizeconv, MS_FILTER_SET_VIDEO_SIZE, &vconf.vsize);
			}
#endif
		}
		if (stream->content != MSVideoContentThumbnail || ms_filter_get_id(stream->source) != MS_ITC_SOURCE_ID) {
			if (format == MS_MJPEG) {
				stream->pixconv = ms_factory_create_filter(stream->ms.factory, MS_MJPEG_DEC_ID);
				if (stream->pixconv == NULL) {
					ms_error("Could not create mjpeg decoder, check your build options.");
				}
			} else if (format == MS_PIX_FMT_UNKNOWN && pf.fmt != NULL) {
				if (!stream->is_forwarding)
					stream->pixconv = ms_factory_create_decoder(stream->ms.factory, pf.fmt->encoding);
			} else {
				stream->pixconv = ms_factory_create_filter(stream->ms.factory, MS_PIX_CONV_ID);
				/*set it to the pixconv */
				ms_filter_call_method(stream->pixconv, MS_FILTER_SET_PIX_FMT, &format);
				ms_filter_call_method(stream->pixconv, MS_FILTER_SET_VIDEO_SIZE, &cam_vsize);
			}
		}
	}
	if (stream->ms.rc) {
		ms_bitrate_controller_destroy(stream->ms.rc);
		stream->ms.rc = NULL;
	}
	if (stream->ms.rc_enable && stream->ms.encoder) {
		switch (stream->ms.rc_algorithm) {
			case MSQosAnalyzerAlgorithmSimple:
				stream->ms.rc =
				    ms_av_bitrate_controller_new(NULL, NULL, stream->ms.sessions.rtp_session, stream->ms.encoder);
				break;
			case MSQosAnalyzerAlgorithmStateful:
				stream->ms.rc = ms_bandwidth_bitrate_controller_new(NULL, NULL, stream->ms.sessions.rtp_session,
				                                                    stream->ms.encoder);
				break;
		}
	}
}

static void configure_recorder_output(VideoStream *stream) {
	if (stream->recorder_output) {
		configure_sink(stream, stream->recorder_output);
	}
}

static void configure_decoder(VideoStream *stream, PayloadType *pt) {
	bool_t avpf_enabled = !!(pt->flags & PAYLOAD_TYPE_RTCP_FEEDBACK_ENABLED);
	ms_filter_call_method(stream->ms.decoder, MS_VIDEO_DECODER_ENABLE_AVPF, &avpf_enabled);
	ms_filter_call_method(stream->ms.decoder, MS_VIDEO_DECODER_FREEZE_ON_ERROR, &stream->freeze_on_error);

	if (stream->content == MSVideoContentThumbnail) {
		/* don't let the decoder spawn multiple threads to decode thumbnail video, it is inefficient. */
		int max_threads = 1;
		ms_filter_call_method(stream->ms.decoder, MS_VIDEO_DECODER_SET_MAX_THREADS, &max_threads);
	}
	ms_filter_add_notify_callback(stream->ms.decoder, event_cb, stream, FALSE);
	/* It is important that the internal_event_cb is called synchronously! */
	ms_filter_add_notify_callback(stream->ms.decoder, internal_event_cb, stream, TRUE);
}

#ifdef QRCODE_ENABLED
static void configure_qrcode_filter(VideoStream *stream) {
	ms_filter_add_notify_callback(stream->qrcode, event_cb, stream, FALSE);
}
#endif

static void video_stream_payload_type_changed(RtpSession *session,
                                              void *data,
                                              BCTBX_UNUSED(void *unused1),
                                              BCTBX_UNUSED(void *unused2)) {
	VideoStream *stream = (VideoStream *)data;
	RtpProfile *prof = rtp_session_get_profile(session);
	int payload = rtp_session_get_recv_payload_type(session);
	PayloadType *pt = rtp_profile_get_payload(prof, payload);

	if (stream->ms.decoder == NULL) {
		ms_message("video_stream_payload_type_changed(): no decoder!");
		return;
	}

	if (pt != NULL) {
		MSFilter *dec;

		/* Q: why only video ? A: because an audio format can be used at different rates: ex: speex/16000 speex/8000*/
		if ((stream->ms.decoder != NULL) && (stream->ms.decoder->desc->enc_fmt != NULL) &&
		    (strcasecmp(pt->mime_type, stream->ms.decoder->desc->enc_fmt) == 0)) {
			/* Same formats behind different numbers, nothing to do. */
			return;
		}

		//		dec = ms_filter_create_decoder(pt->mime_type);
		dec = ms_factory_create_decoder(stream->ms.factory, pt->mime_type);
		if (dec != NULL) {
			MSFilter *prevFilter = stream->ms.decoder->inputs[0]->prev.filter;
			MSFilter *nextFilter = stream->ms.decoder->outputs[0]->next.filter;

			ms_filter_unlink(prevFilter, 0, stream->ms.decoder, 0);
			ms_filter_unlink(stream->ms.decoder, 0, nextFilter, 0);
			ms_filter_postprocess(stream->ms.decoder);
			ms_filter_destroy(stream->ms.decoder);
			stream->ms.decoder = dec;
			if (pt->recv_fmtp != NULL)
				ms_filter_call_method(stream->ms.decoder, MS_FILTER_ADD_FMTP, (void *)pt->recv_fmtp);
			ms_filter_link(prevFilter, 0, stream->ms.decoder, 0);
			ms_filter_link(stream->ms.decoder, 0, nextFilter, 0);
			ms_filter_preprocess(stream->ms.decoder, stream->ms.sessions.ticker);

			configure_decoder(stream, pt);
		} else {
			ms_warning("No decoder found for %s", pt->mime_type);
		}
	} else {
		ms_warning("No payload defined with number %i", payload);
	}

	configure_recorder_output(stream);
	configure_sink(stream, stream->forward_sink);
}

int video_stream_start(VideoStream *stream,
                       RtpProfile *profile,
                       const char *rem_rtp_ip,
                       int rem_rtp_port,
                       const char *rem_rtcp_ip,
                       int rem_rtcp_port,
                       int payload,
                       int jitt_comp,
                       MSWebCam *cam) {
	MSMediaStreamIO io = MS_MEDIA_STREAM_IO_INITIALIZER;
	if (cam == NULL) {
		cam = ms_web_cam_manager_get_default_cam(ms_factory_get_web_cam_manager(stream->ms.factory));
	}
	io.input.type = MSResourceCamera;
	io.input.camera = cam;
	io.output.type = MSResourceDefault;
	io.output.resource_arg = NULL;
	rtp_session_set_jitter_compensation(stream->ms.sessions.rtp_session, jitt_comp);
	return video_stream_start_from_io(stream, profile, rem_rtp_ip, rem_rtp_port, rem_rtcp_ip, rem_rtcp_port, payload,
	                                  &io);
}

void video_recorder_handle_event(void *userdata,
                                 BCTBX_UNUSED(MSFilter *f),
                                 unsigned int event,
                                 BCTBX_UNUSED(void *event_args)) {
	VideoStream *stream = (VideoStream *)userdata;
	switch (event) {
		case MS_RECORDER_NEEDS_FIR:
			ms_message("Request sending of FIR on videostream [%p]", stream);
			video_stream_send_fir(stream);
			break;
		default:
			break;
	}
}

int video_stream_start_from_io(VideoStream *stream,
                               RtpProfile *profile,
                               const char *rem_rtp_ip,
                               int rem_rtp_port,
                               const char *rem_rtcp_ip,
                               int rem_rtcp_port,
                               int payload_type,
                               const MSMediaStreamIO *io) {
	MSWebCam *cam = NULL;
	MSFilter *source = NULL;
	MSFilter *output = NULL;
	MSFilter *recorder = NULL;

	if (stream->ms.state != MSStreamInitialized) {
		ms_error("VideoStream in bad state");
		return -1;
	}

	if (!ms_media_stream_io_is_consistent(io)) return -1;

	if (media_stream_get_direction(&stream->ms) != MediaStreamRecvOnly) {
		switch (io->input.type) {
			case MSResourceRtp:
				stream->rtp_io_session = io->input.session;
				source = ms_factory_create_filter(stream->ms.factory, MS_RTP_RECV_ID);
				ms_filter_call_method(source, MS_RTP_RECV_SET_SESSION, stream->rtp_io_session);
				break;
			case MSResourceCamera:
				cam = io->input.camera;
				source = ms_web_cam_create_reader(cam);
				break;
			case MSResourceFile:
				source = ms_factory_create_filter(stream->ms.factory, MS_MKV_PLAYER_ID);
				if (!source) {
					ms_error("Mediastreamer2 library compiled without libmastroska2");
					return -1;
				}
				stream->source = source;
				if (io->input.file) {
					if (video_stream_open_remote_play(stream, io->input.file) != NULL)
						ms_filter_call_method_noarg(source, MS_PLAYER_START);
				}
				break;
			case MSResourceVoid:
				stream->source = ms_factory_create_filter(stream->ms.factory, MS_VOID_SOURCE_ID);
				break;
			case MSResourceItc:
				stream->source = ms_factory_create_filter(stream->ms.factory, MS_ITC_SOURCE_ID);
				if (io->input.itc) {
					ms_filter_call_method(io->input.itc, MS_ITC_SINK_CONNECT, stream->source);
				}
				break;
			case MSResourceScreenSharing:
				source = ms_factory_create_filter(stream->ms.factory, MS_SCREEN_SHARING_ID);
				ms_filter_call_method(source, MS_SCREEN_SHARING_SET_SOURCE_DESCRIPTOR,
				                      (void *)&io->input.screen_sharing);
				break;
			default:
				ms_error("Unhandled input resource type %s", ms_resource_type_to_string(io->input.type));
				break;
		}
	}
	if (media_stream_get_direction(&stream->ms) != MediaStreamSendOnly) {
		switch (io->output.type) {
			case MSResourceRtp:
				output = ms_factory_create_filter(stream->ms.factory, MS_RTP_SEND_ID);
				stream->rtp_io_session = io->input.session;
				ms_filter_call_method(output, MS_RTP_SEND_SET_SESSION, stream->rtp_io_session);
				break;
			case MSResourceFile:
				recorder = ms_factory_create_filter(stream->ms.factory, MS_MKV_RECORDER_ID);
				if (!recorder) {
					ms_error("Mediastreamer2 library compiled without libmastroska2");
					return -1;
				}
				if (stream->recorder_output) {
					ms_filter_destroy(stream->recorder_output);
				}
				stream->recorder_output = recorder;
				ms_filter_add_notify_callback(recorder, video_recorder_handle_event, stream, TRUE);
				if (io->output.file) video_stream_open_remote_record(stream, io->output.file);
				break;
			case MSResourceVoid:
				output = ms_factory_create_filter(stream->ms.factory, MS_VOID_SINK_ID);
				break;
			default:
				/*will just display in all other cases*/
				/*ms_error("Unhandled output resource type %s", ms_resource_type_to_string(io->output.type));*/
				break;
		}
	}

	return video_stream_start_with_source_and_output(stream, profile, rem_rtp_ip, rem_rtp_port, rem_rtcp_ip,
	                                                 rem_rtcp_port, payload_type, -1, cam, source, output);
}

void link_video_stream_with_itc_sink(VideoStream *stream) {
	if (!stream->itcsink) stream->itcsink = ms_factory_create_filter(stream->ms.factory, MS_ITC_SINK_ID);
	if (stream->tee) {
		ms_filter_link(stream->tee, 3, stream->itcsink, 0);
	}
}

bool_t video_stream_started(VideoStream *stream) {
	return media_stream_started(&stream->ms);
}

static void apply_video_preset(VideoStream *stream, PayloadType *pt) {
	MSVideoPresetsManager *vpm = ms_factory_get_video_presets_manager(stream->ms.factory);
	MSVideoPresetConfiguration *vpc = NULL;
	MSVideoConfiguration *conf = NULL;
	bctbx_list_t *codec_tags = NULL;
	bool_t hardware_accelerated = FALSE;
	if (stream->preset != NULL) {
		codec_tags = bctbx_list_append(codec_tags, ms_strdup(payload_type_get_mime(pt)));
		codec_tags = bctbx_list_append(codec_tags, ms_strdup(stream->ms.encoder->desc->name));
		if (ms_filter_has_method(stream->ms.encoder, MS_VIDEO_ENCODER_IS_HARDWARE_ACCELERATED) == TRUE) {
			ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_IS_HARDWARE_ACCELERATED, &hardware_accelerated);
		}
		if (hardware_accelerated == TRUE) {
			codec_tags = bctbx_list_append(codec_tags, ms_strdup("hardware"));
		}
		vpc = ms_video_presets_manager_find_preset_configuration(vpm, stream->preset, codec_tags);
		bctbx_list_for_each(codec_tags, ms_free);
		bctbx_list_free(codec_tags);
		if (vpc != NULL) {
			char *conf_tags = ms_video_preset_configuration_get_tags_as_string(vpc);
			conf = ms_video_preset_configuration_get_video_configuration(vpc);
			if (conf_tags) {
				ms_message("Using the '%s' video preset tagged '%s'", stream->preset, conf_tags);
				ms_free(conf_tags);
			} else {
				ms_message("Using the '%s' video preset non-tagged", stream->preset);
			}
		} else {
			ms_warning("No '%s' video preset has been found", stream->preset);
		}
	}
	if (conf == NULL) {
		ms_message("Using the default video configuration list");
		if (ms_filter_has_method(stream->ms.encoder, MS_VIDEO_ENCODER_GET_CONFIGURATION_LIST) == TRUE) {
			ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_GET_CONFIGURATION_LIST, &conf);
		}
	}
	stream->vconf_list = conf;
}

static void apply_bitrate_limit(VideoStream *stream, PayloadType *pt) {
	int target_upload_bandwidth = 0;

	if (stream->ms.target_bitrate > 0) {
		ms_message("Current target bitrate is set to [%i] bit/s.", stream->ms.target_bitrate);
		target_upload_bandwidth = stream->ms.target_bitrate;
	} else if (pt->normal_bitrate <= 0 && stream->ms.max_target_bitrate <= 0) {
		ms_message("target and payload bitrates not set for stream [%p] using lowest configuration of preferred video "
		           "size %dx%d",
		           stream, stream->sent_vsize.width, stream->sent_vsize.height);
	} else if (stream->ms.max_target_bitrate <= 0) {
		ms_message("Max target bitrate not set for stream [%p], but using payload type's bitrate [%i]", stream,
		           stream->ms.max_target_bitrate);
		target_upload_bandwidth = stream->ms.max_target_bitrate = pt->normal_bitrate;
	} else {
		ms_message("Using max target bitrate [%i] bit/s", stream->ms.max_target_bitrate);
		target_upload_bandwidth = stream->ms.max_target_bitrate;
	}

	if (stream->vconf_list != NULL) {
		MSVideoConfiguration vconf;

		if (target_upload_bandwidth > 0) {
			vconf = ms_video_find_best_configuration_for_bitrate(stream->vconf_list, target_upload_bandwidth,
			                                                     ms_factory_get_cpu_count(stream->ms.factory));
			/* Adjust configuration video size to use the user preferred video size if it is lower that the
			 * configuration one. */
			if ((stream->sent_vsize.height * stream->sent_vsize.width) < (vconf.vsize.height * vconf.vsize.width)) {
				vconf.vsize = stream->sent_vsize;
			}
		} else {
			/* We retrieve the lowest configuration for that vsize since the bandwidth estimator will increase quality
			 * if possible */
			vconf = ms_video_find_worst_configuration_for_size(stream->vconf_list, stream->sent_vsize,
			                                                   ms_factory_get_cpu_count(stream->ms.factory));
			/*If the lower config is found, required_bitrate will be 0. In this case, use the bitrate_limit*/
			vconf.required_bitrate = target_upload_bandwidth =
			    vconf.required_bitrate > 0 ? vconf.required_bitrate : vconf.bitrate_limit;
		}
		ms_message("Limiting bitrate of video encoder to %i bits/s for stream [%p]", target_upload_bandwidth, stream);
		ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_SET_CONFIGURATION, &vconf);
	} else {
		MSVideoConfiguration vconf;
		/* Get current video configuration and change only bitrate */
		ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_GET_CONFIGURATION, &vconf);

		vconf.required_bitrate = target_upload_bandwidth;
		ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_SET_CONFIGURATION, &vconf);
	}
	rtp_session_set_target_upload_bandwidth(stream->ms.sessions.rtp_session, target_upload_bandwidth);
}

static MSPixFmt mime_type_to_pix_format(const char *mime_type) {
	if (strcasecmp(mime_type, "H264") == 0) return MS_H264;
	return MS_PIX_FMT_UNKNOWN;
}

static void csrc_event_cb(void *ud, BCTBX_UNUSED(MSFilter *f), unsigned int event, void *eventdata) {
	VideoStream *stream = (VideoStream *)ud;

	switch (event) {
		case MS_RTP_RECV_CSRC_CHANGED:
			stream->new_csrc = *((uint32_t *)eventdata);

			if (stream->new_csrc == 0) {
				if (stream->wait_for_frame_decoded) stream->wait_for_frame_decoded = FALSE;
				stream->csrc_changed_cb(stream->csrc_changed_cb_user_data, 0);
			} else {
				bool_t reset = TRUE;
				ms_filter_call_method(stream->ms.decoder, MS_VIDEO_DECODER_RESET_FIRST_IMAGE_NOTIFICATION, &reset);
				stream->wait_for_frame_decoded = TRUE;
			}

			stream->csrc_change_received = TRUE;

			break;
		case MS_VIDEO_AGGREGATOR_INPUT_CHANGED:
			// When the video is in mixer mode, we will only receive events via the CSRC.
			// But the video aggregator will still notify the first input changed. Ignore it.
			if (stream->csrc_change_received) return;

			const int input = *((int *)eventdata);
			if (input == 0) {
				// 0 is the default rtp session of the stream
				stream->new_csrc = rtp_session_get_recv_ssrc(stream->ms.sessions.rtp_session);
			} else {
				stream->new_csrc = rtp_session_get_recv_ssrc(stream->branches[input - 1].session);
			}

			if (stream->new_csrc == 0) {
				if (stream->wait_for_frame_decoded) stream->wait_for_frame_decoded = FALSE;
				stream->csrc_changed_cb(stream->csrc_changed_cb_user_data, 0);
			} else {
				bool_t reset = TRUE;
				ms_filter_call_method(stream->ms.decoder, MS_VIDEO_DECODER_RESET_FIRST_IMAGE_NOTIFICATION, &reset);
				stream->wait_for_frame_decoded = TRUE;
			}

			break;
		case MS_VIDEO_DECODER_FIRST_IMAGE_DECODED:
			if (stream->wait_for_frame_decoded) {
				stream->csrc_changed_cb(stream->csrc_changed_cb_user_data, stream->new_csrc);
				stream->wait_for_frame_decoded = FALSE;
			}
			break;
		default:
			break;
	}
}

static int video_stream_start_with_source_and_output(VideoStream *stream,
                                                     RtpProfile *profile,
                                                     const char *rem_rtp_ip,
                                                     int rem_rtp_port,
                                                     const char *rem_rtcp_ip,
                                                     int rem_rtcp_port,
                                                     int payload,
                                                     int jitt_comp,
                                                     MSWebCam *cam,
                                                     MSFilter *source,
                                                     MSFilter *output) {
	PayloadType *pt;
	RtpSession *rtps = stream->ms.sessions.rtp_session;
	MSPixFmt format;
	MSVideoSize disp_size;
	JBParameters jbp;

	bool_t avpf_enabled = FALSE;
	bool_t rtp_source = FALSE;
	bool_t rtp_output = FALSE;
	bool_t do_ts_adjustments;

	if (source == NULL) {
		source = stream->source;
	}
	rtp_source = (source && ms_filter_get_id(source) == MS_RTP_RECV_ID) ? TRUE : FALSE;
	do_ts_adjustments = !rtp_source;

	pt = rtp_profile_get_payload(profile, payload);
	if (pt == NULL) {
		ms_error("videostream.c: undefined payload type %d.", payload);
		return -1;
	}
	if (pt->flags & PAYLOAD_TYPE_RTCP_FEEDBACK_ENABLED) avpf_enabled = TRUE;

	if ((cam != NULL) && (cam->desc->encode_to_mime_type != NULL) &&
	    (cam->desc->encode_to_mime_type(cam, pt->mime_type) == TRUE)) {
		stream->source_performs_encoding = TRUE;
	}

	rtp_session_set_profile(rtps, profile);
	if (rem_rtp_port > 0) rtp_session_set_remote_addr_full(rtps, rem_rtp_ip, rem_rtp_port, rem_rtcp_ip, rem_rtcp_port);
	if (rem_rtcp_port > 0) {
		rtp_session_enable_rtcp(rtps, TRUE);
	} else {
		rtp_session_enable_rtcp(rtps, FALSE);
	}
	rtp_session_set_payload_type(rtps, payload);
	if (jitt_comp != -1) {
		/*jitt_comp = -1 don't change value. The application can use rtp_session_set_jitter_buffer_params() directly.*/
		rtp_session_set_jitter_compensation(rtps, jitt_comp);
	}

	rtp_session_signal_connect(stream->ms.sessions.rtp_session, "payload_type_changed",
	                           (RtpCallback)video_stream_payload_type_changed, &stream->ms);

	rtp_session_get_jitter_buffer_params(stream->ms.sessions.rtp_session, &jbp);
	jbp.max_packets = 1000; // needed for high resolution video
	rtp_session_set_jitter_buffer_params(stream->ms.sessions.rtp_session, &jbp);

	media_stream_create_or_update_fec_session(&stream->ms);

	/* Plumb the outgoing stream */
	if (rem_rtp_port > 0)
		ms_filter_call_method(stream->ms.rtpsend, MS_RTP_SEND_SET_SESSION, stream->ms.sessions.rtp_session);
	ms_filter_call_method(stream->ms.rtpsend, MS_RTP_SEND_ENABLE_TS_ADJUSTMENT, &do_ts_adjustments);

	if (stream->frame_marking_extension_id > 0) {
		ms_filter_call_method(stream->ms.rtpsend, MS_RTP_SEND_SET_FRAME_MARKING_EXTENSION_ID,
		                      &stream->frame_marking_extension_id);
	}

	if (media_stream_get_direction(&stream->ms) == MediaStreamRecvOnly) {
		/* Create a dummy sending stream to send the STUN packets to open firewall ports. */
		MSConnectionHelper ch;
		bool_t send_silence = FALSE;
		stream->void_source = ms_factory_create_filter(stream->ms.factory, MS_VOID_SOURCE_ID);
		ms_filter_call_method(stream->void_source, MS_VOID_SOURCE_SEND_SILENCE, &send_silence);
		ms_connection_helper_start(&ch);
		ms_connection_helper_link(&ch, stream->void_source, -1, 0);
		ms_connection_helper_link(&ch, stream->ms.rtpsend, 0, -1);
	} else {
		MSConnectionHelper ch;
		if (stream->source_performs_encoding == TRUE) {
			format = mime_type_to_pix_format(pt->mime_type);
			ms_filter_call_method(source, MS_FILTER_SET_PIX_FMT, &format);
		} else if (!rtp_source) {
			stream->ms.encoder = ms_factory_create_encoder(stream->ms.factory, pt->mime_type);
			if (stream->ms.encoder == NULL) {
				/* big problem: we don't have a registered codec for this payload...*/
				if (stream->fallback_to_dummy_codec == TRUE) {
					ms_error("videostream.c: No encoder available for payload %i:%s. Try to create a dummy one",
					         payload, pt->mime_type);
					stream->ms.encoder = ms_factory_create_filter(stream->ms.factory, MS_DUMMY_ENC_ID);
					if (stream->ms.encoder == NULL) {
						ms_error("videostream.c: No encoder available for dummy codec");
						if (source) ms_filter_destroy(source);
						return -1;
					}
				} else {
					ms_error("videostream.c: No encoder available for payload %i:%s.", payload, pt->mime_type);
					if (source) ms_filter_destroy(source);
					return -1;
				}
			}
		}
		/* creates the filters */
		stream->cam = cam;
		stream->source = source;
		if (rtp_source) {
			stream->ms.encoder = stream->source; /* Consider that the source is also the encoder */
		} else {
			stream->tee = ms_factory_create_filter(stream->ms.factory, MS_TEE_ID);
			stream->local_jpegwriter = ms_factory_create_filter(stream->ms.factory, MS_JPEG_WRITER_ID);
			if (stream->source_performs_encoding == TRUE) {
				stream->ms.encoder = stream->source; /* Consider the encoder is the source */
			}

			apply_video_preset(stream, pt);
			apply_bitrate_limit(stream, pt);

			if (pt->send_fmtp) {
				ms_filter_call_method(stream->ms.encoder, MS_FILTER_ADD_FMTP, pt->send_fmtp);
			}
			ms_filter_call_method(stream->ms.encoder, MS_VIDEO_ENCODER_ENABLE_AVPF, &avpf_enabled);
			if (stream->use_preview_window) {
				if (stream->rendercb == NULL) {
					stream->output2 = ms_factory_create_filter_from_name(stream->ms.factory, stream->display_name);
					if (stream->output2) ms_filter_add_notify_callback(stream->output2, display_cb, stream, FALSE);
				}
			}
			configure_video_source(stream, FALSE, TRUE);
		}

		if (stream->active_speaker_mode == TRUE) {
			RtpBundle *bundle = stream->ms.sessions.rtp_session->bundle;
			if (bundle) {
				// Use rtp_session_signal_connect_from_source_session so that the bundle can disconnect it if this
				// session is removed too early.
				rtp_session_signal_connect_from_source_session(
				    rtp_bundle_get_primary_session(bundle), "new_incoming_ssrc_found_in_bundle",
				    on_incoming_ssrc_in_bundle, stream, stream->ms.sessions.rtp_session);
			}
		}

		if (stream->ms.transfer_mode == TRUE) {
			rtp_session_set_mode(stream->ms.sessions.rtp_session, RTP_SESSION_RECVONLY);
			RtpBundle *bundle = stream->ms.sessions.rtp_session->bundle;
			if (bundle) {
				// Use rtp_session_signal_connect_from_source_session so that the bundle can disconnect it if this
				// session is removed too early.
				rtp_session_signal_connect_from_source_session(
				    rtp_bundle_get_primary_session(bundle), "new_outgoing_ssrc_found_in_bundle",
				    media_stream_on_outgoing_ssrc_in_bundle, &stream->ms, stream->ms.sessions.rtp_session);
			}
		}

		/* and then connect all */
		ms_connection_helper_start(&ch);
		ms_connection_helper_link(&ch, stream->source, -1, 0);
		/* Note: if pixconv is not null then it is needed. For example, Thumbnail can come directly from camera and
		 * not from itc*/
		if (stream->pixconv) {
			ms_connection_helper_link(&ch, stream->pixconv, 0, 0);
		}
		if (stream->tee) {
			ms_connection_helper_link(&ch, stream->tee, 0, 0);
		}
		if (stream->itcsink) {
			ms_filter_link(stream->tee, 3, stream->itcsink, 0);
		}
		if (stream->sizeconv) {
			ms_connection_helper_link(&ch, stream->sizeconv, 0, 0);
		}
		if ((stream->source_performs_encoding == FALSE) && !rtp_source) {
			ms_connection_helper_link(&ch, stream->ms.encoder, 0, 0);
		}
		ms_connection_helper_link(&ch, stream->ms.rtpsend, 0, -1);
		if (stream->output2) {
			if (stream->preview_window_id != 0) {
				ms_filter_call_method(stream->output2, MS_VIDEO_DISPLAY_SET_NATIVE_WINDOW_ID,
				                      &stream->preview_window_id);
			}
			if (ms_filter_implements_interface(stream->output2, MSFilterVideoDisplayInterface)) {
				assign_value_to_mirroring_flag_to_preview(stream);
			}
			if (ms_filter_has_method(stream->output2, MS_VIDEO_DISPLAY_SET_MODE)) {
				ms_message("Video stream[%p] thumbnail[%d] direction[%d]: set display mode %d to filter %s", stream,
				           stream->content == MSVideoContentThumbnail ? 1 : 0, media_stream_get_direction(&stream->ms),
				           stream->preview_display_mode, ms_filter_get_name(stream->output2));
				ms_filter_call_method(stream->output2, MS_VIDEO_DISPLAY_SET_MODE, &stream->preview_display_mode);
			}

			ms_filter_link(stream->tee, 1, stream->output2, 0);
		}
		if (stream->local_jpegwriter) {
			ms_filter_link(stream->tee, 2, stream->local_jpegwriter, 0);
		}
	}

	/* Plumb the incoming stream */
	if (output != NULL) {
		rtp_output = (ms_filter_get_id(output) == MS_RTP_SEND_ID) ? TRUE : FALSE;
	}

	if (media_stream_get_direction(&stream->ms) == MediaStreamSendRecv ||
	    media_stream_get_direction(&stream->ms) == MediaStreamRecvOnly) {
		MSConnectionHelper ch;

		if (!rtp_output) {
			/* create decoder first */
			stream->ms.decoder = ms_factory_create_decoder(stream->ms.factory, pt->mime_type);
			if (stream->ms.decoder == NULL) {
				/* big problem: we don't have a registered decoderfor this payload...*/
				if (stream->fallback_to_dummy_codec == TRUE) {
					ms_error("videostream.c: No decoder available for payload %i:%s. Try to create a dummy one",
					         payload, pt->mime_type);
					stream->ms.decoder = ms_factory_create_filter(stream->ms.factory, MS_DUMMY_DEC_ID);
					if (stream->ms.decoder == NULL) {
						ms_error("videostream.c: No decoder available for dummy codec");
						return -1;
					}
				} else {
					ms_error("videostream.c: No decoder available for payload %i:%s.", payload, pt->mime_type);
					return -1;
				}
			}
		}

		/* display logic */
		if (stream->rendercb != NULL) {
			/* rendering logic delegated to user supplied callback */
			stream->output = ms_factory_create_filter(stream->ms.factory, MS_EXT_DISPLAY_ID);
			ms_filter_add_notify_callback(stream->output, ext_display_cb, stream, TRUE);
			ms_filter_add_notify_callback(stream->output, display_cb, stream, FALSE);
		} else {
			/* no user supplied callback -> create filter */
			MSVideoDisplayDecodingSupport decoding_support;

			if (((output == NULL) || (ms_filter_get_id(output) != MS_RTP_SEND_ID)) &&
			    ms_filter_has_method(stream->ms.decoder, MS_VIDEO_DECODER_SUPPORT_RENDERING)) {
				/* Check if the decoding filter can perform the rendering */
				decoding_support.mime_type = pt->mime_type;
				decoding_support.supported = FALSE;
				ms_filter_call_method(stream->ms.decoder, MS_VIDEO_DECODER_SUPPORT_RENDERING, &decoding_support);
				stream->output_performs_decoding = decoding_support.supported;
			}

			if (stream->output_performs_decoding) {
				stream->output = stream->ms.decoder;
			} else if (output != NULL) {
				stream->output = output;
				if (rtp_output) {
					stream->ms.decoder = stream->output;
				}
			} else {
				/* Create default display filter */
				stream->output = ms_factory_create_filter_from_name(stream->ms.factory, stream->display_name);
				if (stream->output) ms_filter_add_notify_callback(stream->output, display_cb, stream, FALSE);
			}
		}

		/* Don't allow null output */
		if (stream->output == NULL) {
			ms_fatal("No video display filter could be instantiated. Please check build-time configuration. "
			         "display_name: %s",
			         stream->display_name);
		}

		stream->ms.rtprecv = ms_factory_create_filter(stream->ms.factory, MS_RTP_RECV_ID);
		ms_filter_call_method(stream->ms.rtprecv, MS_RTP_RECV_SET_SESSION, stream->ms.sessions.rtp_session);

		if (stream->csrc_changed_cb) {
			bool_t enable = TRUE;
			ms_filter_call_method(stream->ms.rtprecv, MS_RTP_RECV_ENABLE_CSRC_EVENTS, &enable);
			ms_filter_add_notify_callback(stream->ms.rtprecv, csrc_event_cb, stream, FALSE);
			ms_filter_add_notify_callback(stream->ms.decoder, csrc_event_cb, stream, FALSE);
		}

		if (stream->frame_marking_extension_id > 0) {
			ms_filter_call_method(stream->ms.rtprecv, MS_RTP_RECV_SET_FRAME_MARKING_EXTENSION_ID,
			                      &stream->frame_marking_extension_id);
		}

		if (stream->active_speaker_mode == TRUE && stream->frame_marking_extension_id > 0) {
			stream->aggregator = ms_factory_create_filter(stream->ms.factory, MS_VIDEO_AGGREGATOR_ID);
			if (stream->csrc_changed_cb) {
				ms_filter_add_notify_callback(stream->aggregator, csrc_event_cb, stream, TRUE);
			}

			for (int i = 0; i < VIDEO_STREAM_MAX_BRANCHES; i++) {
				stream->branches[i].recv = ms_factory_create_filter(stream->ms.factory, MS_RTP_RECV_ID);
				ms_filter_call_method(stream->branches[i].recv, MS_RTP_RECV_SET_FRAME_MARKING_EXTENSION_ID,
				                      &stream->frame_marking_extension_id);
			}
		}

		if (!rtp_output) {
			if (stream->output_performs_decoding == FALSE) {
				stream->jpegwriter = ms_factory_create_filter(stream->ms.factory, MS_JPEG_WRITER_ID);
				if (stream->jpegwriter) {
					stream->tee2 = ms_factory_create_filter(stream->ms.factory, MS_TEE_ID);
					stream->forward_sink = ms_factory_create_filter(stream->ms.factory, MS_ITC_SINK_ID);
				}
			}

			/* set parameters to the decoder*/
			if (pt->send_fmtp) {
				ms_filter_call_method(stream->ms.decoder, MS_FILTER_ADD_FMTP, pt->send_fmtp);
			}
			if (pt->recv_fmtp != NULL)
				ms_filter_call_method(stream->ms.decoder, MS_FILTER_ADD_FMTP, (void *)pt->recv_fmtp);
			configure_decoder(stream, pt);

			if (stream->output_performs_decoding) {
				format = mime_type_to_pix_format(pt->mime_type);
			} else {
				/*force the decoder to output YUV420P */
				format = MS_YUV420P;
			}
			ms_filter_call_method(stream->ms.decoder, MS_FILTER_SET_PIX_FMT, &format);

			/*configure the display window */
			if (stream->output != NULL) {
				int autofit = 1;
				// Use default size. If output supports it, it should resize automatically after first received frame
				disp_size.width = MS_VIDEO_SIZE_CIF_W;
				disp_size.height = MS_VIDEO_SIZE_CIF_H;

				ms_filter_call_method(stream->output, MS_FILTER_SET_VIDEO_SIZE, &disp_size);

				/* if pixconv is used, force yuv420 */
				if (stream->pixconv || !stream->source)
					ms_filter_call_method(stream->output, MS_FILTER_SET_PIX_FMT, &format);
				/* else, use format from input */
				else {
					MSPixFmt source_format;
					ms_filter_call_method(stream->source, MS_FILTER_GET_PIX_FMT, &source_format);
					ms_filter_call_method(stream->output, MS_FILTER_SET_PIX_FMT, &source_format);
				}

				if (ms_filter_has_method(stream->output, MS_VIDEO_DISPLAY_SET_LOCAL_VIEW_MODE))
					ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_SET_LOCAL_VIEW_MODE, &stream->corner);
				if (stream->window_id != 0) {
					autofit = 0;
					ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_SET_NATIVE_WINDOW_ID, &stream->window_id);
				}
				if (ms_filter_has_method(stream->output, MS_VIDEO_DISPLAY_ENABLE_AUTOFIT))
					ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_ENABLE_AUTOFIT, &autofit);
				if (stream->display_filter_auto_rotate_enabled &&
				    ms_filter_has_method(stream->output, MS_VIDEO_DISPLAY_SET_DEVICE_ORIENTATION)) {
					ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_SET_DEVICE_ORIENTATION,
					                      &stream->device_orientation);
				}
				if (ms_filter_has_method(stream->output, MS_VIDEO_DISPLAY_SET_MODE)) {
					ms_message("Video stream[%p] thumbnail[%d] direction[%d]: set display mode %d to filter %s", stream,
					           stream->content == MSVideoContentThumbnail ? 1 : 0,
					           media_stream_get_direction(&stream->ms), stream->display_mode,
					           ms_filter_get_name(stream->output));
					ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_SET_MODE, &stream->display_mode);
				}
			}
		}

		/* and connect the filters */
		ms_connection_helper_start(&ch);
		ms_connection_helper_link(&ch, stream->ms.rtprecv, -1, 0);
		if ((stream->output_performs_decoding == FALSE) && !rtp_output) {
			if (stream->aggregator) {
				ms_connection_helper_link(&ch, stream->aggregator, 0, 0);
				for (int i = 0; i < VIDEO_STREAM_MAX_BRANCHES; i++) {
					ms_filter_link(stream->branches[i].recv, 0, stream->aggregator, i + 1);
				}
			}

			if (stream->recorder_output) {
				ms_connection_helper_link(&ch, stream->tee3, 0, 0);
				ms_filter_link(stream->tee3, 1, stream->recorder_output, 0);
				video_stream_enable_recording(stream,
				                              FALSE); /*until recorder is started, the tee3 is kept muted on pin 1*/
				configure_recorder_output(stream);
			}

			ms_connection_helper_link(&ch, stream->ms.decoder, 0, 0);
		}
		if (stream->tee2) {
			ms_connection_helper_link(&ch, stream->tee2, 0, 0);
			ms_filter_link(stream->tee2, 1, stream->jpegwriter, 0);
			ms_filter_link(stream->tee2, 2, stream->forward_sink, 0);
			configure_sink(stream, stream->forward_sink);
		}
		if (stream->output != NULL) ms_connection_helper_link(&ch, stream->output, 0, -1);
		/* the video source must be sent for preview , if it exists. */
		if (stream->tee != NULL && stream->output != NULL && stream->output2 == NULL) {
			// Don't add the preview output if the source is encoded. We could also add a decoding step here
			if (stream->source_performs_encoding == FALSE) {
				ms_filter_link(stream->tee, 1, stream->output, 1);
			}
		}
	}
	if (media_stream_get_direction(&stream->ms) == MediaStreamSendOnly) {
		stream->ms.rtprecv = ms_factory_create_filter(stream->ms.factory, MS_RTP_RECV_ID);
		ms_filter_call_method(stream->ms.rtprecv, MS_RTP_RECV_SET_SESSION, stream->ms.sessions.rtp_session);
		stream->ms.voidsink = ms_factory_create_filter(stream->ms.factory, MS_VOID_SINK_ID);
		ms_filter_link(stream->ms.rtprecv, 0, stream->ms.voidsink, 0);
	}

	/*start the video recorder if it was opened previously*/
	if (stream->recorder_output && ms_filter_implements_interface(stream->recorder_output, MSFilterRecorderInterface)) {
		MSRecorderState state = MSRecorderClosed;
		ms_filter_call_method(stream->recorder_output, MS_RECORDER_GET_STATE, &state);
		if (state == MSRecorderPaused) {
			ms_filter_call_method_noarg(stream->recorder_output, MS_RECORDER_START);
		}
	}

	/* create the ticker */
	if (stream->ms.sessions.ticker == NULL) media_stream_start_ticker(&stream->ms);

	stream->ms.start_time = ms_time(NULL);
	stream->last_fps_check = (uint64_t)-1;
	stream->last_camera_check = (uint64_t)-1;
	stream->ms.is_beginning = TRUE;

	/* attach the graphs */
	if (stream->source) {
		ms_ticker_attach(stream->ms.sessions.ticker, stream->source);
	}
	if (stream->void_source) {
		ms_ticker_attach(stream->ms.sessions.ticker, stream->void_source);
	}
	if (stream->ms.rtprecv) ms_ticker_attach(stream->ms.sessions.ticker, stream->ms.rtprecv);

	stream->ms.state = MSStreamStarted;
	return 0;
}

int video_stream_start_with_source(VideoStream *stream,
                                   RtpProfile *profile,
                                   const char *rem_rtp_ip,
                                   int rem_rtp_port,
                                   const char *rem_rtcp_ip,
                                   int rem_rtcp_port,
                                   int payload,
                                   int jitt_comp,
                                   MSWebCam *cam,
                                   MSFilter *source) {
	return video_stream_start_with_source_and_output(stream, profile, rem_rtp_ip, rem_rtp_port, rem_rtcp_ip,
	                                                 rem_rtcp_port, payload, jitt_comp, cam, source, NULL);
}

void video_stream_prepare_video(VideoStream *stream) {
	video_stream_unprepare_video(stream);
	stream->ms.rtprecv = ms_factory_create_filter(stream->ms.factory, MS_RTP_RECV_ID);
	rtp_session_set_payload_type(stream->ms.sessions.rtp_session, 0);
	rtp_session_enable_rtcp(stream->ms.sessions.rtp_session, FALSE);
	ms_filter_call_method(stream->ms.rtprecv, MS_RTP_RECV_SET_SESSION, stream->ms.sessions.rtp_session);
	stream->ms.voidsink = ms_factory_create_filter(stream->ms.factory, MS_VOID_SINK_ID);
	ms_filter_link(stream->ms.rtprecv, 0, stream->ms.voidsink, 0);
	media_stream_start_ticker(&stream->ms);
	ms_ticker_attach(stream->ms.sessions.ticker, stream->ms.rtprecv);
	stream->ms.state = MSStreamPreparing;
}

void video_stream_unprepare_video(VideoStream *stream) {
	if (stream->ms.state == MSStreamPreparing) {
		stop_preload_graph(stream);
		stream->ms.state = MSStreamInitialized;
	}
}

MSFilter *video_stream_get_source_filter(const VideoStream *stream) {
	if (stream) {
		return stream->source;
	} else {
		return NULL;
	}
}

/**
 * Will update the source camera for the videostream passed as argument.
 * The parameters:
 * - stream : the stream for which to update the source
 * - cam : the camera which should now be considered as the new source.
 * - new_source (optional): if passed as non-NULL, it is expected that this filter comes from the specified camera.
 *							In this case we don't create the source and use this one instead.
 *                          This allows you to reuse the camera and keep it alive when not needed, for fast on/off
 *operations
 * - sink : when this filter is not NULL, it represents the AVPlayer video ITC source, which allows inter-graph
 *communication.
 * - keep_old_source: when TRUE, will not destroy the previous stream source and return it for later usage.
 *
 * @return NULL if keep_old_source is FALSE, or the previous source filter if keep_old_source is TRUE
 */
static MSFilter *_video_stream_change_camera(VideoStream *stream,
                                             MSWebCam *cam,
                                             MSFilter *new_source,
                                             MSFilter *sink,
                                             bool_t keep_old_source,
                                             bool_t skip_payload_config,
                                             bool_t skip_bitrate,
                                             bool_t is_forwarding,
                                             bool_t is_preview) {
	MSFilter *old_source = NULL;
	bool_t new_src_different = (new_source && new_source != stream->source);
	bool_t use_player = (sink && !stream->player_active) || (!sink && stream->player_active && cam != NULL);
	bool_t change_source = ((cam != NULL && cam != stream->cam) || new_src_different || use_player);
	bool_t encoder_has_builtin_converter = (!stream->is_forwarding && !stream->pixconv && !stream->sizeconv);
	/* We get the ticker from the source filter rather than stream->ms.sessions.ticker, for the case where the graph is
	 * actually running in a MSVideoConference that has its own ticker. */
	MSTicker *current_ticker = stream->source ? ms_filter_get_ticker(stream->source) : NULL;

	if (current_ticker && stream->source) {
		ms_ticker_detach(current_ticker, stream->source);
		/*unlink source filters and subsequent post processing filters */
		if (encoder_has_builtin_converter || (stream->source_performs_encoding == TRUE)) {
			ms_filter_unlink(stream->source, 0, stream->tee, 0);
		} else {
			if (stream->pixconv) {
				ms_filter_unlink(stream->source, 0, stream->pixconv, 0);
				ms_filter_unlink(stream->pixconv, 0, stream->tee, 0);
			} else {
				ms_filter_unlink(stream->source, 0, stream->tee, 0);
			}
			if (stream->sizeconv) {
				ms_filter_unlink(stream->tee, 0, stream->sizeconv, 0);
				if (stream->source_performs_encoding == FALSE) {
					if (stream->ms.encoder) ms_filter_unlink(stream->sizeconv, 0, stream->ms.encoder, 0);
				} else {
					ms_filter_unlink(stream->sizeconv, 0, stream->ms.rtpsend, 0);
				}
			} else {
				if (stream->source_performs_encoding == FALSE) {
					if (stream->ms.encoder) ms_filter_unlink(stream->tee, 0, stream->ms.encoder, 0);
				} else {
					ms_filter_unlink(stream->tee, 0, stream->ms.rtpsend, 0);
				}
			}
		}
		/*destroy the filters */
		if (change_source) {
			if (!keep_old_source) {
				ms_filter_destroy(stream->source);
			} else {
				old_source = stream->source;
			}
		}

		if (!encoder_has_builtin_converter && (stream->source_performs_encoding == FALSE)) {
			/* FIXME:
			 * Destroying pixconv is a problem when pixconv is actually a video decoder because we are playing
			 * from mkv file with ItcSource. We are going to loose the decoding context until next key frame.
			 */
			if (stream->pixconv) {
				ms_filter_destroy(stream->pixconv);
				stream->pixconv = NULL;
			}
			if (stream->sizeconv) {
				ms_filter_destroy(stream->sizeconv);
				stream->sizeconv = NULL;
			}
		}

		/*re create new ones and configure them*/
		if (change_source) {
			if (sink) {
				stream->source = ms_factory_create_filter(stream->ms.factory, MS_ITC_SOURCE_ID);
				ms_filter_call_method(sink, MS_ITC_SINK_CONNECT, stream->source);
				stream->player_active = TRUE;
				stream->is_forwarding = is_forwarding;
			} else {
				stream->source = new_source ? new_source : ms_web_cam_create_reader(cam);
				stream->cam = cam;
				stream->player_active = FALSE;
				stream->is_forwarding = FALSE;
			}
		}
		if (stream->source_performs_encoding == TRUE) {
			stream->ms.encoder = stream->source; /* Consider the encoder is the source */
		}

		/* update orientation for video output*/
		if (stream->output && stream->display_filter_auto_rotate_enabled &&
		    ms_filter_has_method(stream->output, MS_VIDEO_DISPLAY_SET_DEVICE_ORIENTATION)) {
			ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_SET_DEVICE_ORIENTATION, &stream->device_orientation);
		}

		if (!skip_payload_config && stream->ms.sessions.rtp_session) {
			PayloadType *pt;
			RtpProfile *profile;
			int payload;

			/* Apply bitrate limit to increase video size if the preferred one has changed. */
			profile = rtp_session_get_profile(stream->ms.sessions.rtp_session);
			payload = rtp_session_get_send_payload_type(stream->ms.sessions.rtp_session);
			pt = rtp_profile_get_payload(profile, payload);
			if (stream->source_performs_encoding == TRUE) {
				MSPixFmt format = mime_type_to_pix_format(pt->mime_type);
				ms_filter_call_method(stream->source, MS_FILTER_SET_PIX_FMT, &format);
			}
			apply_video_preset(stream, pt);
			if (!skip_bitrate) apply_bitrate_limit(stream, pt);
		}
		if (is_preview) _configure_video_preview_source(stream, change_source);
		else configure_video_source(stream, skip_bitrate, change_source);

		if (encoder_has_builtin_converter || (stream->source_performs_encoding == TRUE)) {
			ms_filter_link(stream->source, 0, stream->tee, 0);
		} else {
			if (stream->pixconv) {
				ms_filter_link(stream->source, 0, stream->pixconv, 0);
				ms_filter_link(stream->pixconv, 0, stream->tee, 0);
			} else {
				ms_filter_link(stream->source, 0, stream->tee, 0);
			}
			if (stream->sizeconv) {
				ms_filter_link(stream->tee, 0, stream->sizeconv, 0);
				if (stream->source_performs_encoding == FALSE) {
					if (stream->ms.encoder) ms_filter_link(stream->sizeconv, 0, stream->ms.encoder, 0);
				} else {
					ms_filter_link(stream->sizeconv, 0, stream->ms.rtpsend, 0);
				}
			} else {
				if (stream->source_performs_encoding == FALSE) {
					if (stream->ms.encoder) ms_filter_link(stream->tee, 0, stream->ms.encoder, 0);
				} else {
					ms_filter_link(stream->tee, 0, stream->ms.rtpsend, 0);
				}
			}
		}

		// Change value of mirroring as potentially the webcam was changed
		if (stream->output2 && ms_filter_implements_interface(stream->output2, MSFilterVideoDisplayInterface)) {
			assign_value_to_mirroring_flag_to_preview(stream);
		}

		ms_ticker_attach(current_ticker, stream->source);
	}
	return old_source;
}

void video_stream_update_video_params(VideoStream *stream) {
	/*calling video_stream_change_camera() does the job of unplumbing/replumbing and configuring the new graph*/
	_video_stream_change_camera(stream, NULL, NULL, NULL, FALSE, FALSE, FALSE, FALSE, FALSE);
}

void video_stream_change_camera(VideoStream *stream, MSWebCam *cam) {
	_video_stream_change_camera(stream, cam, NULL, NULL, FALSE, FALSE, FALSE, FALSE, FALSE);
}

void video_stream_change_camera_skip_bitrate(VideoStream *stream, MSWebCam *cam) {
	_video_stream_change_camera(stream, cam, NULL, NULL, FALSE, FALSE, TRUE, FALSE, FALSE);
}

MSFilter *video_stream_change_camera_keep_previous_source(VideoStream *stream, MSWebCam *cam) {
	return _video_stream_change_camera(stream, cam, NULL, NULL, TRUE, FALSE, FALSE, FALSE, FALSE);
}

MSFilter *
video_stream_change_source_filter(VideoStream *stream, MSWebCam *cam, MSFilter *filter, bool_t keep_previous) {
	return _video_stream_change_camera(stream, cam, filter, NULL, keep_previous, FALSE, FALSE, FALSE, FALSE);
}

void video_stream_open_player(VideoStream *stream, MSFilter *sink) {
	ms_message("video_stream_open_player(): sink=%p", sink);
	_video_stream_change_camera(stream, stream->cam, NULL, sink, FALSE, FALSE, FALSE, FALSE, FALSE);
}

void video_stream_close_player(VideoStream *stream) {
	_video_stream_change_camera(stream, stream->cam, NULL, NULL, FALSE, FALSE, FALSE, FALSE, FALSE);
}

void video_stream_forward_source_stream(VideoStream *stream, VideoStream *source) {
	_video_stream_change_camera(stream, stream->cam, NULL, source->forward_sink, FALSE, FALSE, FALSE, TRUE, FALSE);
}

void video_stream_send_fir(VideoStream *stream) {
	if (stream->ms.sessions.rtp_session != NULL) {
		rtp_session_send_rtcp_fb_fir(stream->ms.sessions.rtp_session);
	}
}

void video_stream_send_pli(VideoStream *stream) {
	if (stream->ms.sessions.rtp_session != NULL) {
		rtp_session_send_rtcp_fb_pli(stream->ms.sessions.rtp_session);
	}
}

void video_stream_send_sli(VideoStream *stream, uint16_t first, uint16_t number, uint8_t picture_id) {
	if (stream->ms.sessions.rtp_session != NULL) {
		rtp_session_send_rtcp_fb_sli(stream->ms.sessions.rtp_session, first, number, picture_id);
	}
}

void video_stream_send_rpsi(VideoStream *stream, uint8_t *bit_string, uint16_t bit_string_len) {
	if (stream->ms.sessions.rtp_session != NULL) {
		rtp_session_send_rtcp_fb_rpsi(stream->ms.sessions.rtp_session, bit_string, bit_string_len);
	}
}

void video_stream_send_vfu(VideoStream *stream) {
	if (stream->ms.encoder) ms_filter_call_method_noarg(stream->ms.encoder, MS_VIDEO_ENCODER_REQ_VFU);
}

static MSFilter *_video_stream_stop(VideoStream *stream, bool_t keep_source) {
	MSEventQueue *evq;
	MSFilter *source = NULL;

	stream->eventcb = NULL;
	stream->event_pointer = NULL;
	if (stream->ms.sessions.ticker) {
		if (stream->ms.state == MSStreamPreparing) {
			stop_preload_graph(stream);
		} else if (stream->ms.state == MSStreamStarted) {
			if (stream->source) ms_ticker_detach(stream->ms.sessions.ticker, stream->source);
			if (stream->void_source) ms_ticker_detach(stream->ms.sessions.ticker, stream->void_source);
			if (stream->ms.rtprecv) ms_ticker_detach(stream->ms.sessions.ticker, stream->ms.rtprecv);
			ms_message("Stopping VideoStream");
			media_stream_print_summary(&stream->ms);
			if (stream->void_source) {
				MSConnectionHelper ch;
				ms_connection_helper_start(&ch);
				ms_connection_helper_unlink(&ch, stream->void_source, -1, 0);
				ms_connection_helper_unlink(&ch, stream->ms.rtpsend, 0, -1);
			}
			if (stream->source) {
				MSConnectionHelper ch;
				bool_t rtp_source = (ms_filter_get_id(stream->source) == MS_RTP_RECV_ID) ? TRUE : FALSE;
				ms_connection_helper_start(&ch);
				ms_connection_helper_unlink(&ch, stream->source, -1, 0);
				if (stream->pixconv) {
					ms_connection_helper_unlink(&ch, stream->pixconv, 0, 0);
				}
				if (stream->itcsink) {
					ms_filter_unlink(stream->tee, 3, stream->itcsink, 0);
				}
				if (stream->tee) {
					ms_connection_helper_unlink(&ch, stream->tee, 0, 0);
				}
				if (stream->sizeconv) {
					ms_connection_helper_unlink(&ch, stream->sizeconv, 0, 0);
				}
				if ((stream->source_performs_encoding == FALSE) && !rtp_source) {
					ms_connection_helper_unlink(&ch, stream->ms.encoder, 0, 0);
				}

				ms_connection_helper_unlink(&ch, stream->ms.rtpsend, 0, -1);
				if (stream->output2) {
					ms_filter_unlink(stream->tee, 1, stream->output2, 0);
				}
				if (stream->local_jpegwriter) {
					ms_filter_unlink(stream->tee, 2, stream->local_jpegwriter, 0);
				}
			}
			if (stream->ms.voidsink) {
				ms_filter_unlink(stream->ms.rtprecv, 0, stream->ms.voidsink, 0);
			} else if (stream->ms.rtprecv) {
				MSConnectionHelper h;
				bool_t rtp_output = (ms_filter_get_id(stream->output) == MS_RTP_SEND_ID) ? TRUE : FALSE;
				ms_connection_helper_start(&h);
				ms_connection_helper_unlink(&h, stream->ms.rtprecv, -1, 0);
				if ((stream->output_performs_decoding == FALSE) && !rtp_output) {
					if (stream->aggregator) {
						ms_connection_helper_unlink(&h, stream->aggregator, 0, 0);
						for (int i = 0; i < VIDEO_STREAM_MAX_BRANCHES; i++) {
							ms_filter_unlink(stream->branches[i].recv, 0, stream->aggregator, i + 1);
						}
					}
					if (stream->recorder_output) {
						ms_connection_helper_unlink(&h, stream->tee3, 0, 0);
						ms_filter_unlink(stream->tee3, 1, stream->recorder_output, 0);
					}
					ms_connection_helper_unlink(&h, stream->ms.decoder, 0, 0);
				}
				if (stream->tee2) {
					ms_connection_helper_unlink(&h, stream->tee2, 0, 0);
					ms_filter_unlink(stream->tee2, 1, stream->jpegwriter, 0);
					ms_filter_unlink(stream->tee2, 2, stream->forward_sink, 0);
				}
				if (stream->output) ms_connection_helper_unlink(&h, stream->output, 0, -1);
				if (stream->tee && stream->output && stream->output2 == NULL) {
					if (stream->source_performs_encoding == FALSE) {
						ms_filter_unlink(stream->tee, 1, stream->output, 1);
					}
				}
			}
		}
	}
	rtp_session_set_rtcp_xr_media_callbacks(stream->ms.sessions.rtp_session, NULL);

	rtp_session_signal_disconnect_by_callback(stream->ms.sessions.rtp_session, "payload_type_changed",
	                                          (RtpCallback)video_stream_payload_type_changed);

	/*Automatically the video recorder if it was opened previously*/
	if (stream->recorder_output && ms_filter_implements_interface(stream->recorder_output, MSFilterRecorderInterface)) {
		MSRecorderState state = MSRecorderClosed;
		ms_filter_call_method(stream->recorder_output, MS_RECORDER_GET_STATE, &state);
		if (state != MSRecorderClosed) {
			ms_filter_call_method_noarg(stream->recorder_output, MS_RECORDER_CLOSE);
		}
	}

	if (keep_source) {
		source = stream->source;
		stream->source = NULL; // will prevent video_stream_free() from destroying the source
	}
	/*before destroying the filters, pump the event queue so that pending events have a chance to reach their listeners.
	 * When the filter are destroyed, all their pending events in the event queue will be cancelled*/
	evq = ms_factory_get_event_queue(stream->ms.factory);
	if (evq) ms_event_queue_pump(evq);

	if (stream->ms.sessions.rtp_session->fec_stream) {
		media_stream_destroy_fec_stream(&stream->ms);
	}

	video_stream_free(stream);

	return source;
}

void video_stream_stop(VideoStream *stream) {
	// don't keep the source
	_video_stream_stop(stream, FALSE);
}

MSFilter *video_stream_stop_keep_source(VideoStream *stream) {
	return _video_stream_stop(stream, TRUE);
}

void video_stream_show_video(VideoStream *stream, bool_t show) {
	if (stream->output) {
		ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_SHOW_VIDEO, &show);
	}
}

void *video_stream_get_native_window_id(VideoStream *stream) {
	void *id;
	if (stream->output) {
		if (ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_GET_NATIVE_WINDOW_ID, &id) == 0) return id;
	}
	return stream->window_id;
}

void *video_stream_create_native_window_id(VideoStream *stream, void *context) {
	void *id = context;
	if (stream->output) {
		if (ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_CREATE_NATIVE_WINDOW_ID, &id) == 0) return id;
	}
	return stream->window_id;
}

void video_stream_set_native_window_id(VideoStream *stream, void *id) {
	stream->window_id = id;
	if (stream->output) {
		ms_filter_call_method(stream->output, MS_VIDEO_DISPLAY_SET_NATIVE_WINDOW_ID, &id);
	}
}

void video_stream_set_native_preview_window_id(VideoStream *stream, void *id) {
	stream->preview_window_id = id;
#if !TARGET_OS_IPHONE
	if (stream->output2) {
		ms_filter_call_method(stream->output2, MS_VIDEO_DISPLAY_SET_NATIVE_WINDOW_ID, &id);
	}
#endif
	if (stream->source) {
		ms_filter_call_method(stream->source, MS_VIDEO_DISPLAY_SET_NATIVE_WINDOW_ID, &id);
	}
}

void *video_stream_get_native_preview_window_id(VideoStream *stream) {
	void *id = 0;
	if (stream->output2) {
		if (ms_filter_call_method(stream->output2, MS_VIDEO_DISPLAY_GET_NATIVE_WINDOW_ID, &id) == 0) return id;
	}
	if (stream->source) {
		if (ms_filter_has_method(stream->source, MS_VIDEO_DISPLAY_GET_NATIVE_WINDOW_ID) &&
		    ms_filter_call_method(stream->source, MS_VIDEO_DISPLAY_GET_NATIVE_WINDOW_ID, &id) == 0)
			return id;
	}
	return stream->preview_window_id;
}

void *video_stream_create_native_preview_window_id(VideoStream *stream, void *context) {
	void *id = context;
	if (stream->output2) {
		if (ms_filter_call_method(stream->output2, MS_VIDEO_DISPLAY_CREATE_NATIVE_WINDOW_ID, &id) == 0) return id;
	}
	if (stream->source) {
		if (ms_filter_has_method(stream->source, MS_VIDEO_DISPLAY_CREATE_NATIVE_WINDOW_ID) &&
		    ms_filter_call_method(stream->source, MS_VIDEO_DISPLAY_CREATE_NATIVE_WINDOW_ID, &id) == 0)
			return id;
	}
	return stream->preview_window_id;
}

void video_stream_use_preview_video_window(VideoStream *stream, bool_t yesno) {
	stream->use_preview_window = yesno;
}

void video_stream_set_device_rotation(VideoStream *stream, int orientation) {
	stream->device_orientation = orientation;
}

void video_stream_set_display_mode(VideoStream *stream, MSVideoDisplayMode mode) {
	stream->display_mode = mode;
}

MSVideoDisplayMode video_stream_get_display_mode(VideoStream *stream) {
	return stream->display_mode;
}

MSVideoDisplayMode video_stream_get_preview_display_mode(VideoStream *stream) {
	return stream->preview_display_mode;
}

void video_stream_set_preview_display_mode(VideoStream *stream, MSVideoDisplayMode mode) {
	stream->preview_display_mode = mode;
}

void video_stream_set_freeze_on_error(VideoStream *stream, bool_t yesno) {
	stream->freeze_on_error = yesno;
}

void video_stream_set_fallback_to_dummy_codec(VideoStream *stream, bool_t yesno) {
	stream->fallback_to_dummy_codec = yesno;
}

int video_stream_get_camera_sensor_rotation(VideoStream *stream) {
	int rotation = -1;
	if (stream->source) {
		if (ms_filter_has_method(stream->source, MS_VIDEO_CAPTURE_GET_CAMERA_SENSOR_ROTATION) &&
		    ms_filter_call_method(stream->source, MS_VIDEO_CAPTURE_GET_CAMERA_SENSOR_ROTATION, &rotation) == 0)
			return rotation;
	}
	return -1;
}

VideoPreview *video_preview_new(MSFactory *factory) {
	VideoPreview *stream = (VideoPreview *)ms_new0(VideoPreview, 1);
	stream->ms.factory = factory;
	MS_VIDEO_SIZE_ASSIGN(stream->sent_vsize, CIF);
	choose_display_name(stream);
	stream->ms.owns_sessions = TRUE;
	return stream;
}

MSVideoSize video_preview_get_current_size(VideoPreview *stream) {
	MSVideoSize ret = {0};
	if (stream->source) {
		ms_filter_call_method(stream->source, MS_FILTER_GET_VIDEO_SIZE, &ret);
	}
	return ret;
}

static void _configure_video_preview_source(VideoPreview *stream, bool_t change_source) {
	MSPixFmt format;
	MSVideoSize vsize = stream->sent_vsize;
	float fps;
	bool_t is_player =
	    stream->content != MSVideoContentThumbnail &&
	    (ms_filter_get_id(stream->source) == MS_ITC_SOURCE_ID || ms_filter_get_id(stream->source) == MS_MKV_PLAYER_ID);

	if (stream->forced_fps != 0) fps = stream->forced_fps;
	else fps = (float)29.97;

	/* Transmit orientation to source filter. */
	if (ms_filter_has_method(stream->source, MS_VIDEO_CAPTURE_SET_DEVICE_ORIENTATION))
		ms_filter_call_method(stream->source, MS_VIDEO_CAPTURE_SET_DEVICE_ORIENTATION, &stream->device_orientation);
	/* Initialize the capture device orientation. */
	if (ms_filter_has_method(stream->source, MS_VIDEO_DISPLAY_SET_DEVICE_ORIENTATION)) {
		ms_filter_call_method(stream->source, MS_VIDEO_DISPLAY_SET_DEVICE_ORIENTATION, &stream->device_orientation);
	}
	if (change_source) { // Add callbacks if sources changed.
		ms_filter_add_notify_callback(stream->source, event_cb, stream, FALSE);
		if (!is_player) ms_filter_add_notify_callback(stream->source, preview_source_event_cb, stream, FALSE);
		/* It is important that the internal_event_cb is called synchronously! */
		ms_filter_add_notify_callback(stream->source, internal_event_cb, stream, TRUE);
	}
	if (!ms_filter_implements_interface(stream->source, MSFilterVideoEncoderInterface)) {
		ms_filter_call_method(stream->source, MS_FILTER_SET_VIDEO_SIZE, &vsize);
		if (ms_filter_get_id(stream->source) != MS_STATIC_IMAGE_ID) {
			ms_filter_call_method(stream->source, MS_FILTER_SET_FPS, &fps);
		}
		ms_filter_call_method(stream->source, MS_FILTER_GET_VIDEO_SIZE, &vsize);
	} else {
		MSVideoConfiguration vconf;
		ms_filter_call_method(stream->source, MS_VIDEO_ENCODER_GET_CONFIGURATION, &vconf);
		vconf.vsize = vsize;
		vconf.fps = fps;
		ms_filter_call_method(stream->source, MS_VIDEO_ENCODER_SET_CONFIGURATION, &vconf);
	}
	ms_filter_call_method(stream->source, MS_FILTER_GET_PIX_FMT, &format);
	if (stream->pixconv) {
		ms_filter_destroy(stream->pixconv);
		stream->pixconv = NULL;
	}
	if (format == MS_MJPEG) {
		stream->pixconv = ms_factory_create_filter(stream->ms.factory, MS_MJPEG_DEC_ID);
		if (stream->pixconv == NULL) {
			ms_error("Could not create mjpeg decoder, check your build options.");
		}
	} else if (!ms_filter_implements_interface(stream->source, MSFilterVideoEncoderInterface)) {
		stream->pixconv = ms_factory_create_filter(stream->ms.factory, MS_PIX_CONV_ID);
		ms_filter_call_method(stream->pixconv, MS_FILTER_SET_PIX_FMT, &format);
		ms_filter_call_method(stream->pixconv, MS_FILTER_SET_VIDEO_SIZE, &vsize);
	}
}
static void configure_video_preview_source(VideoPreview *stream) {
	_configure_video_preview_source(stream, TRUE);
}
void video_preview_start(VideoPreview *stream, MSWebCam *device) {
	MSConnectionHelper ch;
	MSTickerParams ticker_params = {0};

	stream->source = ms_web_cam_create_reader(device);
	stream->cam = device;

	/* configure the filters */
	configure_video_preview_source(stream);

#if defined(__ANDROID__)
	// On Android the capture filter doesn't need a display filter to render the preview
	stream->output2 = ms_factory_create_filter(stream->ms.factory, MS_VOID_SINK_ID);
	ms_filter_add_notify_callback(stream->output2, display_cb, stream, FALSE);
#else
	{
		MSPixFmt format = MS_YUV420P; /* Display format */
		int corner = -1;
		MSVideoSize disp_size = stream->sent_vsize;
		const char *displaytype = stream->display_name;

		if (displaytype) {
			stream->output2 = ms_factory_create_filter_from_name(stream->ms.factory, displaytype);
			if (stream->output2) {
				ms_filter_add_notify_callback(stream->output2, display_cb, stream, FALSE);
				ms_filter_call_method(stream->output2, MS_FILTER_SET_PIX_FMT, &format);
				ms_filter_call_method(stream->output2, MS_FILTER_SET_VIDEO_SIZE, &disp_size);
				ms_filter_call_method(stream->output2, MS_VIDEO_DISPLAY_SET_LOCAL_VIEW_MODE, &corner);
			}
			assign_value_to_mirroring_flag_to_preview(stream);
			/* and then connect all */
		}
	}
#endif

	stream->local_jpegwriter = ms_factory_create_filter(stream->ms.factory, MS_JPEG_WRITER_ID);
	if (stream->local_jpegwriter) {
		stream->tee = ms_factory_create_filter(stream->ms.factory, MS_TEE_ID);
	}

	ms_connection_helper_start(&ch);
	ms_connection_helper_link(&ch, stream->source, -1, 0);

	if (ms_filter_implements_interface(stream->source, MSFilterVideoEncoderInterface)) {
		/* Need to decode first */
		stream->ms.decoder = ms_factory_create_decoder(stream->ms.factory, stream->source->desc->enc_fmt);
		if (stream->ms.decoder == NULL) {
			/* big problem: we don't have a registered decoderfor this payload...*/
			ms_error("video_preview_start: No decoder available for payload %s.", stream->source->desc->enc_fmt);
			return;
		}
		ms_connection_helper_link(&ch, stream->ms.decoder, 0, 0);
	}

	if (stream->output2) {
		video_stream_set_native_preview_window_id(stream, stream->preview_window_id);
	}

	if (stream->pixconv) {
		ms_connection_helper_link(&ch, stream->pixconv, 0, 0);
	}

	if (stream->enable_qrcode_decoder) {
#ifdef QRCODE_ENABLED
		stream->qrcode = ms_factory_create_filter(stream->ms.factory, MS_QRCODE_READER_ID);
		configure_qrcode_filter(stream);
		ms_connection_helper_link(&ch, stream->qrcode, 0, 0);
		ms_filter_call_method(stream->qrcode, MS_QRCODE_READET_SET_DECODER_RECT, &stream->decode_rect);
#else
		ms_error("Can't create qrcode decoder, dependency not enabled.");
#endif
	}

	if (stream->tee) {
		ms_connection_helper_link(&ch, stream->tee, 0, 0);
		if (stream->output2) ms_filter_link(stream->tee, 1, stream->output2, 0);
		ms_filter_link(stream->tee, 2, stream->local_jpegwriter, 0);
	} else {
		if (stream->output2) ms_filter_link(stream->pixconv, 0, stream->output2, 0);
	}

	/* create the ticker */
	ticker_params.name = "Preview";
	ticker_params.prio = __ms_get_default_prio(TRUE);
	stream->ms.sessions.ticker = ms_ticker_new_with_params(&ticker_params);
	ms_ticker_attach(stream->ms.sessions.ticker, stream->source);
	stream->ms.state = MSStreamStarted;
}

void video_preview_enable_qrcode(VideoPreview *stream, bool_t enable) {
	stream->enable_qrcode_decoder = enable;
	if (enable) ms_filter_call_method_noarg(stream->qrcode, MS_QRCODE_READER_RESET_SEARCH);
	else ms_filter_call_method_noarg(stream->qrcode, MS_QRCODE_READER_STOP_SEARCH);
}

void video_preview_set_decode_rect(VideoPreview *stream, MSRect rect) {
	stream->decode_rect = rect;
}

void video_preview_stream_change_camera(VideoStream *stream, MSWebCam *cam) {
	_video_stream_change_camera(stream, cam, NULL, NULL, FALSE, FALSE, FALSE, FALSE, TRUE);
}

void video_preview_stream_update_video_params(VideoStream *stream) {
	/*calling video_preview_stream_change_camera() does the job of unplumbing/replumbing and configuring the new graph
	 * for preview*/
	video_preview_stream_change_camera(stream, stream->cam);
}

bool_t video_preview_qrcode_enabled(BCTBX_UNUSED(VideoPreview *stream)) {
#ifdef QRCODE_ENABLED
	return stream->enable_qrcode_decoder;
#else
	return FALSE;
#endif
}

static MSFilter *_video_preview_stop(VideoPreview *stream, bool_t keep_source) {
	MSFilter *source = NULL;
	MSConnectionHelper ch;
	ms_ticker_detach(stream->ms.sessions.ticker, stream->source);

	stream->eventcb = NULL;
	stream->event_pointer = NULL;

	ms_connection_helper_start(&ch);
	ms_connection_helper_unlink(&ch, stream->source, -1, 0);
	if (stream->ms.decoder) {
		ms_connection_helper_unlink(&ch, stream->ms.decoder, 0, 0);
	}
	if (stream->pixconv) {
		ms_connection_helper_unlink(&ch, stream->pixconv, 0, 0);
	}
	if (stream->qrcode) {
		ms_connection_helper_unlink(&ch, stream->qrcode, 0, 0);
	}
	if (stream->tee) {
		ms_connection_helper_unlink(&ch, stream->tee, 0, 0);
		if (stream->output2) {
			ms_filter_unlink(stream->tee, 1, stream->output2, 0);
		}
		if (stream->local_jpegwriter) {
			ms_filter_unlink(stream->tee, 2, stream->local_jpegwriter, 0);
		}
	} else {
		if (stream->output2) {
			ms_connection_helper_unlink(&ch, stream->output2, 0, 0);
		}
	}

	if (keep_source) {
		source = stream->source;
		ms_message("video_preview_stop: keeping source %p", source);
		stream->source = NULL; // prevent destroy of the source
	}
	video_stream_free(stream);
	return source;
}

void video_preview_stop(VideoPreview *stream) {
	_video_preview_stop(stream, FALSE);
}

MSFilter *video_preview_stop_reuse_source(VideoPreview *stream) {
	return _video_preview_stop(stream, TRUE);
}

static MSFilter *
_video_preview_change_camera(VideoPreview *stream, MSWebCam *cam, MSFilter *new_source, bool_t keep_old_source) {
	MSConnectionHelper ch;
	MSFilter *old_source = NULL;
	bool_t new_src_different = (new_source && new_source != stream->source);
	bool_t change_source = (cam != stream->cam || new_src_different);
	MSVideoSize disp_size = stream->sent_vsize;

	if (stream->ms.sessions.ticker && stream->source) {
		ms_ticker_detach(stream->ms.sessions.ticker, stream->source);
		/*unlink source filters and subsequent post processing filters */
		ms_connection_helper_start(&ch);
		ms_connection_helper_unlink(&ch, stream->source, -1, 0);
		if (stream->pixconv) {
			ms_connection_helper_unlink(&ch, stream->pixconv, 0, 0);
		}
		if (stream->qrcode) {
			ms_connection_helper_unlink(&ch, stream->qrcode, 0, 0);
		}
		if (stream->tee) {
			ms_connection_helper_unlink(&ch, stream->tee, 0, 0);
			if (stream->output2) {
				ms_filter_unlink(stream->tee, 1, stream->output2, 0);
			}
			if (stream->local_jpegwriter) {
				ms_filter_unlink(stream->tee, 2, stream->local_jpegwriter, 0);
			}
		} else {
			if (stream->output2) ms_connection_helper_unlink(&ch, stream->output2, 0, 0);
		}

		/*destroy the filters */
		if (change_source) {
			if (!keep_old_source) {
				ms_filter_destroy(stream->source);
			} else {
				old_source = stream->source;
			}
		}

		if (stream->pixconv) {
			ms_filter_destroy(stream->pixconv);
			stream->pixconv = NULL;
		}

		/*re create new ones and configure them*/
		if (change_source) {
			stream->source = new_source ? new_source : ms_web_cam_create_reader(cam);
			stream->cam = cam;
			stream->player_active = FALSE;
		}

		configure_video_preview_source(stream);
		if (stream->output2) ms_filter_call_method(stream->output2, MS_FILTER_SET_VIDEO_SIZE, &disp_size);

		ms_connection_helper_start(&ch);
		ms_connection_helper_link(&ch, stream->source, -1, 0);
		if (stream->pixconv) {
			ms_connection_helper_link(&ch, stream->pixconv, 0, 0);
		}
		if (stream->qrcode) {
			ms_connection_helper_link(&ch, stream->qrcode, 0, 0);
		}
		if (stream->tee) {
			ms_connection_helper_link(&ch, stream->tee, 0, 0);
			if (stream->output2) {
				if (ms_filter_implements_interface(stream->output2, MSFilterVideoDisplayInterface)) {
					assign_value_to_mirroring_flag_to_preview(stream);
				}
				ms_filter_link(stream->tee, 1, stream->output2, 0);
			}
			if (stream->local_jpegwriter) {
				ms_filter_link(stream->tee, 2, stream->local_jpegwriter, 0);
			}
		} else {
			if (stream->output2) ms_filter_link(stream->pixconv, 0, stream->output2, 0);
		}

		ms_ticker_attach(stream->ms.sessions.ticker, stream->source);
	}
	return old_source;
}

void video_preview_change_camera(VideoPreview *stream, MSWebCam *cam) {
	_video_preview_change_camera(stream, cam, NULL, FALSE);
}

void video_preview_update_video_params(VideoPreview *stream) {
	/* Calling video_preview_change_camera() does the job of unplumbing/replumbing and configuring the new graph */
	video_preview_change_camera(stream, stream->cam);
}

int video_stream_recv_only_start(
    VideoStream *videostream, RtpProfile *profile, const char *addr, int port, int used_pt, int jitt_comp) {
	media_stream_set_direction(&videostream->ms, MediaStreamRecvOnly);
	return video_stream_start(videostream, profile, addr, port, addr, port + 1, used_pt, jitt_comp, NULL);
}

int video_stream_send_only_start(VideoStream *videostream,
                                 RtpProfile *profile,
                                 const char *addr,
                                 int port,
                                 int rtcp_port,
                                 int used_pt,
                                 int jitt_comp,
                                 MSWebCam *device) {
	media_stream_set_direction(&videostream->ms, MediaStreamSendOnly);
	return video_stream_start(videostream, profile, addr, port, addr, rtcp_port, used_pt, jitt_comp, device);
}

void video_stream_recv_only_stop(VideoStream *vs) {
	video_stream_stop(vs);
}

void video_stream_send_only_stop(VideoStream *vs) {
	video_stream_stop(vs);
}

/* enable ZRTP on the video stream using information from the audio stream */
void video_stream_enable_zrtp(VideoStream *vstream, AudioStream *astream) {
	if (astream->ms.sessions.zrtp_context != NULL && vstream->ms.sessions.zrtp_context == NULL) {
		vstream->ms.sessions.zrtp_context =
		    ms_zrtp_multistream_new(&(vstream->ms.sessions), astream->ms.sessions.zrtp_context);
	} else if (vstream->ms.sessions.zrtp_context && !media_stream_secured(&vstream->ms)) {
		ms_zrtp_reset_transmition_timer(vstream->ms.sessions.zrtp_context);
	}
}

void video_stream_start_zrtp(VideoStream *stream) {
	if (stream->ms.sessions.zrtp_context != NULL) {
		if (ms_zrtp_channel_start(stream->ms.sessions.zrtp_context) == MSZRTP_ERROR_CHANNEL_ALREADY_STARTED) {
			ms_zrtp_reset_transmition_timer(stream->ms.sessions.zrtp_context);
		}
	} else {
		ms_warning("Trying to start a ZRTP channel on videotream, but none was enabled");
	}
}

void video_stream_enable_display_filter_auto_rotate(VideoStream *stream, bool_t enable) {
	stream->display_filter_auto_rotate_enabled = enable;
}

bool_t video_stream_is_decoding_error_to_be_reported(VideoStream *stream, uint32_t ms) {
	if (((stream->ms.sessions.ticker->time - stream->last_reported_decoding_error_time) > ms) ||
	    (stream->last_reported_decoding_error_time == 0))
		return TRUE;
	else return FALSE;
}

void video_stream_decoding_error_reported(VideoStream *stream) {
	stream->last_reported_decoding_error_time = stream->ms.sessions.ticker->time;
}

void video_stream_decoding_error_recovered(VideoStream *stream) {
	stream->last_reported_decoding_error_time = 0;
}
const MSWebCam *video_stream_get_camera(const VideoStream *stream) {
	return stream->cam;
}

void video_stream_use_video_preset(VideoStream *stream, const char *preset) {
	if (stream->preset != NULL) ms_free(stream->preset);
	stream->preset = ms_strdup(preset);
}

const char *video_stream_get_video_preset(VideoStream *stream) {
	return stream->preset;
}

/*this function optimizes the processing by enabling the duplication of video packets to the recorder, which is not
 * required to be done when the recorder is not recording of course.*/
void video_stream_enable_recording(VideoStream *stream, bool_t enabled) {
	if (stream->tee3) {
		int pin = 1;
		ms_filter_call_method(stream->tee3, enabled ? MS_TEE_UNMUTE : MS_TEE_MUTE, &pin);
	}
}

MSFilter *video_stream_open_remote_play(VideoStream *stream, const char *filename) {
	MSFilter *source = stream->source;

	if (!source || !ms_filter_implements_interface(source, MSFilterPlayerInterface)) {
		ms_error("video_stream_open_remote_play(): the stream is not using a player.");
		return NULL;
	}
	video_stream_close_remote_play(stream);
	if (ms_filter_call_method(source, MS_PLAYER_OPEN, (void *)filename) != 0) {
		return NULL;
	}
	return source;
}

void video_stream_close_remote_play(VideoStream *stream) {
	MSPlayerState state = MSPlayerClosed;
	MSFilter *source = stream->source;

	if (!source) return;

	ms_filter_call_method(source, MS_PLAYER_GET_STATE, &state);
	if (state != MSPlayerClosed) {
		ms_filter_call_method_noarg(source, MS_PLAYER_CLOSE);
	}
}

MSFilter *video_stream_open_remote_record(VideoStream *stream, const char *filename) {
	MSFilter *recorder = stream->recorder_output;
	if (!recorder || !ms_filter_implements_interface(recorder, MSFilterRecorderInterface)) {
		ms_error("video_stream_open_remote_play(): the stream is not using a recorder.");
		return NULL;
	}
	if (ms_filter_call_method(recorder, MS_RECORDER_OPEN, (void *)filename) != 0) {
		return NULL;
	}
	return recorder;
}

void video_stream_close_remote_record(VideoStream *stream) {
	MSFilter *recorder = stream->recorder_output;
	MSRecorderState state = MSRecorderClosed;

	if (!recorder || !ms_filter_implements_interface(recorder, MSFilterRecorderInterface)) {
		ms_error("video_stream_close_remote_record(): the stream is not using a recorder.");
		return;
	}

	ms_filter_call_method(recorder, MS_RECORDER_GET_STATE, &state);
	if (state != MSRecorderClosed) {
		ms_filter_call_method_noarg(recorder, MS_RECORDER_CLOSE);
	}
}

void video_stream_set_frame_marking_extension_id(VideoStream *stream, int extension_id) {
	stream->frame_marking_extension_id = extension_id;
}

void video_stream_set_sent_video_size_max(VideoStream *stream, MSVideoSize max) {
	stream->max_sent_vsize = max;
}

void video_stream_set_csrc_changed_callback(VideoStream *stream, VideoStreamCsrcChangedCb cb, void *user_pointer) {
	stream->csrc_changed_cb = cb;
	stream->csrc_changed_cb_user_data = user_pointer;
}

bool_t video_stream_local_screen_sharing_enabled(VideoStream *stream) {
	return stream->local_screen_sharing_enabled;
}

void video_stream_enable_local_screen_sharing(VideoStream *stream, bool_t enable) {
	stream->local_screen_sharing_enabled = enable;
}

void video_stream_enable_active_speaker_mode(VideoStream *stream, bool_t enable) {
	stream->active_speaker_mode = enable;
}
