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

#include <bctoolbox/defs.h>
#include <mediastreamer2/android_utils.h>
#include <mediastreamer2/devices.h>

#include <mediastreamer2/msfilter.h>
#include <mediastreamer2/msjava.h>
#include <mediastreamer2/mssndcard.h>
#include <mediastreamer2/msticker.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#include <dlfcn.h>
#include <jni.h>
#include <string.h>
#include <sys/types.h>

namespace fake_opensles {
SLInterfaceID SLW_IID_ENGINE = NULL;
SLInterfaceID SLW_IID_ANDROIDSIMPLEBUFFERQUEUE = NULL;
SLInterfaceID SLW_IID_ANDROIDCONFIGURATION = NULL;
SLInterfaceID SLW_IID_RECORD = NULL;
SLInterfaceID SLW_IID_VOLUME = NULL;
SLInterfaceID SLW_IID_PLAY = NULL;

typedef SLresult (*OpenSLESConstructor)(
    SLObjectItf *, SLuint32, const SLEngineOption *, SLuint32, const SLInterfaceID *, const SLboolean *);

OpenSLESConstructor slwCreateEngine = NULL;

int findSymbol(void *handle, SLInterfaceID &dest, const char *name) {
	SLInterfaceID *sym = (SLInterfaceID *)dlsym(handle, name);
	const char *error = dlerror();
	if (sym == NULL || error) {
		ms_error("[OpenSLES] Couldn't find %s symbol : %s", name, error);
		return 1;
	}
	dest = *sym;
	return 0;
}

int initOpenSLES() {
	int result = 0;
	void *handle;

	if ((handle = dlopen("libOpenSLES.so", RTLD_NOW)) == NULL) {
		ms_warning("Fail to load libOpenSLES : %s", dlerror());
		result = -1;
	} else {
		dlerror(); // Clear previous message if present

		result += findSymbol(handle, SLW_IID_ENGINE, "SL_IID_ENGINE");
		result += findSymbol(handle, SLW_IID_ANDROIDSIMPLEBUFFERQUEUE, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
		result += findSymbol(handle, SLW_IID_ANDROIDCONFIGURATION, "SL_IID_ANDROIDCONFIGURATION");
		result += findSymbol(handle, SLW_IID_RECORD, "SL_IID_RECORD");
		result += findSymbol(handle, SLW_IID_VOLUME, "SL_IID_VOLUME");
		result += findSymbol(handle, SLW_IID_PLAY, "SL_IID_PLAY");

		slwCreateEngine = (OpenSLESConstructor)dlsym(handle, "slCreateEngine");
		if (slwCreateEngine == NULL) {
			result += 1;
			ms_error("[OpenSLES] Couldn't find slCreateEngine symbol");
		}
	}
	return result;
}
} // namespace fake_opensles

static const int flowControlIntervalMs = 5000;
static const int flowControlThresholdMs = 40;

using namespace fake_opensles;

static void android_snd_card_device_create(JNIEnv *env,
                                           jobject deviceInfo,
                                           SoundDeviceDescription *deviceDescription,
                                           MSSndCardManager *m);
static void snd_card_device_create(int device_id,
                                   const char *name,
                                   MSSndCardDeviceType type,
                                   unsigned int capabilities,
                                   SoundDeviceDescription *deviceDescription,
                                   MSSndCardManager *m);
static MSFilter *ms_android_snd_read_new(MSFactory *factory);
static MSFilter *ms_android_snd_write_new(MSFactory *factory);

struct OpenSLESContext {
	OpenSLESContext() {
		nchannels = 1;
		builtin_aec = false;

		engineObject = NULL;
		engineEngine = NULL;
	}
	void initRuntimeValues(MSFactory *factory) {
		AndroidSoundUtils *asu = ms_factory_get_android_sound_utils(factory);
		buffersize = ms_android_sound_utils_get_preferred_buffer_size(asu);
		samplerate = ms_android_sound_utils_get_preferred_sample_rate(asu);
	}
	int buffersize = 0;
	int samplerate = 0;
	int nchannels;
	bool builtin_aec;

	SLObjectItf engineObject;
	SLEngineItf engineEngine;
};

struct OpenSLESOutputContext {
	OpenSLESOutputContext(MSFilter *f) {
		filter = f;
		streamType = SL_ANDROID_STREAM_VOICE;
		nbufs = 0;
		ms_flow_controlled_bufferizer_init(&buffer, f, 44100, 1);
		ms_mutex_init(&mutex, NULL);

		currentBuffer = 0;
		playBuffer[0] = NULL;
		playBuffer[1] = NULL;

		// Invalidate objects
		outputMixObject = NULL;
		playerObject = NULL;
		playerPlay = NULL;
		playerBufferQueue = NULL;
		playerConfig = NULL;
	}

	~OpenSLESOutputContext() {
		if (playBuffer[0] != NULL) free(playBuffer[0]);
		if (playBuffer[1] != NULL) free(playBuffer[1]);
		ms_flow_controlled_bufferizer_uninit(&buffer);
		ms_mutex_destroy(&mutex);
	}

	void setContext(OpenSLESContext *context) {
		opensles_context = context;
		opensles_context->initRuntimeValues(filter->factory);
		ms_flow_controlled_bufferizer_set_samplerate(&buffer, opensles_context->samplerate);
		ms_flow_controlled_bufferizer_set_nchannels(&buffer, opensles_context->nchannels);
		ms_flow_controlled_bufferizer_set_max_size_ms(&buffer, flowControlThresholdMs);
		ms_flow_controlled_bufferizer_set_flow_control_interval_ms(&buffer, flowControlIntervalMs);
	}

	void updateStreamTypeFromMsSndCard() {
		MSSndCardStreamType type = ms_snd_card_get_stream_type(soundCard);
		streamType = SL_ANDROID_STREAM_VOICE;
		if (type == MS_SND_CARD_STREAM_RING) {
			ms_message("[OpenSLES] Sound card configured for RING stream");
			streamType = SL_ANDROID_STREAM_RING;
		} else if (type == MS_SND_CARD_STREAM_MEDIA) {
			ms_message("[OpenSLES] Sound card configured for MEDIA stream");
			streamType = SL_ANDROID_STREAM_MEDIA;
		} else if (type == MS_SND_CARD_STREAM_DTMF) {
			ms_message("[OpenSLES] Sound card configured for DTMF stream");
			streamType = SL_ANDROID_STREAM_NOTIFICATION;
		} else {
			ms_message("[OpenSLES] Sound card configured for VOICE stream");
		}
	}

	OpenSLESContext *opensles_context;

