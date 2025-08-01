/*
 * Copyright (c) 2010-2022 Belledonne Communications SARL.
 *
 * This file is part of Liblinphone
 * (see https://gitlab.linphone.org/BC/public/liblinphone).
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

#include <bctoolbox/defs.h>

#include "private.h"

#include "mediastreamer2/dtmfgen.h"
#include "mediastreamer2/mstonedetector.h"

#include "c-wrapper/c-wrapper.h"
#include "linphone/lpconfig.h"

static void ecc_init_filters(EcCalibrator *ecc) {
	unsigned int rate;
	int channels = 1;
	int ecc_channels = 1;
	MSTickerParams params;
	memset(&params, 0, sizeof(params));
	params.name = "Echo calibrator";
	params.prio = MS_TICKER_PRIO_HIGH;
	ecc->ticker = ms_ticker_new_with_params(&params);

	ecc->sndread = ms_snd_card_create_reader(ecc->capt_card);
	ms_message("[Echo Canceller Calibration] Using capture device ID: %s (%i)", ecc->capt_card->id,
	           ecc->capt_card->internal_id);
	ms_filter_call_method(ecc->sndread, MS_FILTER_SET_SAMPLE_RATE, &ecc->rate);
	ms_filter_call_method(ecc->sndread, MS_FILTER_GET_SAMPLE_RATE, &rate);
	ms_filter_call_method(ecc->sndread, MS_FILTER_SET_NCHANNELS, &ecc_channels);
	ms_filter_call_method(ecc->sndread, MS_FILTER_GET_NCHANNELS, &channels);
	ecc->read_resampler = ms_factory_create_filter(ecc->factory, MS_RESAMPLE_ID);
	ms_filter_call_method(ecc->read_resampler, MS_FILTER_SET_SAMPLE_RATE, &rate);
	ms_filter_call_method(ecc->read_resampler, MS_FILTER_SET_OUTPUT_SAMPLE_RATE, &ecc->rate);
	ms_filter_call_method(ecc->read_resampler, MS_FILTER_SET_NCHANNELS, &channels);
	ms_filter_call_method(ecc->read_resampler, MS_FILTER_SET_OUTPUT_NCHANNELS, &ecc_channels);

	ecc->det = ms_factory_create_filter(ecc->factory, MS_TONE_DETECTOR_ID);
	ms_filter_call_method(ecc->det, MS_FILTER_SET_SAMPLE_RATE, &ecc->rate);
	ecc->rec = ms_factory_create_filter(ecc->factory, MS_VOID_SINK_ID);

	ms_filter_link(ecc->sndread, 0, ecc->read_resampler, 0);
	ms_filter_link(ecc->read_resampler, 0, ecc->det, 0);
	ms_filter_link(ecc->det, 0, ecc->rec, 0);

	ecc->play = ms_factory_create_filter(ecc->factory, MS_VOID_SOURCE_ID);
	ecc->gen = ms_factory_create_filter(ecc->factory, MS_DTMF_GEN_ID);
	ms_filter_call_method(ecc->gen, MS_FILTER_SET_SAMPLE_RATE, &ecc->rate);
	ecc->write_resampler = ms_factory_create_filter(ecc->factory, MS_RESAMPLE_ID);
	ecc->sndwrite = ms_snd_card_create_writer(ecc->play_card);
	ms_message("[Echo Canceller Calibration] Using playback device ID: %s (%i)", ecc->play_card->id,
	           ecc->play_card->internal_id);

	// Notify capture filter of playback device (needed to change microphone when using AAudio on Android)
	if (ms_filter_implements_interface(ecc->sndread, MSFilterAudioCaptureInterface)) {
		if (ms_filter_has_method(ecc->sndread, MS_AUDIO_CAPTURE_PLAYBACK_DEVICE_CHANGED)) {
			ms_message("[Echo Canceller Calibration] Notify record filter [%s:%p] that playback device is being "
			           "set to [%s]",
			           ms_filter_get_name(ecc->sndread), ecc->sndread, ecc->play_card->id);
			ms_filter_call_method(ecc->sndread, MS_AUDIO_CAPTURE_PLAYBACK_DEVICE_CHANGED, ecc->play_card);
		}
	}

	ms_filter_call_method(ecc->sndwrite, MS_FILTER_SET_SAMPLE_RATE, &ecc->rate);
	ms_filter_call_method(ecc->sndwrite, MS_FILTER_GET_SAMPLE_RATE, &rate);
	ms_filter_call_method(ecc->sndwrite, MS_FILTER_SET_NCHANNELS, &ecc_channels);
	ms_filter_call_method(ecc->sndwrite, MS_FILTER_GET_NCHANNELS, &channels);
	ms_filter_call_method(ecc->write_resampler, MS_FILTER_SET_SAMPLE_RATE, &ecc->rate);
	ms_filter_call_method(ecc->write_resampler, MS_FILTER_SET_OUTPUT_SAMPLE_RATE, &rate);
	ms_filter_call_method(ecc->write_resampler, MS_FILTER_SET_NCHANNELS, &ecc_channels);
	ms_filter_call_method(ecc->write_resampler, MS_FILTER_SET_OUTPUT_NCHANNELS, &channels);

	ms_filter_link(ecc->play, 0, ecc->gen, 0);
	ms_filter_link(ecc->gen, 0, ecc->write_resampler, 0);
	ms_filter_link(ecc->write_resampler, 0, ecc->sndwrite, 0);

	ms_ticker_attach(ecc->ticker, ecc->sndread);
	ms_ticker_attach(ecc->ticker, ecc->play);

	if (ecc->audio_init_cb != NULL) {
		(*ecc->audio_init_cb)(ecc->cb_data);
	}
}

static void ecc_deinit_filters(EcCalibrator *ecc) {
	if (ecc->audio_uninit_cb != NULL) {
		(*ecc->audio_uninit_cb)(ecc->cb_data);
	}

	ms_ticker_detach(ecc->ticker, ecc->sndread);
	ms_ticker_detach(ecc->ticker, ecc->play);

	ms_filter_unlink(ecc->play, 0, ecc->gen, 0);
	ms_filter_unlink(ecc->gen, 0, ecc->write_resampler, 0);
	ms_filter_unlink(ecc->write_resampler, 0, ecc->sndwrite, 0);

	ms_filter_unlink(ecc->sndread, 0, ecc->read_resampler, 0);
	ms_filter_unlink(ecc->read_resampler, 0, ecc->det, 0);
	ms_filter_unlink(ecc->det, 0, ecc->rec, 0);

	ms_filter_destroy(ecc->sndread);
	ms_filter_destroy(ecc->det);
	ms_filter_destroy(ecc->rec);
	ms_filter_destroy(ecc->play);
	ms_filter_destroy(ecc->gen);
	ms_filter_destroy(ecc->read_resampler);
	ms_filter_destroy(ecc->write_resampler);
	ms_filter_destroy(ecc->sndwrite);

	ms_ticker_destroy(ecc->ticker);

	if (ecc->capt_card) ms_snd_card_unref(ecc->capt_card);
	if (ecc->play_card) ms_snd_card_unref(ecc->play_card);
}

static void on_tone_sent(void *data, BCTBX_UNUSED(MSFilter *f), unsigned int event_id, void *arg) {
	if (event_id == MS_DTMF_GEN_EVENT) {
		MSDtmfGenEvent *ev = (MSDtmfGenEvent *)arg;
		EcCalibrator *ecc = (EcCalibrator *)data;
		if (ev->tone_name[0] != '\0') {
			ecc->acc -= ev->tone_start_time;
			ms_message("Sent tone at %u", (unsigned int)ev->tone_start_time);
			if (strcmp(&ev->tone_name[0], "C") == 0) {
				ecc->tone_start_time[0] = ev->tone_start_time;
			} else if (strcmp(&ev->tone_name[0], "D") == 0) {
				ecc->tone_start_time[1] = ev->tone_start_time;
			} else if (strcmp(&ev->tone_name[0], "E") == 0) {
				ecc->tone_start_time[2] = ev->tone_start_time;
			}
		}
	}
}

static bool_t is_valid_tone(EcCalibrator *ecc, MSToneDetectorEvent *ev) {
	bool_t *toneflag = NULL;
	if (strcmp(ev->tone_name, "freq1") == 0) {
		if (ecc->tone_start_time[1] == 0) {
			ms_message("False tone detection at freq1 (not sent yet), time is %d", (int)ev->tone_start_time);
			return FALSE;
		}
		toneflag = &ecc->freq1;
	} else if (strcmp(ev->tone_name, "freq2") == 0) {
		if (ecc->tone_start_time[2] == 0) {
			ms_message("False tone detection at freq2 (not sent yet), time is %d", (int)ev->tone_start_time);
			return FALSE;
		}
		toneflag = &ecc->freq2;
	} else if (strcmp(ev->tone_name, "freq3") == 0) {
		if (ecc->tone_start_time[0] == 0) {
			ms_message("False tone detection at freq3 (not sent yet), time is %d", (int)ev->tone_start_time);
			return FALSE;
		}
		toneflag = &ecc->freq3;
	} else {
		ms_error("Calibrator bug.");
		return FALSE;
	}
	if (*toneflag) {
		ms_message("Duplicated tone event, ignored.");
		return FALSE;
	}
	*toneflag = TRUE;
	return TRUE;
}

static void on_tone_received(void *data, BCTBX_UNUSED(MSFilter *f), BCTBX_UNUSED(unsigned int event_id), void *arg) {
	MSToneDetectorEvent *ev = (MSToneDetectorEvent *)arg;
	EcCalibrator *ecc = (EcCalibrator *)data;
	if (is_valid_tone(ecc, ev)) {
		ecc->acc += ev->tone_start_time;
		ms_message("Received tone at %u", (unsigned int)ev->tone_start_time);
	}
}

static void ecc_play_tones(EcCalibrator *ecc) {
	MSDtmfGenCustomTone tone;
	MSToneDetectorDef expected_tone;

	memset(&tone, 0, sizeof(tone));
	memset(&expected_tone, 0, sizeof(expected_tone));

	ms_filter_add_notify_callback(ecc->det, on_tone_received, ecc, TRUE);

	/* configure the tones to be scanned */

	strncpy(expected_tone.tone_name, "freq1", sizeof(expected_tone.tone_name));
	expected_tone.frequency = (int)2349.32;
	expected_tone.min_duration = 40;
	expected_tone.min_amplitude = 0.1f;

	ms_filter_call_method(ecc->det, MS_TONE_DETECTOR_ADD_SCAN, &expected_tone);

	strncpy(expected_tone.tone_name, "freq2", sizeof(expected_tone.tone_name));
	expected_tone.frequency = (int)2637.02;
	expected_tone.min_duration = 40;
	expected_tone.min_amplitude = 0.1f;

	ms_filter_call_method(ecc->det, MS_TONE_DETECTOR_ADD_SCAN, &expected_tone);

	strncpy(expected_tone.tone_name, "freq3", sizeof(expected_tone.tone_name));
	expected_tone.frequency = (int)2093;
	expected_tone.min_duration = 40;
	expected_tone.min_amplitude = 0.1f;

	ms_filter_call_method(ecc->det, MS_TONE_DETECTOR_ADD_SCAN, &expected_tone);

	/*play an initial tone to startup the audio playback/capture*/

	tone.frequencies[0] = 140;
	tone.duration = 1000;
	tone.amplitude = 0.5;

	ms_filter_call_method(ecc->gen, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
	ms_sleep(2);

	ms_filter_add_notify_callback(ecc->gen, on_tone_sent, ecc, TRUE);

	/* play the three tones*/

	if (ecc->play_cool_tones) {
		strncpy(tone.tone_name, "D", sizeof(tone.tone_name));
		tone.frequencies[0] = (int)2349.32;
		tone.duration = 100;
		ms_filter_call_method(ecc->gen, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
		ms_usleep(300000);

		strncpy(tone.tone_name, "E", sizeof(tone.tone_name));
		tone.frequencies[0] = (int)2637.02;
		tone.duration = 100;
		ms_filter_call_method(ecc->gen, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
		ms_usleep(300000);

		strncpy(tone.tone_name, "C", sizeof(tone.tone_name));
		tone.frequencies[0] = (int)2093;
		tone.duration = 100;
		ms_filter_call_method(ecc->gen, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
		ms_usleep(300000);
	} else {
		strncpy(tone.tone_name, "C", sizeof(tone.tone_name));
		tone.frequencies[0] = (int)2093;
		tone.duration = 100;
		ms_filter_call_method(ecc->gen, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
		ms_usleep(300000);

		strncpy(tone.tone_name, "D", sizeof(tone.tone_name));
		tone.frequencies[0] = (int)2349.32;
		tone.duration = 100;
		ms_filter_call_method(ecc->gen, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
		ms_usleep(300000);

		strncpy(tone.tone_name, "E", sizeof(tone.tone_name));
		tone.frequencies[0] = (int)2637.02;
		tone.duration = 100;
		ms_filter_call_method(ecc->gen, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
		ms_usleep(300000);
	}

	/*these two next ones are for lyrism*/
	if (ecc->play_cool_tones) {
		tone.tone_name[0] = '\0';
		tone.frequencies[0] = (int)1046.5;
		tone.duration = 400;
		ms_filter_call_method(ecc->gen, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
		ms_usleep(300000);

		tone.tone_name[0] = '\0';
		tone.frequencies[0] = (int)1567.98;
		tone.duration = 400;
		ms_filter_call_method(ecc->gen, MS_DTMF_GEN_PLAY_CUSTOM, &tone);
	}

	ms_sleep(1);

	if (ecc->freq1 && ecc->freq2 && ecc->freq3) {
		int delay = (int)(ecc->acc / 3);
		if (delay < 0) {
			ms_error("Quite surprising calibration result, delay=%i", delay);
			ecc->status = LinphoneEcCalibratorFailed;
		} else {
			// Remove a margin in ms to compensate for long delays in order to increase the probability that the
			// echo is present in the audio sample analyzed by the AEC. The margin cannot be higher than the half
			// window of webRTC AEC.
			delay = delay - MIN((int)((float)delay * 0.2), 40);
			ms_message("Echo calibration estimated delay to be %i ms", delay);
			ecc->delay = delay;
			ecc->status = LinphoneEcCalibratorDone;
		}
	} else if ((ecc->freq1 || ecc->freq2 || ecc->freq3) == FALSE) {
		ms_message("Echo calibration succeeded, no echo has been detected");
		ecc->status = LinphoneEcCalibratorDoneNoEcho;
	} else {
		ecc->status = LinphoneEcCalibratorFailed;
	}

	if (ecc->status == LinphoneEcCalibratorFailed) {
		ms_error("Echo calibration failed.");
	}
}

static void *ecc_thread(void *p) {
	EcCalibrator *ecc = (EcCalibrator *)p;
	ecc_play_tones(ecc);
	ms_thread_exit(NULL);
	return NULL;
}

EcCalibrator *ec_calibrator_new(MSFactory *factory,
                                MSSndCard *play_card,
                                MSSndCard *capt_card,
                                unsigned int rate,
                                LinphoneEcCalibrationCallback cb,
                                LinphoneEcCalibrationAudioInit audio_init_cb,
                                LinphoneEcCalibrationAudioUninit audio_uninit_cb,
                                void *cb_data) {
	EcCalibrator *ecc = ms_new0(EcCalibrator, 1);

	ecc->rate = rate;
	ecc->cb = cb;
	ecc->cb_data = cb_data;
	ecc->audio_init_cb = audio_init_cb;
	ecc->audio_uninit_cb = audio_uninit_cb;
	ecc->capt_card = ms_snd_card_ref(capt_card);
	ecc->play_card = ms_snd_card_ref(play_card);
	ecc->factory = factory;
	ecc->tone_start_time[0] = 0;
	ecc->tone_start_time[1] = 0;
	ecc->tone_start_time[2] = 0;
	return ecc;
}

void ec_calibrator_start(EcCalibrator *ecc) {
	ecc_init_filters(ecc);
	ms_thread_create(&ecc->thread, NULL, ecc_thread, ecc);
}

LinphoneEcCalibratorStatus ec_calibrator_get_status(EcCalibrator *ecc) {
	return ecc->status;
}

void ec_calibrator_destroy(EcCalibrator *ecc) {
	if (ecc->thread != 0) ms_thread_join(ecc->thread, NULL);
	ecc_deinit_filters(ecc);
	ms_free(ecc);
}

static void _ec_calibration_result_cb(LinphoneCore *lc,
                                      LinphoneEcCalibratorStatus status,
                                      int delay_ms,
                                      BCTBX_UNUSED(void *user_data)) {
	linphone_core_notify_ec_calibration_result(lc, status, delay_ms);
	if (status != LinphoneEcCalibratorInProgress) {
		getPlatformHelpers(lc)->stopAudioForEchoTestOrCalibration();
		getPlatformHelpers(lc)->restorePreviousAudioRoute();
	}
}

static void _ec_calibration_audio_init_cb(void *user_data) {
	LinphoneCore *lc = (LinphoneCore *)user_data;
	linphone_core_notify_ec_calibration_audio_init(lc);
}

static void _ec_calibration_audio_uninit_cb(void *user_data) {
	LinphoneCore *lc = (LinphoneCore *)user_data;
	linphone_core_notify_ec_calibration_audio_uninit(lc);
}

LinphoneStatus linphone_core_start_echo_canceller_calibration(LinphoneCore *lc) {
	unsigned int rate;

	if (lc->ecc != NULL) {
		ms_error("Echo calibration is still on going !");
		return -1;
	}

	if (lc->sound_conf.play_sndcard == NULL) {
		ms_error("No playback device configured, can't start echo calibration!");
		return -1;
	}
	if (lc->sound_conf.capt_sndcard == NULL) {
		ms_error("No recording device configured, can't start echo calibration!");
		return -1;
	}

	rate = (unsigned int)linphone_config_get_int(lc->config, "sound", "echo_cancellation_rate", 8000);
	getPlatformHelpers(lc)->routeAudioToSpeaker();
	lc->ecc = ec_calibrator_new(lc->factory, lc->sound_conf.play_sndcard, lc->sound_conf.capt_sndcard, rate,
	                            _ec_calibration_result_cb, _ec_calibration_audio_init_cb,
	                            _ec_calibration_audio_uninit_cb, lc);
	lc->ecc->play_cool_tones = !!linphone_config_get_int(lc->config, "sound", "ec_calibrator_cool_tones", 0);
	ec_calibrator_start(lc->ecc);
	getPlatformHelpers(lc)->startAudioForEchoTestOrCalibration();
	return 0;
}

bool_t linphone_core_has_builtin_echo_canceller(LinphoneCore *lc) {
	MSFactory *factory = linphone_core_get_ms_factory(lc);
	MSDevicesInfo *devices = ms_factory_get_devices_info(factory);
	SoundDeviceDescription *sound_description = ms_devices_info_get_sound_device_description(devices);
	if (sound_description == NULL) return FALSE;
	if (sound_description->flags & DEVICE_HAS_BUILTIN_AEC) return TRUE;
	return FALSE;
}

bool_t linphone_core_is_echo_canceller_calibration_required(LinphoneCore *lc) {
	MSFactory *factory = linphone_core_get_ms_factory(lc);
	MSDevicesInfo *devices = ms_factory_get_devices_info(factory);
	SoundDeviceDescription *sound_description = ms_devices_info_get_sound_device_description(devices);
	if (sound_description == NULL) return TRUE;
	if (sound_description->flags & DEVICE_HAS_BUILTIN_AEC) return FALSE;
	if (sound_description->delay != 0) return FALSE;
	return TRUE;
}