	SLObjectItf outputMixObject;
	SLObjectItf playerObject;
	SLPlayItf playerPlay;
	SLAndroidSimpleBufferQueueItf playerBufferQueue;
	SLAndroidConfigurationItf playerConfig;
	SLint32 streamType;

	MSSndCard *soundCard;
	MSFilter *filter;
	MSFlowControlledBufferizer buffer;
	int nbufs;
	ms_mutex_t mutex;

	uint8_t *playBuffer[2];
	int outBufSize;
	int currentBuffer;
};

struct OpenSLESInputContext {
	OpenSLESInputContext(MSFilter *f) {
		streamType = SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION;
		qinit(&q);
		ms_mutex_init(&mutex, NULL);
		mTickerSynchronizer = NULL;
		aec = NULL;
		mAvSkew = 0;
		recorderObject = NULL;
		recorderRecord = NULL;
		recorderBufferQueue = NULL;
		recorderConfig = NULL;

		currentBuffer = 0;
		recBuffer[0] = NULL;
		recBuffer[1] = NULL;
		voiceRecognitionMode = false;
		deviceChanged = false;
		mFilter = f;
	}

	~OpenSLESInputContext() {
		if (recBuffer[0] != NULL) free(recBuffer[0]);
		if (recBuffer[1] != NULL) free(recBuffer[1]);
		flushq(&q, 0);
		ms_mutex_destroy(&mutex);
	}

	void setContext(OpenSLESContext *context) {
		opensles_context = context;
		opensles_context->initRuntimeValues(mFilter->factory);
	}

	OpenSLESContext *opensles_context;

	SLObjectItf recorderObject;
	SLRecordItf recorderRecord;
	SLAndroidSimpleBufferQueueItf recorderBufferQueue;
	SLAndroidConfigurationItf recorderConfig;
	SLint32 streamType;

	queue_t q;
	ms_mutex_t mutex;
	MSTickerSynchronizer *mTickerSynchronizer;
	MSSndCard *soundCard;
	MSFilter *mFilter;
	int64_t read_samples;
	jobject aec;

	uint8_t *recBuffer[2];
	int inBufSize;
	int currentBuffer;
	double mAvSkew;
	bool voiceRecognitionMode;
	bool deviceChanged;
};

static SLuint32 convertSamplerate(int samplerate) {
	switch (samplerate) {
		case 8000:
			return SL_SAMPLINGRATE_8;
			break;
		case 16000:
			return SL_SAMPLINGRATE_16;
			break;
		case 22050:
			return SL_SAMPLINGRATE_22_05;
			break;
		case 32000:
			return SL_SAMPLINGRATE_32;
			break;
		case 44100:
			return SL_SAMPLINGRATE_44_1;
			break;
		case 48000:
			return SL_SAMPLINGRATE_48;
			break;
		default:
			return -1;
	}
}

static void android_snd_card_add_devices(BCTBX_UNUSED(MSDevicesInfo *devices),
                                         SoundDeviceDescription *deviceDescription,
                                         MSSndCardManager *m) {
	JNIEnv *env = ms_get_jni_env();
	int sdkVersion = ms_android_get_sdk_version(env);

	// GetDevices is only available from API23
	if (sdkVersion >= 23) {
		// Get all devices
		jobject devices = ms_android_get_all_devices(env, "all");

		// extract required information from every device
		jobjectArray deviceArray = reinterpret_cast<jobjectArray>(devices);
		jsize deviceNumber = (int)env->GetArrayLength(deviceArray);
		ms_message("[OpenSLES] Create soundcards for %0d devices", deviceNumber);

		for (int idx = 0; idx < deviceNumber; idx++) {
			jobject deviceInfo = env->GetObjectArrayElement(deviceArray, idx);
			android_snd_card_device_create(env, deviceInfo, deviceDescription, m);
		}
	} else {
		// For devices running API older than API23, only 3 devices are created: microphone, speaker and earpiece
		snd_card_device_create(-1, "Microphone", MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_MICROPHONE,
		                       MS_SND_CARD_CAP_CAPTURE, deviceDescription, m);
		snd_card_device_create(-1, "Earpiece", MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_EARPIECE,
		                       MS_SND_CARD_CAP_PLAYBACK, deviceDescription, m);
		snd_card_device_create(-1, "Speaker", MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_SPEAKER,
		                       MS_SND_CARD_CAP_PLAYBACK, deviceDescription, m);
	}
}

static void android_snd_card_detect(MSSndCardManager *m) {
	SoundDeviceDescription *deviceDescription = NULL;
	MSDevicesInfo *devices = NULL;

	if (initOpenSLES() == 0) { // Try to dlopen libOpenSLES
		ms_message("[OpenSLES] libOpenSLES correctly loaded, creating OpenSLES MS soundcard");
		devices = ms_factory_get_devices_info(m->factory);
		deviceDescription = ms_devices_info_get_sound_device_description(devices);
		if (deviceDescription->flags & DEVICE_HAS_CRAPPY_OPENSLES) return;

		android_snd_card_add_devices(devices, deviceDescription, m);
	} else {
		ms_warning("[OpenSLES] Failed to dlopen libOpenSLES, OpenSLES MS soundcard unavailable");
	}
}

static SLresult opensles_engine_init(OpenSLESContext *ctx) {
	SLresult result;

	result = slwCreateEngine(&(ctx->engineObject), 0, NULL, 0, NULL, NULL);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while creating SL engine", result);
		return result;
	}

	result = (*ctx->engineObject)->Realize(ctx->engineObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while realizing SL engine", result);
		return result;
	}

	result = (*ctx->engineObject)->GetInterface(ctx->engineObject, SLW_IID_ENGINE, &(ctx->engineEngine));

	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while getting SL engine interface", result);
		return result;
	}

	return result;
}

static void android_native_snd_card_init(MSSndCard *card) {
	OpenSLESContext *context = new OpenSLESContext();
	opensles_engine_init(context);
	card->data = context;
}

static void android_native_snd_card_uninit(MSSndCard *card) {
	OpenSLESContext *ctx = (OpenSLESContext *)card->data;
	ms_message("[OpenSLES] Deleting card [%p]: name [%s] device ID [%0d] type [%s]", card, card->name,
	           card->internal_id, ms_snd_card_device_type_to_string(card->device_type));

	if (ctx->engineObject != NULL) {
		(*ctx->engineObject)->Destroy(ctx->engineObject);
		ctx->engineObject = NULL;
		ctx->engineEngine = NULL;
	}

	ms_message("[OpenSLES] Deletion of OpenSLES context [%p]", ctx);
	delete ctx;
}

static void android_snd_card_usage_hint(MSSndCard *card, bool_t used) {
	if (!used) {
		if ((card->capabilities & MS_SND_CARD_CAP_PLAYBACK) == MS_SND_CARD_CAP_PLAYBACK) {
			ms_message(
			    "[OpenSLES] Sound card not needed anymore, making sure the earpiece is the default audio route in "
			    "Android's AudioManager");
			JNIEnv *env = ms_get_jni_env();
			ms_android_change_device(env, -1, MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_EARPIECE);
		}
	}
}

static void opensles_reset_recorder(OpenSLESInputContext *ictx) {
	if (ictx->recorderObject != NULL) {
		(*ictx->recorderObject)->Destroy(ictx->recorderObject);
		ictx->recorderObject = NULL;
		ictx->recorderRecord = NULL;
		ictx->recorderBufferQueue = NULL;
	}
}

static SLresult opensles_recorder_init(OpenSLESInputContext *ictx) {
	SLresult result;
	SLuint32 sample_rate = convertSamplerate(ictx->opensles_context->samplerate);
	SLuint32 channels = (SLuint32)ictx->opensles_context->nchannels;

	SLDataLocator_IODevice loc_dev = {SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT, SL_DEFAULTDEVICEID_AUDIOINPUT,
	                                  NULL};

	SLDataSource audio_src = {&loc_dev, NULL};

	SLDataLocator_AndroidSimpleBufferQueue loc_bq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};

	SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM,
	                               channels,
	                               sample_rate,
	                               SL_PCMSAMPLEFORMAT_FIXED_16,
	                               SL_PCMSAMPLEFORMAT_FIXED_16,
	                               SL_SPEAKER_FRONT_CENTER,
	                               SL_BYTEORDER_LITTLEENDIAN};

	SLDataSink audio_sink = {&loc_bq, &format_pcm};

	const SLInterfaceID ids[] = {SLW_IID_ANDROIDSIMPLEBUFFERQUEUE, SLW_IID_ANDROIDCONFIGURATION};

	const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};

	result = (*ictx->opensles_context->engineEngine)
	             ->CreateAudioRecorder(ictx->opensles_context->engineEngine, &ictx->recorderObject, &audio_src,
	                                   &audio_sink, 2, ids, req);
	if (SL_RESULT_SUCCESS != result) {
		opensles_reset_recorder(ictx);
		ms_error("[OpenSLES] Error %u while creating the audio recorder", result);
		return result;
	}

	result = (*ictx->recorderObject)
	             ->GetInterface(ictx->recorderObject, SLW_IID_ANDROIDCONFIGURATION, &ictx->recorderConfig);
	if (SL_RESULT_SUCCESS != result) {
		opensles_reset_recorder(ictx);
		ms_error("[OpenSLES] Error %u while getting the recorder's android config interface", result);
		return result;
	}

	if (ictx->voiceRecognitionMode) {
		// Voice recognition preset works better when recording voice message
		ms_message("[OpenSLES] Using preset voice recognition (%i) instead of %i",
		           SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION, ictx->streamType);
		ictx->streamType = SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
	}
	result = (*ictx->recorderConfig)
	             ->SetConfiguration(ictx->recorderConfig, SL_ANDROID_KEY_RECORDING_PRESET, &ictx->streamType,
	                                sizeof(SLint32));
	if (SL_RESULT_SUCCESS != result) {
		opensles_reset_recorder(ictx);
		ms_error("[OpenSLES] Error %u while setting the audio recorder configuration", result);
		return result;
	}

	result = (*ictx->recorderObject)->Realize(ictx->recorderObject, SL_BOOLEAN_FALSE);
	if (SL_RESULT_SUCCESS != result) {
		opensles_reset_recorder(ictx);
		ms_error("[OpenSLES] Error %u while realizing the audio recorder", result);
		return result;
	}

	result = (*ictx->recorderObject)->GetInterface(ictx->recorderObject, SLW_IID_RECORD, &ictx->recorderRecord);
	if (SL_RESULT_SUCCESS != result) {
		opensles_reset_recorder(ictx);
		ms_error("[OpenSLES] Error %u while getting the audio recorder's interface", result);
		return result;
	}

	result = (*ictx->recorderObject)
	             ->GetInterface(ictx->recorderObject, SLW_IID_ANDROIDSIMPLEBUFFERQUEUE, &ictx->recorderBufferQueue);
	if (SL_RESULT_SUCCESS != result) {
		opensles_reset_recorder(ictx);
		ms_error("[OpenSLES] Error %u while getting the audio recorder's buffer interface", result);
		return result;
	}

	return result;
}

/*
 * This is a callback function called by AudioRecord's thread. This thread is not created by ortp/ms2 and is not able to
 *attach to a JVM without crashing at the end, despite it is detached (since android 4.4). We must not output a single
 *log within this callback in the event that the application is using LinphoneCoreFactory.setLogHandler(), in which case
 * the log would be upcalled to java, which will attach the thread to the jvm.
 **/
static void opensles_recorder_callback(BCTBX_UNUSED(SLAndroidSimpleBufferQueueItf bq), void *context) {
	SLresult result;
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)context;
	ictx->read_samples += ictx->inBufSize / sizeof(int16_t);

	mblk_t *m = allocb(ictx->inBufSize, 0);
	memcpy(m->b_wptr, ictx->recBuffer[ictx->currentBuffer], ictx->inBufSize);
	m->b_wptr += ictx->inBufSize;

	ms_mutex_lock(&ictx->mutex);
	putq(&ictx->q, m);
	ms_mutex_unlock(&ictx->mutex);

	result = (*ictx->recorderBufferQueue)
	             ->Enqueue(ictx->recorderBufferQueue, ictx->recBuffer[ictx->currentBuffer], ictx->inBufSize);
	if (result != SL_RESULT_SUCCESS) {
		/*ms_error("OpenSLES Error %u while enqueueing record buffer", result);*/
	}
	ictx->currentBuffer = ictx->currentBuffer == 1 ? 0 : 1;
}

static SLresult opensles_recorder_callback_init(OpenSLESInputContext *ictx) {
	SLresult result;

	result =
	    (*ictx->recorderBufferQueue)->RegisterCallback(ictx->recorderBufferQueue, opensles_recorder_callback, ictx);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("[OpenSLES] Error %u while realizing the audio recorder", result);
		return result;
	}

	// in case already recording, stop recording and clear buffer queue
	result = (*ictx->recorderRecord)->SetRecordState(ictx->recorderRecord, SL_RECORDSTATE_STOPPED);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("[OpenSLES] Error %u while stopping the audio recorder", result);
		return result;
	}

	result = (*ictx->recorderBufferQueue)->Clear(ictx->recorderBufferQueue);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("[OpenSLES] Error %u while clearing the audio recorder buffer queue", result);
		return result;
	}

	result = (*ictx->recorderRecord)->SetRecordState(ictx->recorderRecord, SL_RECORDSTATE_RECORDING);
	if (SL_RESULT_SUCCESS != result) {
		ms_error("[OpenSLES] Error %u while starting the audio recorder", result);
		return result;
	}

	result = (*ictx->recorderBufferQueue)->Enqueue(ictx->recorderBufferQueue, ictx->recBuffer[0], ictx->inBufSize);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while enqueueing record buffer", result);
	}

	result = (*ictx->recorderBufferQueue)->Enqueue(ictx->recorderBufferQueue, ictx->recBuffer[1], ictx->inBufSize);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while enqueueing record buffer", result);
	}

	return result;
}

static void android_snd_read_init(MSFilter *obj) {
	OpenSLESInputContext *ictx = new OpenSLESInputContext(obj);
	obj->data = ictx;

	bool permissionGranted =
	    ms_android_sound_utils_is_record_audio_permission_granted(ms_factory_get_android_sound_utils(obj->factory));
	if (!permissionGranted) {
		ms_error("[OpenSLES] RECORD_AUDIO permission hasn't been granted!");
	}
}

static void android_snd_read_preprocess(MSFilter *obj) {
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;
	ictx->read_samples = 0;

	ictx->inBufSize = ictx->opensles_context->buffersize * sizeof(int16_t) * ictx->opensles_context->nchannels;
	ictx->recBuffer[0] = (uint8_t *)calloc(ictx->inBufSize, sizeof(uint8_t));
	ictx->recBuffer[1] = (uint8_t *)calloc(ictx->inBufSize, sizeof(uint8_t));

	if (ictx->mTickerSynchronizer == NULL) {
		ictx->mTickerSynchronizer = ms_ticker_synchronizer_new();
		ms_ticker_set_synchronizer(obj->ticker, ictx->mTickerSynchronizer);
	}

	if (SL_RESULT_SUCCESS != opensles_recorder_init(ictx)) {
		ms_error("[OpenSLES] Problem when initialization of opensles recorder");
		return;
	}
	if (SL_RESULT_SUCCESS != opensles_recorder_callback_init(ictx)) {
		ms_error("[OpenSLES] Problem when initialization of opensles recorder callback");
		return;
	}

	if (ictx->opensles_context->builtin_aec) {
		// android_snd_read_activate_hardware_aec(obj);
	}
}

static void android_snd_read_process(MSFilter *obj) {
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;
	mblk_t *m;
	if (ictx->deviceChanged && ictx->mTickerSynchronizer) {
		ms_mutex_lock(&ictx->mutex);
		ms_ticker_synchronizer_resync(ictx->mTickerSynchronizer);
		ms_message("[OpenSLES] resync ticket synchronizer to avoid audio delay");
		ictx->deviceChanged = false;
		ms_mutex_unlock(&ictx->mutex);
	}

	if (obj->ticker->time % 1000 == 0) {
		if (ictx->recorderBufferQueue == NULL) {
			ms_message("[OpenSLES] Trying to init opensles recorder on process");
			if (SL_RESULT_SUCCESS != opensles_recorder_init(ictx)) {
				ms_error("[OpenSLES] Problem when initialization of opensles recorder");
			} else if (SL_RESULT_SUCCESS != opensles_recorder_callback_init(ictx)) {
				ms_error("[OpenSLES] Problem when initialization of opensles recorder callback");
			}
		}
	}

	ms_mutex_lock(&ictx->mutex);
	while ((m = getq(&ictx->q)) != NULL) {
		ms_queue_put(obj->outputs[0], m);
	}
	ictx->mAvSkew = ms_ticker_synchronizer_update(ictx->mTickerSynchronizer, ictx->read_samples,
	                                              (unsigned int)ictx->opensles_context->samplerate);
	ms_mutex_unlock(&ictx->mutex);
	if (obj->ticker->time % 5000 == 0) ms_message("[OpenSLES] sound/wall clock skew is average=%g ms", ictx->mAvSkew);
}

static void android_snd_read_postprocess(MSFilter *obj) {
	SLresult result;
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;

	if (ictx->aec) {
		JNIEnv *env = ms_get_jni_env();
		env->DeleteGlobalRef(ictx->aec);
		ictx->aec = NULL;
	}

	if (ictx->recorderRecord != NULL) {
		result = (*ictx->recorderRecord)->SetRecordState(ictx->recorderRecord, SL_RECORDSTATE_STOPPED);
		if (SL_RESULT_SUCCESS != result) {
			ms_error("[OpenSLES] Error %u while stopping the audio recorder", result);
		}
	}

	if (ictx->recorderBufferQueue != NULL) {
		result = (*ictx->recorderBufferQueue)->Clear(ictx->recorderBufferQueue);
		if (SL_RESULT_SUCCESS != result) {
			ms_error("[OpenSLES] Error %u while clearing the audio recorder buffer queue", result);
		}
	}

	if (ictx->recorderObject != NULL) {
		(*ictx->recorderObject)->Destroy(ictx->recorderObject);
		ictx->recorderObject = NULL;
		ictx->recorderRecord = NULL;
		ictx->recorderBufferQueue = NULL;
	}

	ms_ticker_set_synchronizer(obj->ticker, NULL);
	ms_mutex_lock(&ictx->mutex);
	if (ictx->mTickerSynchronizer) {
		ms_ticker_synchronizer_destroy(ictx->mTickerSynchronizer);
		ictx->mTickerSynchronizer = NULL;
	}
	ms_mutex_unlock(&ictx->mutex);
}

static void android_snd_read_uninit(MSFilter *obj) {
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;

	if (ictx->soundCard) {
		ms_snd_card_unref(ictx->soundCard);
		ictx->soundCard = NULL;
	}

	delete ictx;
}

static int android_snd_read_set_sample_rate(BCTBX_UNUSED(MSFilter *obj), BCTBX_UNUSED(void *data)) {
#if 0
	int *n = (int*)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext*)obj->data;
	if (ictx->opensles_context->forced_sample_rate > 0) {
		ms_warning("Sample rate is forced by mediastreamer2 device table, skipping...");
		return -1;
	}
	ictx->opensles_context->samplerate = *n;
	return 0;
#endif
	return -1; /*don't accept custom sample rates, use recommended rate always*/
}

static int android_snd_read_get_sample_rate(MSFilter *obj, void *data) {
	int *n = (int *)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;
	*n = ictx->opensles_context->samplerate;
	return 0;
}

static int android_snd_read_set_nchannels(MSFilter *obj, void *data) {
	int *n = (int *)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;
	ictx->opensles_context->nchannels = *n;
	return 0;
}

static int android_snd_read_get_nchannels(MSFilter *obj, void *data) {
	int *n = (int *)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;
	*n = ictx->opensles_context->nchannels;
	return 0;
}

static int android_snd_read_configure_soundcard(MSFilter *obj, void *data) {
	MSSndCard *card = (MSSndCard *)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;
	// Check if device_id is different and/or device_type is different
	// For API < 23, all device ID are identical but the device type is different
	if ((ictx->soundCard->internal_id != card->internal_id) || (ictx->soundCard->device_type != card->device_type)) {
		ms_mutex_lock(&ictx->mutex);
		if (ictx->soundCard) {
			ms_snd_card_unref(ictx->soundCard);
			ictx->soundCard = NULL;
		}
		ictx->soundCard = ms_snd_card_ref(card);
		OpenSLESContext *opensles_context = (OpenSLESContext *)card->data;
		ictx->setContext(opensles_context);
		ictx->deviceChanged = true;
		ms_mutex_unlock(&ictx->mutex);
	}
	return 0;
}

static int android_snd_read_get_device_id(MSFilter *obj, void *data) {
	int *n = (int *)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;
	*n = ictx->soundCard->internal_id;
	return 0;
}

static int android_snd_read_hack_speaker_state(BCTBX_UNUSED(MSFilter *obj), BCTBX_UNUSED(void *data)) {
	return 0;
}

static int android_snd_read_enable_voice_rec(MSFilter *obj, void *data) {
	bool *enabled = (bool *)data;
	OpenSLESInputContext *ictx = (OpenSLESInputContext *)obj->data;
	ictx->voiceRecognitionMode = !!(*enabled);
	return 0;
}

static MSFilterMethod android_snd_read_methods[] = {
    {MS_FILTER_SET_SAMPLE_RATE, android_snd_read_set_sample_rate},
    {MS_FILTER_GET_SAMPLE_RATE, android_snd_read_get_sample_rate},
    {MS_FILTER_SET_NCHANNELS, android_snd_read_set_nchannels},
    {MS_FILTER_GET_NCHANNELS, android_snd_read_get_nchannels},
    {MS_AUDIO_CAPTURE_FORCE_SPEAKER_STATE, android_snd_read_hack_speaker_state},
    {MS_AUDIO_CAPTURE_SET_INTERNAL_ID, android_snd_read_configure_soundcard},
    {MS_AUDIO_CAPTURE_GET_INTERNAL_ID, android_snd_read_get_device_id},
    {MS_AUDIO_CAPTURE_ENABLE_VOICE_REC, android_snd_read_enable_voice_rec},
    {0, NULL}};

MSFilterDesc android_snd_opensles_read_desc = {MS_FILTER_PLUGIN_ID,
                                               "MSOpenSLESRecorder",
                                               "android sound source",
                                               MS_FILTER_OTHER,
                                               NULL,
                                               0,
                                               1,
                                               android_snd_read_init,
                                               android_snd_read_preprocess,
                                               android_snd_read_process,
                                               android_snd_read_postprocess,
                                               android_snd_read_uninit,
                                               android_snd_read_methods};

static MSFilter *ms_android_snd_read_new(MSFactory *factory) {
	MSFilter *f = ms_factory_create_filter_from_desc(factory, &android_snd_opensles_read_desc);
	return f;
}

static MSFilter *android_snd_card_create_reader(MSSndCard *card) {
	MSFilter *f = ms_android_snd_read_new(ms_snd_card_get_factory(card));
	OpenSLESInputContext *ictx = static_cast<OpenSLESInputContext *>(f->data);
	ictx->soundCard = ms_snd_card_ref(card);
	ictx->setContext((OpenSLESContext *)card->data);
	return f;
}

static SLresult opensles_mixer_init(OpenSLESOutputContext *octx) {
	SLresult result;
	const SLuint32 nbInterface = 0;
	const SLInterfaceID ids[] = {};
	const SLboolean req[] = {};
	result =
	    (*octx->opensles_context->engineEngine)
	        ->CreateOutputMix(octx->opensles_context->engineEngine, &(octx->outputMixObject), nbInterface, ids, req);
	if (result != SL_RESULT_SUCCESS) {

		ms_error("[OpenSLES] Error %u while creating output mixer", result);
		return result;
	}

	result = (*octx->outputMixObject)->Realize(octx->outputMixObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS) {
		if (octx->outputMixObject != NULL) {
			octx->outputMixObject = NULL;
		}
		ms_error("[OpenSLES] Error %u while realizing output mixer", result);
		return result;
	}

	return result;
}

static void opensles_reset_sink(OpenSLESOutputContext *octx) {
	if (octx->playerObject != NULL) {
		(*octx->playerObject)->Destroy(octx->playerObject);
		octx->playerObject = NULL;
		octx->playerPlay = NULL;
		octx->playerBufferQueue = NULL;
	}
}

static SLresult opensles_sink_init(OpenSLESOutputContext *octx) {
	SLresult result;
	SLuint32 sample_rate = convertSamplerate(octx->opensles_context->samplerate);
	SLuint32 channels = (SLuint32)octx->opensles_context->nchannels;
	SLDataFormat_PCM format_pcm;

	format_pcm.formatType = SL_DATAFORMAT_PCM;
	format_pcm.numChannels = channels;
	format_pcm.samplesPerSec = sample_rate;
	format_pcm.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
	format_pcm.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
	format_pcm.endianness = SL_BYTEORDER_LITTLEENDIAN;
	if (channels == 1) {
		format_pcm.channelMask = SL_SPEAKER_FRONT_CENTER;
	} else if (channels == 2) {
		format_pcm.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
	} else {
		ms_error("[OpenSLES] Error trying to use %i channels", channels);
	}

	SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};

	SLDataSource audio_src = {&loc_bufq, &format_pcm};

	SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, octx->outputMixObject};

	SLDataSink audio_sink = {&loc_outmix, NULL};

	const SLuint32 nbInterface = 3;
	const SLInterfaceID ids[] = {SLW_IID_VOLUME, SLW_IID_ANDROIDSIMPLEBUFFERQUEUE, SLW_IID_ANDROIDCONFIGURATION};
	const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
	result = (*octx->opensles_context->engineEngine)
	             ->CreateAudioPlayer(octx->opensles_context->engineEngine, &(octx->playerObject), &audio_src,
	                                 &audio_sink, nbInterface, ids, req);
	if (result != SL_RESULT_SUCCESS) {
		opensles_reset_sink(octx);
		ms_error("[OpenSLES] Error %u while creating ouput audio player", result);
		return result;
	}

	result = (*octx->playerObject)->GetInterface(octx->playerObject, SLW_IID_ANDROIDCONFIGURATION, &octx->playerConfig);
	if (result != SL_RESULT_SUCCESS) {
		opensles_reset_sink(octx);
		ms_error("[OpenSLES] Error %u while getting android configuration interface", result);
		return result;
	}

	octx->updateStreamTypeFromMsSndCard();
	result = (*octx->playerConfig)
	             ->SetConfiguration(octx->playerConfig, SL_ANDROID_KEY_STREAM_TYPE, &octx->streamType, sizeof(SLint32));
	if (result != SL_RESULT_SUCCESS) {
		opensles_reset_sink(octx);
		ms_error("[OpenSLES] Error %u while setting stream type configuration", result);
		return result;
	}

	result = (*octx->playerObject)->Realize(octx->playerObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS) {
		opensles_reset_sink(octx);
		ms_error("[OpenSLES] Error %u while realizing output sink", result);
		return result;
	}

	result = (*octx->playerObject)->GetInterface(octx->playerObject, SLW_IID_PLAY, &(octx->playerPlay));
	if (result != SL_RESULT_SUCCESS) {
		opensles_reset_sink(octx);
		ms_error("[OpenSLES] Error %u while getting output sink interface play", result);
		return result;
	}

	result = (*octx->playerObject)
	             ->GetInterface(octx->playerObject, SLW_IID_ANDROIDSIMPLEBUFFERQUEUE, &(octx->playerBufferQueue));
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while getting output sink interface buffer queue", result);
		return result;
	}

	return result;
}

/*
 * This is a callback function called by AudioTrack's thread. This thread is not created by ortp/ms2 and is not able to
 *attach to a JVM without crashing at the end, despite it is detached (since android 4.4). We must not output a single
 *log within this callback in the event that the application is using LinphoneCoreFactory.setLogHandler(), in which case
 * the log would be upcalled to java, which will attach the thread to the jvm.
 **/

static void opensles_player_callback(BCTBX_UNUSED(SLAndroidSimpleBufferQueueItf bq), void *context) {
	SLresult result;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)context;

	ms_mutex_lock(&octx->mutex);
	int ask = octx->outBufSize;
	int avail = ms_flow_controlled_bufferizer_get_avail(&octx->buffer);
	int bytes = MIN(ask, avail);

	if (bytes > 0) {
		bytes = ms_flow_controlled_bufferizer_read(&octx->buffer, octx->playBuffer[octx->currentBuffer], bytes);
	} else {
		/* We have an underrun (no more samples to deliver to the callback). We need to provide soundcard with a silence
		 * buffer */
		bytes = ask;
		memset(octx->playBuffer[octx->currentBuffer], 0, bytes);
	}
	ms_mutex_unlock(&octx->mutex);
	octx->nbufs++;

	result = (*octx->playerBufferQueue)->Enqueue(octx->playerBufferQueue, octx->playBuffer[octx->currentBuffer], bytes);
	if (result != SL_RESULT_SUCCESS) {
		/*ms_error("OpenSLES Error %u while adding buffer to output queue", result);*/
	}
	octx->currentBuffer = octx->currentBuffer == 1 ? 0 : 1;
}

static SLresult opensles_player_callback_init(OpenSLESOutputContext *octx) {
	SLresult result;

	result = (*octx->playerPlay)->SetPlayState(octx->playerPlay, SL_PLAYSTATE_STOPPED);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while stopping player", result);
		return result;
	}

	result = (*octx->playerBufferQueue)->Clear(octx->playerBufferQueue);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while clearing player buffer queue", result);
		return result;
	}

	result = (*octx->playerBufferQueue)->RegisterCallback(octx->playerBufferQueue, opensles_player_callback, octx);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while registering player callback", result);
		return result;
	}

	result = (*octx->playerBufferQueue)->Enqueue(octx->playerBufferQueue, octx->playBuffer[0], octx->outBufSize);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while adding buffer to output queue", result);
	}

	result = (*octx->playerBufferQueue)->Enqueue(octx->playerBufferQueue, octx->playBuffer[1], octx->outBufSize);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while adding buffer to output queue", result);
	}

	result = (*octx->playerPlay)->SetPlayState(octx->playerPlay, SL_PLAYSTATE_PLAYING);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Error %u while starting player", result);
		return result;
	}

	return result;
}

static MSFilter *android_snd_card_create_writer(MSSndCard *card) {
	MSFilter *f = ms_android_snd_write_new(ms_snd_card_get_factory(card));
	OpenSLESOutputContext *octx = static_cast<OpenSLESOutputContext *>(f->data);
	octx->soundCard = ms_snd_card_ref(card);
	octx->setContext((OpenSLESContext *)card->data);
	return f;
}

static void android_snd_write_init(MSFilter *obj) {
	OpenSLESOutputContext *octx = new OpenSLESOutputContext(obj);
	obj->data = octx;
}

static void android_snd_write_uninit(MSFilter *obj) {
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)obj->data;

	if (octx->soundCard) {
		ms_snd_card_unref(octx->soundCard);
		octx->soundCard = NULL;
	}

	delete octx;
}

static int android_snd_write_set_sample_rate(BCTBX_UNUSED(MSFilter *obj), BCTBX_UNUSED(void *data)) {
#if 0
	int *n = (int*)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext*)obj->data;
	if (octx->opensles_context->forced_sample_rate > 0) {
		ms_warning("Sample rate is forced by mediastreamer2 device table, skipping...");
		return -1;
	}
	octx->opensles_context->samplerate = *n;
	return 0;
#endif
	return -1; /*don't accept custom sample rates, use recommended rate always*/
}

static int android_snd_write_get_sample_rate(MSFilter *obj, void *data) {
	int *n = (int *)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)obj->data;
	*n = octx->opensles_context->samplerate;
	return 0;
}

static int android_snd_write_set_nchannels(MSFilter *obj, void *data) {
	int *n = (int *)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)obj->data;
	octx->opensles_context->nchannels = *n;
	ms_flow_controlled_bufferizer_set_nchannels(&octx->buffer, octx->opensles_context->nchannels);
	return 0;
}

static int android_snd_write_get_nchannels(MSFilter *obj, void *data) {
	int *n = (int *)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)obj->data;
	*n = octx->opensles_context->nchannels;
	return 0;
}

static int android_snd_write_configure_soundcard(MSFilter *obj, void *data) {
	MSSndCard *card = (MSSndCard *)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)obj->data;

	// Check if device_id is different and/or device_type is different
	// For API < 23, all device ID are identical but the device type is different
	if ((octx->soundCard->internal_id != card->internal_id) || (octx->soundCard->device_type != card->device_type)) {
		ms_mutex_lock(&octx->mutex);
		if (octx->soundCard) {
			ms_snd_card_unref(octx->soundCard);
			octx->soundCard = NULL;
		}
		octx->soundCard = ms_snd_card_ref(card);
		octx->setContext((OpenSLESContext *)card->data);
		JNIEnv *env = ms_get_jni_env();
		ms_android_change_device(env, card->internal_id, card->device_type);
		ms_mutex_unlock(&octx->mutex);
	}
	return 0;
}

static int android_snd_write_get_device_id(MSFilter *obj, void *data) {
	int *n = (int *)data;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)obj->data;
	*n = octx->soundCard->internal_id;
	return 0;
}

static void android_snd_write_preprocess(MSFilter *obj) {
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)obj->data;
	SLresult result;

	octx->outBufSize = octx->opensles_context->buffersize * sizeof(int16_t) * octx->opensles_context->nchannels;
	octx->playBuffer[0] = (uint8_t *)calloc(octx->outBufSize, sizeof(uint8_t));
	octx->playBuffer[1] = (uint8_t *)calloc(octx->outBufSize, sizeof(uint8_t));

	result = opensles_mixer_init(octx);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Couldn't init OpenSLES mixer");
		return;
	}
	result = opensles_sink_init(octx);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Couldn't init OpenSLES sink");
		return;
	}
	result = opensles_player_callback_init(octx);
	if (result != SL_RESULT_SUCCESS) {
		ms_error("[OpenSLES] Couldn't init OpenSLES player");
		return;
	}

	octx->nbufs = 0;

	// Ensure consistency between the soundcard the core is setting and the one actually used as an output
	JNIEnv *env = ms_get_jni_env();
	ms_android_change_device(env, octx->soundCard->internal_id, octx->soundCard->device_type);
}

static void android_snd_write_process(MSFilter *obj) {
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)obj->data;
	ms_mutex_lock(&octx->mutex);

	ms_flow_controlled_bufferizer_put_from_queue(&octx->buffer, obj->inputs[0]);
	ms_mutex_unlock(&octx->mutex);
}

static void android_snd_write_postprocess(MSFilter *obj) {
	SLresult result;
	OpenSLESOutputContext *octx = (OpenSLESOutputContext *)obj->data;

	if (octx->playerPlay) {
		result = (*octx->playerPlay)->SetPlayState(octx->playerPlay, SL_PLAYSTATE_STOPPED);
		if (result != SL_RESULT_SUCCESS) {
			ms_error("[OpenSLES] Error %u while stopping player", result);
		}
	}

	if (octx->playerBufferQueue) {
		result = (*octx->playerBufferQueue)->Clear(octx->playerBufferQueue);
		if (result != SL_RESULT_SUCCESS) {
			ms_error("[OpenSLES] Error %u while clearing player buffer queue", result);
		}
	}

	if (octx->playerObject != NULL) {
		(*octx->playerObject)->Destroy(octx->playerObject);
		octx->playerObject = NULL;
		octx->playerPlay = NULL;
		octx->playerBufferQueue = NULL;
	}

	if (octx->outputMixObject != NULL) {
		(*octx->outputMixObject)->Destroy(octx->outputMixObject);
		octx->outputMixObject = NULL;
	}

	free(octx->playBuffer[0]);
	octx->playBuffer[0] = NULL;
	free(octx->playBuffer[1]);
	octx->playBuffer[1] = NULL;
}

static MSFilterMethod android_snd_write_methods[] = {
    {MS_FILTER_SET_SAMPLE_RATE, android_snd_write_set_sample_rate},
    {MS_FILTER_GET_SAMPLE_RATE, android_snd_write_get_sample_rate},
    {MS_FILTER_SET_NCHANNELS, android_snd_write_set_nchannels},
    {MS_FILTER_GET_NCHANNELS, android_snd_write_get_nchannels},
    {MS_AUDIO_PLAYBACK_SET_INTERNAL_ID, android_snd_write_configure_soundcard},
    {MS_AUDIO_PLAYBACK_GET_INTERNAL_ID, android_snd_write_get_device_id},
    {0, NULL}};

MSFilterDesc android_snd_opensles_write_desc = {MS_FILTER_PLUGIN_ID,
                                                "MSOpenSLESPlayer",
                                                "android sound output",
                                                MS_FILTER_OTHER,
                                                NULL,
                                                1,
                                                0,
                                                android_snd_write_init,
                                                android_snd_write_preprocess,
                                                android_snd_write_process,
                                                android_snd_write_postprocess,
                                                android_snd_write_uninit,
                                                android_snd_write_methods};

static MSFilter *ms_android_snd_write_new(MSFactory *factory) {
	MSFilter *f = ms_factory_create_filter_from_desc(factory, &android_snd_opensles_write_desc);
	return f;
}

MSSndCardDesc android_native_snd_opensles_card_desc = {.driver_type = "openSLES",
                                                       .detect = android_snd_card_detect,
                                                       .init = android_native_snd_card_init,
                                                       .create_reader = android_snd_card_create_reader,
                                                       .create_writer = android_snd_card_create_writer,
                                                       .uninit = android_native_snd_card_uninit,
                                                       .usage_hint = android_snd_card_usage_hint};

static void snd_card_device_create_extra_fields(BCTBX_UNUSED(MSSndCardManager *m),
                                                MSSndCard *card,
                                                SoundDeviceDescription *deviceDescription) {
	OpenSLESContext *card_data = (OpenSLESContext *)card->data;

	if ((card->capabilities & MS_SND_CARD_CAP_CAPTURE) == MS_SND_CARD_CAP_CAPTURE) {
		bool hasBuiltingOpenslesAec =
		    (deviceDescription->flags & DEVICE_HAS_BUILTIN_OPENSLES_AEC) == DEVICE_HAS_BUILTIN_OPENSLES_AEC;
		bool hasBuiltinAec = (deviceDescription->flags & DEVICE_HAS_BUILTIN_AEC) == DEVICE_HAS_BUILTIN_AEC;
		bool hasCrappyAec = (deviceDescription->flags & DEVICE_HAS_BUILTIN_AEC_CRAPPY) == DEVICE_HAS_BUILTIN_AEC_CRAPPY;

		if (hasBuiltingOpenslesAec) {
			ms_message("[OpenSLES] Adding MS_SND_CARD_CAP_BUILTIN_ECHO_CANCELLER flag to soundcard ([%s] device ID "
			           "[%0d] type [%s]) because of DEVICE_HAS_BUILTIN_OPENSLES_AEC",
			           card->name, card->internal_id, ms_snd_card_device_type_to_string(card->device_type));
			card->capabilities |= MS_SND_CARD_CAP_BUILTIN_ECHO_CANCELLER;
			card_data->builtin_aec = true;
		}
		// Don't do that anymore since we removed the legacy Java soundcard descriptor in msvoip.c
		/*else if (hasBuiltinAec && !hasCrappyAec && card->device_type == MS_SND_CARD_DEVICE_TYPE_MICROPHONE) {
		    // Only apply this workaround to default microphone, not any other device that has CAPTURE capability
		    ms_warning("[OpenSLES] Removing MS_SND_CARD_CAP_CAPTURE flag from soundcard ([%s] device ID [%0d] type "
		               "[%s]) to use HAEC Java capture soundcard",
		               card->name, card->internal_id, ms_snd_card_device_type_to_string(card->device_type));
		    card->capabilities &= ~MS_SND_CARD_CAP_CAPTURE;
		    card_data->builtin_aec = false;
		} */
	}

	card->latency = deviceDescription->delay;
	if (deviceDescription->recommended_rate) {
		card_data->samplerate = deviceDescription->recommended_rate;
	}
}

static void snd_card_device_create(int device_id,
                                   const char *name,
                                   MSSndCardDeviceType type,
                                   unsigned int capabilities,
                                   SoundDeviceDescription *deviceDescription,
                                   MSSndCardManager *m) {
	MSSndCard *card = ms_snd_card_new(&android_native_snd_opensles_card_desc);
	card = ms_snd_card_ref(card);

	card->name = ms_strdup(name);
	card->internal_id = device_id;
	card->device_type = type;
	card->device_description = deviceDescription;

	// Card capabilities
	card->capabilities = capabilities;
	snd_card_device_create_extra_fields(m, card, deviceDescription);

	bool hasCaptureCapability = (card->capabilities & MS_SND_CARD_CAP_CAPTURE) == MS_SND_CARD_CAP_CAPTURE;
	bool hasPlaybackCapability = (card->capabilities & MS_SND_CARD_CAP_PLAYBACK) == MS_SND_CARD_CAP_PLAYBACK;
	// Last argument is set to false because cards are added based on their type.
	// Their capabilities are ignored as we do upcalls to the audio manager to set the desired device
	// For example a Bluetooth device can be a speaker and/or microphone. The upcall to the audio manager will
	// enable (i.e. start) or disable (i.e. stop) the bluetooth device regardless whether it is used as speaker or
	// microphone.
	MSSndCard *duplicate = ms_snd_card_get_card_duplicate(m, card, FALSE);
	if (duplicate == NULL) {
		if (hasCaptureCapability || hasPlaybackCapability) {
			ms_snd_card_manager_add_card(m, card);
			ms_message("[OpenSLES] Added card [%p]: name [%s] device ID [%0d] type [%s]", card, card->name,
			           card->internal_id, ms_snd_card_device_type_to_string(card->device_type));
		} else {
			ms_warning("[OpenSLES] Card [%p]: name [%s] device ID [%0d] type [%s] not added, neither capture nor "
			           "playback capability",
			           card, card->name, card->internal_id, ms_snd_card_device_type_to_string(card->device_type));
		}
	} else {
		ms_warning("[OpenSLES] Card [%p]: name [%s] device ID [%0d] type [%s] not added, considered a duplicate of "
		           "card [%p]: name [%s] device ID [%0d] type [%s]",
		           card, card->name, card->internal_id, ms_snd_card_device_type_to_string(card->device_type), duplicate,
		           duplicate->name, duplicate->internal_id, ms_snd_card_device_type_to_string(duplicate->device_type));
		if (card->device_type == MS_SND_CARD_DEVICE_TYPE_BLUETOOTH ||
		    card->device_type == MS_SND_CARD_DEVICE_TYPE_HEARING_AID ||
		    card->device_type == MS_SND_CARD_DEVICE_TYPE_HEADSET ||
		    card->device_type == MS_SND_CARD_DEVICE_TYPE_HEADPHONES) {
			if (hasCaptureCapability &&
			    (duplicate->capabilities & MS_SND_CARD_CAP_PLAYBACK) == MS_SND_CARD_CAP_PLAYBACK) {
				ms_message("[OpenSLES] Duplicate card has Playback capability, adding to it the Capture capability");
				duplicate->capabilities |= MS_SND_CARD_CAP_CAPTURE;
			} else if (hasPlaybackCapability &&
			           (duplicate->capabilities & MS_SND_CARD_CAP_CAPTURE) == MS_SND_CARD_CAP_CAPTURE) {
				ms_message("[OpenSLES] Duplicate card has Capture capability, adding to it the Playback capability");
				duplicate->capabilities |= MS_SND_CARD_CAP_PLAYBACK;
			}
			ms_snd_card_unref(duplicate);
		}
	}
	ms_snd_card_unref(card);
}

static void android_snd_card_device_create(JNIEnv *env,
                                           jobject deviceInfo,
                                           SoundDeviceDescription *deviceDescription,
                                           MSSndCardManager *m) {
	MSSndCardDeviceType type = ms_android_get_device_type(env, deviceInfo);
	if ((type == MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_BLUETOOTH) ||
	    (type == MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_EARPIECE) ||
	    (type == MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_SPEAKER) ||
	    (type == MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_MICROPHONE) ||
	    (type == MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_HEADSET) ||
	    (type == MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_HEADPHONES) ||
	    (type == MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_HEARING_AID) ||
	    (type == MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_GENERIC_USB) ||
	    (type == MSSndCardDeviceType::MS_SND_CARD_DEVICE_TYPE_HDMI)) {

		char *name = ms_android_get_device_product_name(env, deviceInfo);
		int device_id = ms_android_get_device_id(env, deviceInfo);
		unsigned int capabilities = ms_android_get_device_capabilities(env, deviceInfo);
		snd_card_device_create(device_id, name, type, capabilities, deviceDescription, m);
		ms_free(name);
	} else {
		ms_message("[OpenSLES] SKipped device with type [%s]", ms_snd_card_device_type_to_string(type));
	}
}
